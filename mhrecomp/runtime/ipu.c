/* IPU (MPEG-2 macroblock decoder) with its two DMA channels, ch3 fromIPU and
 * ch4 toIPU.
 *
 * Commands run on their own host fiber so they can stall exactly where the
 * hardware does: waiting for bitstream data (input FIFO empty, ch4 idle) or
 * for room in the output FIFO (ch3 idle). The EE side resumes the fiber
 * whenever something it could be waiting on changes: a register access, a
 * FIFO write, or a ch3/ch4 start. While a command is stalled the IPU reads
 * as busy, which is what lets the game's sceMpeg "no data" callback run.
 *
 * The input side keeps the hardware's 8+2 quadword FIFO so D4_MADR, IPU_BP
 * (BP, IFC, FP) and IPU_CTRL.IFC agree with each other: sceMpeg saves and
 * restores the stream position as MADR - (IFC+FP)*16 + BP/8.
 *
 * Decoding follows ISO 13818-2 (tables B.1-B.15, inverse quantisation,
 * mismatch control, IDCT) plus the IPU's own command set: BCLR, IDEC, BDEC,
 * VDEC, FDEC, SETIQ, SETVQ, CSC, PACK, SETTH. */
#include "rt.h"
#include <windows.h>
#include <math.h>

u32 *hw_dma_regs(int ch);

enum { CMD_BCLR, CMD_IDEC, CMD_BDEC, CMD_VDEC, CMD_FDEC, CMD_SETIQ, CMD_SETVQ, CMD_CSC, CMD_PACK, CMD_SETTH };
enum { MB_INTRA = 1, MB_PATTERN = 2, MB_BACK = 4, MB_FWD = 8, MB_QUANT = 16 };

#define IN_MAX  10      /* 8-qw FIFO + 2-qw decoder buffer (IPU_BP.FP) */
#define OUT_MAX 8

static struct {
    u8  in[(IN_MAX + 1) * 16];   /* +1 qw padding so 64-bit peeks never leave the array */
    int in_qw, bp;               /* queued quadwords, bit pointer into in[0..15] */
    u8  out[OUT_MAX][16];
    int out_head, out_qw;
    u32 ctrl;                    /* IPU_CTRL bits 14-26 (ECD SCD IDP AS IVF QST MP1 PCT), CBP */
    u32 cmd, data;               /* current command word, IPU_CMD.DATA */
    int busy, abort;
    int wait_in, wait_qw, wait_out;   /* fiber stalled on input (in_qw at the stall) / on a full output FIFO */
    u8  iqm[2][64];              /* quantiser matrices in raster order: [0] intra, [1] non-intra */
    u16 vqclut[16];
    u32 th0, th1;
    int dc_pred[3];
    int d4_need_tag;             /* ch4 chain: read a tag before the next transfer */
    u32 irq;                     /* pending: bit 3/4 DMAC ch3/ch4 done, bit 8 IPU interrupt */
    u64 ncmd[10];
} ipu;

static void *ipu_fiber, *ipu_caller;
static u32 ctrl_value(void);
static int in_fiber;

/* ---- VLC tables --------------------------------------------------------- */
typedef struct { const char *code; int v; } vlc_def;
/* lookup on the next 16 bits: (len << 16) | (u16)value, 0 = invalid */
static u32 lut_mbai[65536], lut_mbt[5][65536], lut_cbp[65536], lut_mc[65536], lut_dmv[65536];
static u32 lut_dcl[65536], lut_dcc[65536], lut_b14[65536], lut_b15[65536];

#define V_EOB 0xffff
#define V_ESC 0xfffe
#define RL(r, l) (((r) << 8) | (l))

static const vlc_def t_mbai[] = {
    {"1",1},{"011",2},{"010",3},{"0011",4},{"0010",5},{"00011",6},{"00010",7},{"0000111",8},
    {"0000110",9},{"00001011",10},{"00001010",11},{"00001001",12},{"00001000",13},{"00000111",14},
    {"00000110",15},{"0000010111",16},{"0000010110",17},{"0000010101",18},{"0000010100",19},
    {"0000010011",20},{"0000010010",21},{"00000100011",22},{"00000100010",23},{"00000100001",24},
    {"00000100000",25},{"00000011111",26},{"00000011110",27},{"00000011101",28},{"00000011100",29},
    {"00000011011",30},{"00000011010",31},{"00000011001",32},{"00000011000",33},
    {"00000001000",0x23},   /* macroblock_escape */
    {"00000001111",0x22},   /* macroblock_stuffing (MPEG-1) */
    {0,0}};
static const vlc_def t_mbt_i[] = {{"1",0x01},{"01",0x11},{0,0}};
static const vlc_def t_mbt_p[] = {{"1",0x0a},{"01",0x02},{"001",0x08},{"00011",0x01},{"00010",0x1a},
    {"00001",0x12},{"000001",0x11},{0,0}};
static const vlc_def t_mbt_b[] = {{"10",0x0c},{"11",0x0e},{"010",0x04},{"011",0x06},{"0010",0x08},
    {"0011",0x0a},{"00011",0x01},{"00010",0x1e},{"000011",0x1a},{"000010",0x16},{"000001",0x11},{0,0}};
static const vlc_def t_mbt_d[] = {{"1",0x01},{0,0}};
static const vlc_def t_cbp[] = {
    {"111",60},{"1101",4},{"1100",8},{"1011",16},{"1010",32},{"10011",12},{"10010",48},{"10001",20},
    {"10000",40},{"01111",28},{"01110",44},{"01101",52},{"01100",56},{"01011",1},{"01010",61},
    {"01001",2},{"01000",62},{"001111",24},{"001110",36},{"001101",3},{"001100",63},{"0010111",5},
    {"0010110",9},{"0010101",17},{"0010100",33},{"0010011",6},{"0010010",10},{"0010001",18},
    {"0010000",34},{"00011111",7},{"00011110",11},{"00011101",19},{"00011100",35},{"00011011",13},
    {"00011010",49},{"00011001",21},{"00011000",41},{"00010111",14},{"00010110",50},{"00010101",22},
    {"00010100",42},{"00010011",15},{"00010010",51},{"00010001",23},{"00010000",43},{"00001111",25},
    {"00001110",37},{"00001101",26},{"00001100",38},{"00001011",29},{"00001010",45},{"00001001",53},
    {"00001000",57},{"00000111",30},{"00000110",46},{"00000101",54},{"00000100",58},{"000000111",31},
    {"000000110",47},{"000000101",55},{"000000100",59},{"000000011",27},{"000000010",39},
    {"000000001",0},{0,0}};
/* motion_code magnitudes; the sign bit follows every non-zero code */
static const vlc_def t_mc[] = {
    {"1",0},{"01",1},{"001",2},{"0001",3},{"000011",4},{"0000101",5},{"0000100",6},{"0000011",7},
    {"000001011",8},{"000001010",9},{"000001001",10},{"0000010001",11},{"0000010000",12},
    {"0000001111",13},{"0000001110",14},{"0000001101",15},{"0000001100",16},{0,0}};
static const vlc_def t_dmv[] = {{"0",0},{"10",1},{"11",-1},{0,0}};
static const vlc_def t_dcl[] = {{"100",0},{"00",1},{"01",2},{"101",3},{"110",4},{"1110",5},{"11110",6},
    {"111110",7},{"1111110",8},{"11111110",9},{"111111110",10},{"111111111",11},{0,0}};
static const vlc_def t_dcc[] = {{"00",0},{"01",1},{"10",2},{"110",3},{"1110",4},{"11110",5},{"111110",6},
    {"1111110",7},{"11111110",8},{"111111110",9},{"1111111110",10},{"1111111111",11},{0,0}};
/* B.14 (sign bit not included). "11" is (0,1) for coefficients after the
 * first; the first coefficient of a non-intra block uses "1" (handled in code). */
#define DCT_COMMON \
    {"00000000011111",RL(0,16)},{"00000000011110",RL(0,17)},{"00000000011101",RL(0,18)},{"00000000011100",RL(0,19)}, \
    {"00000000011011",RL(0,20)},{"00000000011010",RL(0,21)},{"00000000011001",RL(0,22)},{"00000000011000",RL(0,23)}, \
    {"00000000010111",RL(0,24)},{"00000000010110",RL(0,25)},{"00000000010101",RL(0,26)},{"00000000010100",RL(0,27)}, \
    {"00000000010011",RL(0,28)},{"00000000010010",RL(0,29)},{"00000000010001",RL(0,30)},{"00000000010000",RL(0,31)}, \
    {"000000000011000",RL(0,32)},{"000000000010111",RL(0,33)},{"000000000010110",RL(0,34)},{"000000000010101",RL(0,35)}, \
    {"000000000010100",RL(0,36)},{"000000000010011",RL(0,37)},{"000000000010010",RL(0,38)},{"000000000010001",RL(0,39)}, \
    {"000000000010000",RL(0,40)},{"000000000011111",RL(1,8)},{"000000000011110",RL(1,9)},{"000000000011101",RL(1,10)}, \
    {"000000000011100",RL(1,11)},{"000000000011011",RL(1,12)},{"000000000011010",RL(1,13)},{"000000000011001",RL(1,14)}, \
    {"0000000000010011",RL(1,15)},{"0000000000010010",RL(1,16)},{"0000000000010001",RL(1,17)},{"0000000000010000",RL(1,18)}, \
    {"0000000000010100",RL(6,3)},{"0000000000011010",RL(11,2)},{"0000000000011001",RL(12,2)},{"0000000000011000",RL(13,2)}, \
    {"0000000000010111",RL(14,2)},{"0000000000010110",RL(15,2)},{"0000000000010101",RL(16,2)},{"0000000000011111",RL(27,1)}, \
    {"0000000000011110",RL(28,1)},{"0000000000011101",RL(29,1)},{"0000000000011100",RL(30,1)},{"0000000000011011",RL(31,1)}, \
    {"0000000010110",RL(1,6)},{"0000000010101",RL(1,7)},{"0000000010100",RL(2,5)},{"0000000010011",RL(3,4)}, \
    {"0000000010010",RL(5,3)},{"0000000010001",RL(9,2)},{"0000000010000",RL(10,2)},{"0000000011111",RL(22,1)}, \
    {"0000000011110",RL(23,1)},{"0000000011101",RL(24,1)},{"0000000011100",RL(25,1)},{"0000000011011",RL(26,1)}, \
    {"000001",V_ESC}
static const vlc_def t_b14[] = {
    {"10",V_EOB},{"11",RL(0,1)},{"011",RL(1,1)},{"0100",RL(0,2)},{"0101",RL(2,1)},{"00101",RL(0,3)},
    {"00111",RL(3,1)},{"00110",RL(4,1)},{"000110",RL(1,2)},{"000111",RL(5,1)},{"000101",RL(6,1)},
    {"000100",RL(7,1)},{"0000110",RL(0,4)},{"0000100",RL(2,2)},{"0000111",RL(8,1)},{"0000101",RL(9,1)},
    {"00100110",RL(0,5)},{"00100001",RL(0,6)},{"00100101",RL(1,3)},{"00100100",RL(3,2)},{"00100111",RL(10,1)},
    {"00100011",RL(11,1)},{"00100010",RL(12,1)},{"00100000",RL(13,1)},{"0000001010",RL(0,7)},
    {"0000001100",RL(1,4)},{"0000001011",RL(2,3)},{"0000001111",RL(4,2)},{"0000001001",RL(5,2)},
    {"0000001110",RL(14,1)},{"0000001101",RL(15,1)},{"0000001000",RL(16,1)},{"000000011101",RL(0,8)},
    {"000000011000",RL(0,9)},{"000000010011",RL(0,10)},{"000000010000",RL(0,11)},{"000000011011",RL(1,5)},
    {"000000010100",RL(2,4)},{"000000011100",RL(3,3)},{"000000010010",RL(4,3)},{"000000011110",RL(6,2)},
    {"000000010101",RL(7,2)},{"000000010001",RL(8,2)},{"000000011111",RL(17,1)},{"000000011010",RL(18,1)},
    {"000000011001",RL(19,1)},{"000000010111",RL(20,1)},{"000000010110",RL(21,1)},{"0000000011010",RL(0,12)},
    {"0000000011001",RL(0,13)},{"0000000011000",RL(0,14)},{"0000000010111",RL(0,15)},
    DCT_COMMON, {0,0}};
static const vlc_def t_b15[] = {
    {"0110",V_EOB},{"10",RL(0,1)},{"010",RL(1,1)},{"110",RL(0,2)},{"00101",RL(2,1)},{"0111",RL(0,3)},
    {"00111",RL(3,1)},{"000110",RL(4,1)},{"00110",RL(1,2)},{"000111",RL(5,1)},{"0000110",RL(6,1)},
    {"0000100",RL(7,1)},{"11100",RL(0,4)},{"0000111",RL(2,2)},{"0000101",RL(8,1)},{"1111000",RL(9,1)},
    {"11101",RL(0,5)},{"000101",RL(0,6)},{"1111001",RL(1,3)},{"00100110",RL(3,2)},{"1111010",RL(10,1)},
    {"00100001",RL(11,1)},{"00100101",RL(12,1)},{"00100100",RL(13,1)},{"000100",RL(0,7)},
    {"00100111",RL(1,4)},{"11111100",RL(2,3)},{"11111101",RL(4,2)},{"000000100",RL(5,2)},
    {"000000101",RL(14,1)},{"000000111",RL(15,1)},{"0000001101",RL(16,1)},{"1111011",RL(0,8)},
    {"1111100",RL(0,9)},{"00100011",RL(0,10)},{"00100010",RL(0,11)},{"00100000",RL(1,5)},
    {"0000001100",RL(2,4)},{"000000011100",RL(3,3)},{"000000010010",RL(4,3)},{"000000011110",RL(6,2)},
    {"000000010101",RL(7,2)},{"000000010001",RL(8,2)},{"000000011111",RL(17,1)},{"000000011010",RL(18,1)},
    {"000000011001",RL(19,1)},{"000000010111",RL(20,1)},{"000000010110",RL(21,1)},{"11111010",RL(0,12)},
    {"11111011",RL(0,13)},{"11111110",RL(0,14)},{"11111111",RL(0,15)},
    DCT_COMMON, {0,0}};

static int vlc_build(u32 *lut, const vlc_def *t, const char *name) {
    int bad = 0;
    for (; t->code; t++) {
        int len = 0; u32 code = 0;
        for (const char *c = t->code; *c; c++) { code = code << 1 | (*c == '1'); len++; }
        u32 lo = code << (16 - len), n = 1u << (16 - len);
        for (u32 k = 0; k < n; k++) {
            if (lut[lo + k]) { if (!bad) LOG("ipu: VLC table %s: code %s overlaps\n", name, t->code); bad = 1; }
            lut[lo + k] = (u32)len << 16 | (u16)t->v;
        }
    }
    return bad;
}

static const u8 zigzag[64] = {
     0, 1, 8,16, 9, 2, 3,10,17,24,32,25,18,11, 4, 5,12,19,26,33,40,48,41,34,27,20,13, 6, 7,14,21,28,
    35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63 };
static const u8 altscan[64] = {
     0, 8,16,24, 1, 9, 2,10,17,25,32,40,48,56,57,49,41,33,26,18, 3,11, 4,12,19,27,34,42,50,58,35,43,
    51,59,20,28, 5,13, 6,14,21,29,36,44,52,60,37,45,53,61,22,30, 7,15,23,31,38,46,54,62,39,47,55,63 };
static const u8 qscale_nl[32] = { 0,1,2,3,4,5,6,7,8,10,12,14,16,18,20,22,24,28,32,36,40,44,48,52,56,64,72,80,88,96,104,112 };
static const u8 default_intra[64] = {      /* zigzag order, as SETIQ receives it */
     8,16,16,19,16,19,22,22,22,22,22,22,26,24,26,27,27,27,26,26,26,26,27,27,27,29,29,29,34,34,34,29,
    29,29,27,27,29,29,32,32,34,34,37,38,37,35,35,34,35,38,38,40,40,40,48,48,46,46,56,56,58,69,69,83 };
static double idct_c[8][8];

void ipu_init(void) {
    int bad = 0;
    bad |= vlc_build(lut_mbai, t_mbai, "mbai");
    bad |= vlc_build(lut_mbt[1], t_mbt_i, "mbt_i");
    bad |= vlc_build(lut_mbt[2], t_mbt_p, "mbt_p");
    bad |= vlc_build(lut_mbt[3], t_mbt_b, "mbt_b");
    bad |= vlc_build(lut_mbt[4], t_mbt_d, "mbt_d");
    bad |= vlc_build(lut_cbp, t_cbp, "cbp");
    bad |= vlc_build(lut_mc, t_mc, "motion");
    bad |= vlc_build(lut_dmv, t_dmv, "dmv");
    bad |= vlc_build(lut_dcl, t_dcl, "dc_luma");
    bad |= vlc_build(lut_dcc, t_dcc, "dc_chroma");
    bad |= vlc_build(lut_b14, t_b14, "B14");
    bad |= vlc_build(lut_b15, t_b15, "B15");
    if (bad) LOG("ipu: VLC tables are inconsistent\n");
    for (int i = 0; i < 64; i++) { ipu.iqm[0][zigzag[i]] = default_intra[i]; ipu.iqm[1][i] = 16; }
    for (int x = 0; x < 8; x++)
        for (int u = 0; u < 8; u++)
            idct_c[x][u] = (u ? 0.5 : 0.5 / sqrt(2.0)) * cos((2 * x + 1) * u * M_PI / 16.0);
}

/* ---- DMA channels ------------------------------------------------------- */
static u8 *dma_mem(u32 a) {
    if ((a & 0x80000000u) || (a >> 28) == 7) return ee_spr + (a & 0x3ff0);
    return ee_ram + (a & 0x01fffff0);
}

/* Move ch4 data into the input FIFO until it is full or the channel stops. */
static void in_fill(void) {
    u32 *r = hw_dma_regs(4);
    while (ipu.in_qw < IN_MAX && (r[0] & 0x100)) {
        if ((r[2] & 0xffff) == 0) {
            int chain = ((r[0] >> 2) & 3) == 1;
            if (!chain) goto stop;
            if (!ipu.d4_need_tag) {
                u32 id = (r[0] >> 28) & 7;
                if (id == 0 || id == 7 || ((r[0] >> 31) && (r[0] & 0x80))) goto stop;   /* refe/end/IRQ+TIE */
            }
            ipu.d4_need_tag = 0;
            u32 tadr = r[3];
            u64 tag; memcpy(&tag, dma_mem(tadr), 8);
            u32 qwc = tag & 0xffff, id = (tag >> 28) & 7, addr = (u32)(tag >> 32) & 0xfffffff0u;
            if (tag >> 63) addr |= 0x80000000u;
            r[0] = (r[0] & 0xffff) | ((u32)tag & 0xffff0000u);
            switch (id) {
            case 0: case 3: case 4: r[1] = addr; r[3] = tadr + 16; break;                /* refe ref refs */
            case 1: r[1] = tadr + 16; r[3] = tadr + 16 + qwc * 16; break;               /* cnt */
            case 2: r[1] = tadr + 16; r[3] = addr; break;                                /* next */
            case 7: r[1] = tadr + 16; r[3] = tadr + 16 + qwc * 16; break;               /* end */
            default:
                LOG("ipu: toIPU chain tag id %u unsupported\n", id);
                r[1] = tadr + 16; r[3] = tadr + 16 + qwc * 16; break;
            }
            r[2] = qwc;
            continue;
        }
        memcpy(ipu.in + ipu.in_qw * 16, dma_mem(r[1]), 16);
        ipu.in_qw++;
        r[1] += 16; r[2] = (r[2] & ~0xffffu) | ((r[2] - 1) & 0xffff);
        continue;
    stop:
        r[0] &= ~0x100u;
        ipu.irq |= 1u << 4;
        break;
    }
}

/* Push the output FIFO into memory while ch3 is running. */
static void out_drain(void) {
    u32 *r = hw_dma_regs(3);
    while (ipu.out_qw && (r[0] & 0x100) && (r[2] & 0xffff)) {
        memcpy(dma_mem(r[1]), ipu.out[ipu.out_head], 16);
        ipu.out_head = (ipu.out_head + 1) % OUT_MAX; ipu.out_qw--;
        r[1] += 16; r[2] = (r[2] & ~0xffffu) | ((r[2] - 1) & 0xffff);
        if ((r[2] & 0xffff) == 0) { r[0] &= ~0x100u; ipu.irq |= 1u << 3; }
    }
}

/* ---- fiber plumbing ----------------------------------------------------- */
static void yield(void) {
    SwitchToFiber(ipu_caller);
}

static void need_bits(int n) {
    while (ipu.in_qw * 128 - ipu.bp < n) {
        int before = ipu.in_qw;
        in_fill();
        if (ipu.in_qw == before) { ipu.wait_in = 1; ipu.wait_qw = ipu.in_qw; yield(); ipu.wait_in = 0; }
    }
}
static u32 peek(int n) {          /* n <= 32 */
    if (n == 0) return 0;
    need_bits(n);
    int byte = ipu.bp >> 3, sh = ipu.bp & 7;
    u64 v = 0;
    for (int k = 0; k < 8; k++) v = v << 8 | ipu.in[byte + k];
    return (u32)((v << sh) >> (64 - n));
}
/* Like peek but never waits: bits past the end of the data read as 0. */
static u32 peek_avail(int n) {
    in_fill();
    int byte = ipu.bp >> 3, sh = ipu.bp & 7;
    u64 v = 0;
    for (int k = 0; k < 8; k++) v = v << 8 | ((byte + k) < ipu.in_qw * 16 ? ipu.in[byte + k] : 0);
    return (u32)((v << sh) >> (64 - n));
}
static void skip(int n) {
    need_bits(n);
    ipu.bp += n;
    while (ipu.bp >= 128) {
        memmove(ipu.in, ipu.in + 16, (ipu.in_qw - 1) * 16);
        ipu.in_qw--; ipu.bp -= 128;
    }
    in_fill();
}
static u32 bits(int n) { u32 v = peek(n); skip(n); return v; }
static s32 sbits(int n) { return n ? (s32)(bits(n) << (32 - n)) >> (32 - n) : 0; }

/* Decode one VLC; returns the value, sets *len (0 = invalid code). */
static int vlc(const u32 *lut, int *len) {
    for (;;) {
        int avail = ipu.in_qw * 128 - ipu.bp;
        u32 e = lut[peek_avail(16)];
        /* bits past the data read as 0: a code is only trusted once all its bits are real */
        if (avail >= 16 || (e && (int)(e >> 16) <= avail)) {
            *len = e >> 16;
            if (e) skip(*len);
            return e ? (s16)(e & 0xffff) : 0;
        }
        need_bits(avail + 1);
    }
}

static void out_qw(const void *p) {
    for (;;) {
        out_drain();
        if (ipu.out_qw < OUT_MAX) break;
        ipu.wait_out = 1; yield(); ipu.wait_out = 0;
    }
    memcpy(ipu.out[(ipu.out_head + ipu.out_qw) % OUT_MAX], p, 16);
    ipu.out_qw++;
    out_drain();
}
static void out_bytes(const u8 *p, int n) { for (int k = 0; k < n; k += 16) out_qw(p + k); }

/* ---- block decoding ----------------------------------------------------- */
static int qscale(int code) { return (ipu.ctrl >> 22 & 1) ? qscale_nl[code & 31] : (code & 31) * 2; }
#define IDP   ((ipu.ctrl >> 16) & 3)
#define AS    ((ipu.ctrl >> 20) & 1)
#define IVF   ((ipu.ctrl >> 21) & 1)
#define MP1   ((ipu.ctrl >> 23) & 1)
#define PCT   ((ipu.ctrl >> 24) & 7)

static void error(const char *what) {
    ipu.ctrl |= 1u << 14;          /* ECD */
    if (g_log_io) LOG("ipu: decode error: %s\n", what);
}

/* Read the AC/DC coefficients of one block into blk (raster order, dequantised). */
static int read_block(s32 *blk, int intra, int cc, int qs) {
    memset(blk, 0, 64 * sizeof *blk);
    const u8 *scan = AS ? altscan : zigzag;
    const u8 *w = ipu.iqm[intra ? 0 : 1];
    const u32 *lut = (intra && IVF) ? lut_b15 : lut_b14;
    int i = 0, len, sum = 0;
    if (intra) {
        int sz = vlc(cc ? lut_dcc : lut_dcl, &len);
        if (!len) { error("dc size"); return 0; }
        int diff = 0;
        if (sz) {
            diff = bits(sz);
            if (!(diff >> (sz - 1))) diff -= (1 << sz) - 1;
        }
        ipu.dc_pred[cc] += diff;
        blk[0] = ipu.dc_pred[cc] << (3 - IDP);
        sum = blk[0];
        i = 1;
    } else if (peek(1)) {           /* first coefficient: "1s" is (0,1) */
        skip(1);
        int s = bits(1);
        int v = ((2 * 1 + 1) * w[scan[0]] * qs) / 32;
        if (MP1) v = (v - 1) | 1;
        blk[scan[0]] = s ? -v : v;
        sum = blk[scan[0]];
        i = 1;
    }
    for (;;) {
        int e = vlc(lut, &len);
        if (!len) { error("dct coefficient"); return 0; }
        int run, level, neg;
        if ((u16)e == V_EOB) break;
        if ((u16)e == V_ESC) {
            run = bits(6);
            if (MP1) {
                level = sbits(8);
                if (level == -128) level = (s32)bits(8) - 256;
                else if (level == 0) level = bits(8);
            } else level = sbits(12);
            neg = level < 0; if (neg) level = -level;
        } else {
            run = (e >> 8) & 0xff; level = e & 0xff; neg = bits(1);
        }
        i += run;
        if (i > 63) { error("run past block end"); return 0; }
        int p = scan[i], v;
        if (intra) v = (level * w[p] * qs) / 16;
        else       v = ((2 * level + 1) * w[p] * qs) / 32;
        if (MP1 && v) v = (v - 1) | 1;
        if (v > 2047 + neg) v = 2047 + neg;
        blk[p] = neg ? -v : v;
        sum += blk[p];
        i++;
    }
    if (!MP1 && !(sum & 1)) blk[63] ^= 1;       /* mismatch control */
    return 1;
}

static void idct(const s32 *in, s16 *out, int stride, int lo, int hi) {
    double t[64];
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            double s = 0;
            for (int u = 0; u < 8; u++) s += idct_c[x][u] * in[y * 8 + u];
            t[y * 8 + x] = s;
        }
    for (int x = 0; x < 8; x++)
        for (int y = 0; y < 8; y++) {
            double s = 0;
            for (int v = 0; v < 8; v++) s += idct_c[y][v] * t[v * 8 + x];
            int r = (int)floor(s + 0.5);
            out[y * stride + x] = (s16)(r < lo ? lo : r > hi ? hi : r);
        }
}

/* One macroblock of blocks into a RAW16 macroblock (Y 16x16, Cb 8x8, Cr 8x8). */
static int decode_mb(s16 *mb, int intra, int cbp, int dct_field, int qs) {
    s32 blk[64];
    memset(mb, 0, 384 * 2);
    for (int b = 0; b < 6; b++) {
        if (!(cbp & (32 >> b))) continue;
        int cc = b < 4 ? 0 : b - 3;
        if (!read_block(blk, intra, cc, qs)) return 0;
        s16 *dst; int stride;
        if (b < 4) {
            if (dct_field) { dst = mb + (b >> 1) * 16 + (b & 1) * 8; stride = 32; }
            else           { dst = mb + (b >> 1) * 128 + (b & 1) * 8; stride = 16; }
        } else { dst = mb + 256 + (b - 4) * 64; stride = 8; }
        if (intra) idct(blk, dst, stride, 0, 255);
        else       idct(blk, dst, stride, -256, 255);
    }
    return 1;
}

static void dc_reset(void) { ipu.dc_pred[0] = ipu.dc_pred[1] = ipu.dc_pred[2] = 128 << IDP; }

/* ---- colour space conversion -------------------------------------------- */
static u8 clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

static void csc_mb(const u8 *y, const u8 *cb, const u8 *cr, int rgb16, int dither) {
    static const int dith[4][4] = { {-4,0,-3,1}, {2,-2,3,-1}, {-3,1,-4,0}, {3,-1,2,-2} };
    u32 rgba[256];
    for (int py = 0; py < 16; py++)
        for (int px = 0; px < 16; px++) {
            int Y = y[py * 16 + px] - 16, U = cb[(py >> 1) * 8 + (px >> 1)] - 128, V = cr[(py >> 1) * 8 + (px >> 1)] - 128;
            int r = (298 * Y + 409 * V + 128) >> 8;
            int g = (298 * Y - 100 * U - 208 * V + 128) >> 8;
            int b = (298 * Y + 516 * U + 128) >> 8;
            if (rgb16 && dither) { int d = dith[py & 3][px & 3]; r += d; g += d; b += d; }
            u8 R = clamp8(r), G = clamp8(g), B = clamp8(b), A = 0x80;
            if (R < ipu.th0 && G < ipu.th0 && B < ipu.th0) R = G = B = A = 0;
            else if (R < ipu.th1 && G < ipu.th1 && B < ipu.th1) A = 0x40;
            rgba[py * 16 + px] = R | G << 8 | B << 16 | (u32)A << 24;
        }
    if (!rgb16) { out_bytes((const u8 *)rgba, 1024); return; }
    u16 o[256];
    for (int k = 0; k < 256; k++) {
        u32 c = rgba[k];
        o[k] = (u16)(((c >> 3) & 0x1f) | (((c >> 11) & 0x1f) << 5) | (((c >> 19) & 0x1f) << 10) | ((c >> 31) << 15));
    }
    out_bytes((const u8 *)o, 512);
}

static void raw16_to_raw8(const s16 *mb, u8 *y, u8 *cb, u8 *cr) {
    for (int k = 0; k < 256; k++) y[k] = clamp8(mb[k]);
    for (int k = 0; k < 64; k++) { cb[k] = clamp8(mb[256 + k]); cr[k] = clamp8(mb[320 + k]); }
}

/* ---- commands ------------------------------------------------------------ */
static void set_scd(void) {
    if ((peek_avail(24)) == 1) ipu.ctrl |= 1u << 15; else ipu.ctrl &= ~(1u << 15);
}

static void cmd_vdec(u32 c) {
    skip(c & 0x3f);
    int tbl = (c >> 26) & 3, len = 0, v = 0;
    switch (tbl) {
    case 0: v = vlc(lut_mbai, &len); break;
    case 1: {
        int pct = PCT;
        if (pct < 1 || pct > 4) { error("bad picture type"); break; }
        v = vlc(lut_mbt[pct], &len);
        break; }
    case 2:
        v = vlc(lut_mc, &len);
        if (len && v) { if (bits(1)) v = -v; len++; }
        break;
    case 3: v = vlc(lut_dmv, &len); break;
    }
    if (!len) { if (!(ipu.ctrl & (1u << 14))) error("vdec code"); ipu.data = 0; }
    else ipu.data = (u16)v | (u32)len << 16;
    set_scd();
}

static void cmd_bdec(u32 c) {
    skip(c & 0x3f);
    int qs = qscale((c >> 16) & 31), field = (c >> 25) & 1, dcr = (c >> 26) & 1, intra = (c >> 27) & 1;
    int cbp = 0x3f, len;
    if (dcr) dc_reset();
    if (!intra) {
        cbp = vlc(lut_cbp, &len);
        if (!len) { error("cbp"); return; }
    }
    ipu.ctrl = (ipu.ctrl & ~0x3f00u) | (u32)cbp << 8;
    s16 mb[384];
    if (!decode_mb(mb, intra, cbp, field, qs)) return;
    out_bytes((const u8 *)mb, 768);
    set_scd();
}

static void cmd_idec(u32 c) {
    skip(c & 0x3f);
    int qsc = (c >> 16) & 31, dtd = (c >> 24) & 1, sgn = (c >> 25) & 1, dte = (c >> 26) & 1, ofm = (c >> 27) & 1;
    (void)sgn;
    dc_reset();
    for (int first = 1;; first = 0) {
        int len, inc = 0;
        for (;;) {                      /* macroblock_address_increment with escapes */
            int v = vlc(lut_mbai, &len);
            if (!len) { error("idec mbai"); return; }
            if (v == 0x23) { inc += 33; continue; }
            if (v == 0x22) continue;
            inc += v; break;
        }
        (void)first; (void)inc;
        int mbt = vlc(lut_mbt[1], &len);
        if (!len) { error("idec mb type"); return; }
        int field = dtd ? bits(1) : 0;
        if (mbt & MB_QUANT) qsc = bits(5);
        s16 mb[384]; u8 y[256], cb[64], cr[64];
        if (!decode_mb(mb, 1, 0x3f, field, qscale(qsc))) return;
        raw16_to_raw8(mb, y, cb, cr);
        csc_mb(y, cb, cr, ofm, dte);
        if (peek(23) == 0) break;         /* start code follows: end of slice */
    }
    set_scd();
}

/* Debug: IPU_DUMP=<dir> writes every IPU_DUMP_EVERY-th CSC picture as a PPM.
 * The picture is assumed to be one CSC command, macroblocks in raster order,
 * 2:1 in macroblocks (true for the 256x128 and 512x256 movies seen so far). */
static void dump_csc_picture(const u8 *mbs, int n) {
    static int count; static const char *dir; static int every;
    if (!dir) { dir = getenv("IPU_DUMP"); if (!dir) dir = ""; const char *e = getenv("IPU_DUMP_EVERY"); every = e ? atoi(e) : 30; if (every < 1) every = 1; }
    if (!*dir || n < 2) return;
    if (count++ % every) return;
    int cols = 1; while (cols * cols < n * 2) cols++;
    if (n % cols) return;
    int rows = n / cols, w = cols * 16, h = rows * 16;
    char path[512]; snprintf(path, sizeof path, "%s/csc_%05d_%dx%d.ppm", dir, count - 1, w, h);
    FILE *f = fopen(path, "wb"); if (!f) return;
    fprintf(f, "P6 %d %d 255\n", w, h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const u8 *mb = mbs + ((x >> 4) * rows + (y >> 4)) * 384;   /* sceMpeg: column-major */
            int Y = mb[(y & 15) * 16 + (x & 15)], cb = mb[256 + (y & 15) / 2 * 8 + (x & 15) / 2] - 128, cr = mb[320 + (y & 15) / 2 * 8 + (x & 15) / 2] - 128;
            double yy = 1.164 * (Y - 16);
            int rgb[3] = { (int)(yy + 1.596 * cr), (int)(yy - 0.391 * cb - 0.813 * cr), (int)(yy + 2.018 * cb) };
            for (int k = 0; k < 3; k++) fputc(rgb[k] < 0 ? 0 : rgb[k] > 255 ? 255 : rgb[k], f);
        }
    fclose(f);
}

static void cmd_csc(u32 c) {
    int n = c & 0x7ff, dte = (c >> 26) & 1, ofm = (c >> 27) & 1;
    static u8 pic[2048 * 384];
    for (int m = 0; m < n; m++) {
        u8 raw[384];
        for (int k = 0; k < 384; k++) raw[k] = (u8)bits(8);
        memcpy(pic + m * 384, raw, 384);
        csc_mb(raw, raw + 256, raw + 320, ofm, dte);
    }
    dump_csc_picture(pic, n);
}

static void cmd_pack(u32 c) {
    int n = c & 0x7ff, dte = (c >> 26) & 1, ofm = (c >> 27) & 1;
    (void)dte;
    for (int m = 0; m < n; m++) {
        u32 px[256];
        for (int k = 0; k < 256; k++) {
            u32 v = 0;
            for (int b = 0; b < 4; b++) v |= bits(8) << (8 * b);
            px[k] = v;
        }
        if (ofm) {                      /* RGB16 */
            u16 o[256];
            for (int k = 0; k < 256; k++) {
                u32 p = px[k];
                o[k] = (u16)(((p >> 3) & 0x1f) | (((p >> 11) & 0x1f) << 5) | (((p >> 19) & 0x1f) << 10) | ((p >> 31) << 15));
            }
            out_bytes((const u8 *)o, 512);
        } else {                        /* INDX4 via VQ CLUT: nearest colour */
            u8 o[128];
            for (int k = 0; k < 256; k++) {
                u32 p = px[k]; int best = 0, bd = 1 << 30;
                for (int e = 0; e < 16; e++) {
                    u16 q = ipu.vqclut[e];
                    int dr = (int)((p >> 3) & 31) - (q & 31), dg = (int)((p >> 11) & 31) - ((q >> 5) & 31), db = (int)((p >> 19) & 31) - ((q >> 10) & 31);
                    int d = dr * dr + dg * dg + db * db;
                    if (d < bd) { bd = d; best = e; }
                }
                if (k & 1) o[k >> 1] |= best << 4; else o[k >> 1] = best;
            }
            out_bytes(o, 128);
        }
    }
}

static void execute(u32 c) {
    int op = c >> 28;
    if (op < 10) ipu.ncmd[op]++;
    switch (op) {
    case CMD_IDEC: cmd_idec(c); break;
    case CMD_BDEC: cmd_bdec(c); break;
    case CMD_VDEC: cmd_vdec(c); break;
    case CMD_FDEC:
        skip(c & 0x3f);
        ipu.data = peek(32);
        set_scd();
        break;
    case CMD_SETIQ: {
        skip(c & 0x3f);
        int m = (c >> 27) & 1;
        for (int i = 0; i < 64; i++) ipu.iqm[m][zigzag[i]] = (u8)bits(8);
        break; }
    case CMD_SETVQ:
        for (int i = 0; i < 16; i++) { u32 lo = bits(8), hi = bits(8); ipu.vqclut[i] = (u16)(lo | hi << 8); }
        break;
    case CMD_CSC: cmd_csc(c); break;
    case CMD_PACK: cmd_pack(c); break;
    case CMD_SETTH: ipu.th0 = c & 0x1ff; ipu.th1 = (c >> 16) & 0x1ff; break;
    default: LOG("ipu: unknown command %08x\n", c);
    }
}

int g_log_ipu;

static void WINAPI ipu_main(void *p) {
    (void)p;
    for (;;) {
        execute(ipu.cmd);
        if (g_log_ipu > 1 || (g_log_ipu && ipu.cmd != 0x40000008u))
            LOG("ipu: %08x -> %08x ctrl %08x\n", ipu.cmd, ipu.data, ctrl_value());
        ipu.busy = 0;
        ipu.irq |= 1u << 8;
        yield();
    }
}

/* Resume a running command until it finishes or stalls, then deliver interrupts. */
static void run(void) {
    if (in_fiber) return;
    if (ipu.busy && ipu_fiber && (ipu.wait_in || ipu.wait_out)) {
        /* the decoder is stalled: switch to its fiber only when that can make
           progress (a fiber switch per IPU poll cost ~170 ms at movie starts) */
        in_fill(); out_drain();
        if (ipu.wait_in ? ipu.in_qw == ipu.wait_qw : ipu.out_qw >= OUT_MAX) goto irqs;
    }
    if (ipu.busy) {
        if (!ipu_fiber) ipu_fiber = CreateFiberEx(64 << 10, 1 << 20, FIBER_FLAG_FLOAT_SWITCH, ipu_main, 0);
        in_fiber = 1;
        ipu_caller = GetCurrentFiber();
        SwitchToFiber(ipu_fiber);
        in_fiber = 0;
    } else {
        in_fill();
        out_drain();
    }
irqs:;
    u32 irq = ipu.irq; ipu.irq = 0;
    if (irq & (1u << 3)) dmac_raise(3);
    if (irq & (1u << 4)) dmac_raise(4);
    if (irq & (1u << 8)) intc_raise(8);
}

static void reset(void) {
    if (ipu_fiber) { DeleteFiber(ipu_fiber); ipu_fiber = 0; }
    ipu.busy = 0; ipu.wait_in = ipu.wait_out = 0; ipu.in_qw = 0; ipu.bp = 0; ipu.out_qw = 0; ipu.out_head = 0;
    ipu.ctrl = 0; ipu.data = 0;
}

/* ---- EE interface -------------------------------------------------------- */
u64 g_ipu_last_frame;   /* frame of the newest IPU command (a movie is playing) */
void ipu_write_cmd(u32 v) {
    g_ipu_last_frame = g_frame;
    if (ipu.busy) { LOG("ipu: command %08x written while busy with %08x\n", v, ipu.cmd); return; }
    ipu.ctrl &= ~(3u << 14);         /* ECD, SCD */
    if ((v >> 28) == CMD_BCLR) {
        ipu.ncmd[0]++;
        ipu.in_qw = 0; ipu.bp = v & 0x7f;
        if (g_log_ipu) LOG("ipu: BCLR bp=%d\n", ipu.bp);
        return;
    }
    ipu.cmd = v; ipu.busy = 1;
    run();
}

void ipu_write_ctrl(u32 v) {
    if (v & (1u << 30)) { reset(); return; }
    ipu.ctrl = (ipu.ctrl & ~0x07ff0000u) | (v & 0x07ff0000u);
}

static u32 ctrl_value(void) {
    int fp = ipu.in_qw < 2 ? ipu.in_qw : 2, ifc = ipu.in_qw - fp;
    return (ipu.ctrl & 0x07ffff00u) | ifc | (u32)ipu.out_qw << 4 | (u32)ipu.busy << 31;
}

u32 ipu_read32(u32 off) {
    run();
    int fp = ipu.in_qw < 2 ? ipu.in_qw : 2, ifc = ipu.in_qw - fp;
    switch (off) {
    case 0x2000: return ipu.data;
    case 0x2004: return (u32)ipu.busy << 31;
    case 0x2010: return ctrl_value();
    case 0x2020: return (u32)ipu.bp | (u32)ifc << 8 | (u32)fp << 16;
    case 0x2030: return peek_avail(32);
    case 0x2034: return (ipu.in_qw * 128 - ipu.bp < 32) ? 0x80000000u : 0;
    }
    return 0;
}

u64 ipu_read64(u32 off) { u64 lo = ipu_read32(off); return lo | (u64)ipu_read32(off + 4) << 32; }

void ipu_fifo_write(const u8 *qw) {
    if (ipu.in_qw >= IN_MAX) { LOG("ipu: input FIFO overflow\n"); return; }
    memcpy(ipu.in + ipu.in_qw * 16, qw, 16); ipu.in_qw++;
    run();
}

void ipu_fifo_read(u8 *qw) {
    run();
    if (!ipu.out_qw) { memset(qw, 0, 16); return; }
    memcpy(qw, ipu.out[ipu.out_head], 16);
    ipu.out_head = (ipu.out_head + 1) % OUT_MAX; ipu.out_qw--;
    run();
}

/* CHCR written with STR set on ch3 or ch4. */
void ipu_dma_start(int ch) {
    u32 *r = hw_dma_regs(ch);
    if (ch == 4) ipu.d4_need_tag = ((r[0] >> 2) & 3) == 1 && (r[2] & 0xffff) == 0;
    if (ch == 4 && g_log_ipu) LOG("ipu: ch4 start chcr %08x madr %08x qwc %u tadr %08x (in %d bp %d busy %d)\n", r[0], r[1], r[2] & 0xffff, r[3], ipu.in_qw, ipu.bp, ipu.busy);
    if (ch == 3 && ((r[0] >> 2) & 3) != 0) LOG("ipu: fromIPU in mode %u\n", (r[0] >> 2) & 3);
    run();
}

void ipu_poll(void) { run(); }

void ipu_stats(void) {
    LOG("ipu: cmds bclr %llu idec %llu bdec %llu vdec %llu fdec %llu setiq %llu setvq %llu csc %llu pack %llu setth %llu, busy %d cmd %08x in %d out %d\n",
        ipu.ncmd[0], ipu.ncmd[1], ipu.ncmd[2], ipu.ncmd[3], ipu.ncmd[4], ipu.ncmd[5], ipu.ncmd[6], ipu.ncmd[7],
        ipu.ncmd[8], ipu.ncmd[9], ipu.busy, ipu.cmd, ipu.in_qw, ipu.out_qw);
}
