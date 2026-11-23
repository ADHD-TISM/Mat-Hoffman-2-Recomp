/* VIF0 / VIF1 command processors. The DMA hands over whole quadwords; a
 * command and its data may span transfers, so each VIF keeps its state.
 * DIRECT/DIRECTHL feed GIF path 2, UNPACK writes VU data memory (cycle,
 * mask, row/col and add modes), MPG writes VU micro memory, MSCAL/MSCNT
 * start the VU (VU1 micro mode is still a stub). */
#include "rt.h"

void gif_transfer(const u8 *qw, u32 nqw, int path);
void vu1_start(u32 pc);
void vu_micro_run(int vu, u32 pc);

typedef struct {
    int unit;
    u8 *mem; u32 qmask;          /* data memory, qw address mask */
    u8 *code; u32 cmask;         /* micro memory, byte mask */
    u32 cl, wl, mode, mask, row[4], col[4];
    u32 base, ofst, tops, top, dbf, itops, itop, mark;
    /* current command */
    u32 code_w, cmd, remain, pos, align;
    u32 buf[4]; u32 nbuf;        /* DIRECT qw / STROW / STCOL words */
    /* UNPACK */
    u32 addr, num, cycle, vn, vl, usn, msk, elem_bytes;
    u8 ebuf[16]; u32 nebuf;
    /* MPG */
    u32 mpg_addr;
    u32 irq, err, stat_int;      /* i-bit pending for the current command, ERR, STAT.INT */
    int stalled;                 /* stopped after an i-bit command until FBRST.STC */
    u32 pend[4], pendk[4], npend;
    int scan;                    /* unit 2: scan-only VIF1 on the EE thread (gsthread.c): state, i bits, no writes */
    int no_irq;                  /* the worker's VIF1: i bits are the scan unit's business */
    int inited;
    u64 n_irq;
    u64 n_unpack, n_direct, n_mscal, n_mpg;
} vif_t;

static vif_t vif[3];          /* 0 VIF0, 1 VIF1, 2 VIF1 scan copy (graphics thread on) */
extern int g_gs_mt;
void gst_vif1(const u8 *p, u32 nqw); void gst_vif1_reset(void); void gst_vif1_reg(u32 off, u32 val);
int g_log_vif;
extern u8 vu0_mem[0x1000], vu0_code[0x1000], vu1_mem[0x4000], vu1_code[0x4000];
u32 vif1_top_value(void) { return vif[1].top; }
u32 vif1_itop_value(void) { return vif[1].itop; }
u32 vif0_itop_value(void) { return vif[0].itop; }

static void vif_init_unit(int u) {
    vif_t *v = &vif[u];
    if (v->inited) return;
    v->inited = 1;
    v->scan = u == 2;
    if (u == 2) u = 1;
    v->unit = u;
    if (u) { v->mem = vu1_mem; v->qmask = 0x3ff; v->code = vu1_code; v->cmask = 0x3fff; }
    else { v->mem = vu0_mem; v->qmask = 0xff; v->code = vu0_code; v->cmask = 0xfff; }
    v->cl = v->wl = 1;
}

/* one unpacked element -> VU memory, honouring mask, mode and the write cycle */
static void unpack_write(vif_t *v, const u32 in[4], int have_data) {
    u32 c = v->cycle < 3 ? v->cycle : 3;
    u32 *dst = (u32 *)(v->mem + ((v->addr & v->qmask) << 4));
    for (int i = 0; i < 4; i++) {
        u32 m = v->msk ? (v->mask >> ((c * 4 + i) * 2)) & 3 : 0;
        if (!have_data && m == 0) m = 1;      /* fill cycles: no input, use the row */
        u32 x;
        switch (m) {
        case 0:
            x = in[i];
            if (v->mode == 1) x += v->row[i];
            else if (v->mode == 2) { v->row[i] += x; x = v->row[i]; }
            break;
        case 1: x = v->row[i]; break;
        case 2: x = v->col[c]; break;
        default: continue;
        }
        if (!v->scan) dst[i] = x;
    }
    v->addr++;
    if (++v->cycle >= v->wl) {
        v->cycle = 0;
        if (v->cl > v->wl) v->addr += v->cl - v->wl;   /* skipping write */
    }
}

/* one element's bytes -> 4 words, by VN/VL/USN */
static inline void decode_element(vif_t *v, const u8 *b, u32 out[4]) {
    int comps = v->vn + 1;
    out[0] = out[1] = out[2] = out[3] = 0;
    if (v->vl == 3) {                       /* V4-5: RGBA 5551 */
        u32 x = b[0] | (b[1] << 8);
        out[0] = (x & 31) << 3; out[1] = ((x >> 5) & 31) << 3; out[2] = ((x >> 10) & 31) << 3; out[3] = (x >> 15) << 7;
    } else {
        for (int i = 0; i < comps; i++) {
            u32 x;
            if (v->vl == 0) memcpy(&x, b + 4 * i, 4);
            else if (v->vl == 1) { u16 h; memcpy(&h, b + 2 * i, 2); x = v->usn ? h : (u32)(s32)(int16_t)h; }
            else { u8 c = b[i]; x = v->usn ? c : (u32)(s32)(int8_t)c; }
            out[i] = x;
        }
        if (comps == 1) out[1] = out[2] = out[3] = out[0];
        else if (comps == 2) { out[2] = out[0]; out[3] = out[1]; }
        else if (comps == 3) {
            /* w gets what follows in the stream on hardware; keep memory instead */
            u32 *dst = (u32 *)(v->mem + ((v->addr & v->qmask) << 4));
            out[3] = v->scan ? 0 : dst[3];
        }
    }
}

static void unpack_element(vif_t *v) {
    u32 out[4];
    decode_element(v, v->ebuf, out);
    unpack_write(v, out, 1);
}

/* UNPACK data words straight from the transfer (the byte-at-a-time path in
 * data_word gives the same result): whole elements are decoded from src.
 * One loop per format (vn, vl constant), with the VIF fields in locals: with
 * -fno-strict-aliasing every store to VU memory made the compiler reload them
 * from v. The scan unit (no writes) only advances addr / cycle, and decodes
 * only for add mode 2, whose ROW it must keep. Any bytes of an element cut by
 * the end of the transfer go to ebuf for data_word. */
#define AIV static inline __attribute__((always_inline))
AIV void bulk_loop(vif_t *v, const u8 *src, u32 ne, const int vn, const int vl) {
    u8 *const mem = v->mem;
    const u32 qm = v->qmask, wl = v->wl, cl = v->cl, mode = v->mode, usn = v->usn, msk = v->msk, mask = v->mask;
    const int scan = v->scan, plain = !msk && !mode && !scan;
    const u32 eb = vl == 3 ? 2 : (vn + 1) * (vl == 0 ? 4 : vl == 1 ? 2 : 1);
    u32 addr = v->addr, cycle = v->cycle, row[4], col[4];
    memcpy(row, v->row, 16); memcpy(col, v->col, 16);
    if (scan && mode != 2) {          /* nothing to write or keep: only the position moves */
        for (u32 e = 0; e < ne; e++) { addr++; if (++cycle >= wl) { cycle = 0; addr += cl - wl; } }
        v->addr = addr; v->cycle = cycle;
        return;
    }
    for (u32 e = 0; e < ne; e++, src += eb) {
        u32 *dst = (u32 *)(mem + ((addr & qm) << 4)), out[4];
        if (vl == 3) {                                  /* V4-5: RGBA 5551 */
            u32 x = src[0] | (src[1] << 8);
            out[0] = (x & 31) << 3; out[1] = ((x >> 5) & 31) << 3; out[2] = ((x >> 10) & 31) << 3; out[3] = (x >> 15) << 7;
        } else {
            for (int i = 0; i <= vn; i++) {
                u32 x;
                if (vl == 0) memcpy(&x, src + 4 * i, 4);
                else if (vl == 1) { u16 h; memcpy(&h, src + 2 * i, 2); x = usn ? h : (u32)(s32)(int16_t)h; }
                else { u8 c = src[i]; x = usn ? c : (u32)(s32)(int8_t)c; }
                out[i] = x;
            }
            if (vn == 0) out[1] = out[2] = out[3] = out[0];
            else if (vn == 1) { out[2] = out[0]; out[3] = out[1]; }
            else if (vn == 2) out[3] = scan ? 0 : dst[3];   /* w: hardware takes what follows in the stream; keep memory */
        }
        if (plain) memcpy(dst, out, 16);
        else {
            u32 c = cycle < 3 ? cycle : 3;
            for (int i = 0; i < 4; i++) {
                u32 m = msk ? (mask >> ((c * 4 + i) * 2)) & 3 : 0, x;
                switch (m) {
                case 0:
                    x = out[i];
                    if (mode == 1) x += row[i];
                    else if (mode == 2) { row[i] += x; x = row[i]; }
                    break;
                case 1: x = row[i]; break;
                case 2: x = col[c]; break;
                default: continue;
                }
                if (!scan) dst[i] = x;
            }
        }
        addr++;
        if (++cycle >= wl) { cycle = 0; addr += cl - wl; }
    }
    v->addr = addr; v->cycle = cycle;
    if (mode == 2) memcpy(v->row, row, 16);
}

static u32 unpack_bulk(vif_t *v, const u8 *src, u32 avail) {
    u32 take = avail < v->remain ? avail : v->remain;
    u32 bytes = take * 4, eb = v->elem_bytes, ne = bytes / eb;
    switch (v->vl == 3 ? 15 : v->vn * 4 + v->vl) {
    case 0: bulk_loop(v, src, ne, 0, 0); break;
    case 1: bulk_loop(v, src, ne, 0, 1); break;
    case 2: bulk_loop(v, src, ne, 0, 2); break;
    case 4: bulk_loop(v, src, ne, 1, 0); break;
    case 5: bulk_loop(v, src, ne, 1, 1); break;
    case 6: bulk_loop(v, src, ne, 1, 2); break;
    case 8: bulk_loop(v, src, ne, 2, 0); break;
    case 9: bulk_loop(v, src, ne, 2, 1); break;
    case 10: bulk_loop(v, src, ne, 2, 2); break;
    case 12: bulk_loop(v, src, ne, 3, 0); break;
    case 13: bulk_loop(v, src, ne, 3, 1); break;
    case 14: bulk_loop(v, src, ne, 3, 2); break;
    default: bulk_loop(v, src, ne, 3, 3); break;
    }
    v->remain -= take;
    u32 left = bytes - ne * eb;
    if (v->remain && left) { memcpy(v->ebuf, src + ne * eb, left); v->nebuf = left; }
    else v->nebuf = 0;    /* padding bytes of the last word dropped */
    return take;
}

static u32 unpack_data_elements(vif_t *v, u32 num) {
    if (v->wl <= v->cl) return num;
    return (num / v->wl) * v->cl + (num % v->wl < v->cl ? num % v->wl : v->cl);
}

static void start_cmd(vif_t *v, u32 w) {
    u32 cmd = (w >> 24) & 0x7f, imm = w & 0xffff, num = (w >> 16) & 0xff;
    if (g_log_vif && !v->scan && (cmd || g_log_vif > 1)) LOG("vif%d: %08x cl %u wl %u tops %x\n", v->unit, w, v->cl, v->wl, v->tops);
    v->code_w = w; v->cmd = cmd; v->remain = 0; v->nbuf = 0; v->align = 0;
    if (cmd >= 0x60) {                                  /* UNPACK */
        v->vn = (cmd >> 2) & 3; v->vl = cmd & 3; v->msk = (cmd >> 4) & 1;
        v->num = num ? num : 256;
        v->usn = (imm >> 14) & 1;
        v->addr = imm & 0x3ff;
        if (v->unit == 1 && (imm & 0x8000)) v->addr += v->tops;
        v->cycle = 0;
        static const u8 bits[4] = { 32, 16, 8, 16 };
        v->elem_bytes = v->vl == 3 ? 2 : (v->vn + 1) * bits[v->vl] / 8;
        u32 elems = unpack_data_elements(v, v->num);
        v->remain = (elems * v->elem_bytes + 3) / 4;
        v->nebuf = 0;
        v->n_unpack++;
        /* filling write: WL > CL, the extra cycles are written as the data
           arrives (see unpack_word); a pure fill with no data is done here */
        if (!v->remain) for (u32 k = 0; k < v->num; k++) { u32 z[4] = { 0 }; unpack_write(v, z, 0); }
        return;
    }
    switch (cmd) {
    case 0x00: break;                                               /* NOP */
    case 0x01: v->cl = imm & 0xff; v->wl = (imm >> 8) & 0xff; if (!v->wl) v->wl = 256; if (!v->cl) v->cl = 256; break;
    case 0x02: v->ofst = imm & 0x3ff; v->dbf = 0; v->tops = v->base; break;   /* OFFSET */
    case 0x03: v->base = imm & 0x3ff; break;                       /* BASE */
    case 0x04: v->itops = imm & 0x3ff; break;                      /* ITOP */
    case 0x05: v->mode = imm & 3; break;                           /* STMOD */
    case 0x06: break;                                               /* MSKPATH3 */
    case 0x07: v->mark = imm; break;                                /* MARK */
    case 0x10: case 0x11: case 0x13: break;                         /* FLUSHE/FLUSH/FLUSHA */
    case 0x14: case 0x15: case 0x17:                                /* MSCAL/MSCALF/MSCNT */
        v->n_mscal++;
        if (v->unit == 1) {
            v->top = v->tops; v->dbf ^= 1; v->tops = v->base + (v->dbf ? v->ofst : 0);
            v->itop = v->itops;
            if (!v->scan) vu1_start(cmd == 0x17 ? 0xffffffffu : imm * 8);
        } else {
            v->itop = v->itops;
            if (cmd != 0x17) vu_micro_run(0, imm * 8);
        }
        break;
    case 0x20: v->remain = 1; break;                                /* STMASK */
    case 0x30: case 0x31: v->remain = 4; break;                     /* STROW/STCOL */
    case 0x4a: v->remain = (num ? num : 256) * 2; v->mpg_addr = imm * 8; v->n_mpg++; break;   /* MPG */
    case 0x50: case 0x51:                                           /* DIRECT/DIRECTHL */
        if (v->unit == 1) { v->remain = (imm ? imm : 65536) * 4; v->align = 1; v->n_direct++; }
        break;
    default:
        { static int once[2]; if (!once[v->unit]++) LOG("vif%d: unknown command %08x\n", v->unit, w); }
    }
}

static void data_word(vif_t *v, u32 w) {
    u32 cmd = v->cmd;
    if (cmd >= 0x60) {
        for (int i = 0; i < 4; i++) {
            v->ebuf[v->nebuf++] = (u8)(w >> (8 * i));
            if (v->nebuf == v->elem_bytes) {
                v->nebuf = 0;
                unpack_element(v);
                /* filling: after CL data cycles the next WL-CL cycles have no data */
                if (v->wl > v->cl) while (v->cycle >= v->cl && v->cycle != 0) { u32 z[4] = { 0 }; unpack_write(v, z, 0); }
            }
        }
        if (--v->remain == 0) v->nebuf = 0;   /* padding bytes of the last word dropped */
        return;
    }
    switch (cmd) {
    case 0x20: v->mask = w; break;
    case 0x30: v->row[4 - v->remain] = w; break;
    case 0x31: v->col[4 - v->remain] = w; break;
    case 0x4a: {
        u8 *p = v->code + (v->mpg_addr & v->cmask);
        if (!v->scan && memcmp(p, &w, 4)) { memcpy(p, &w, 4); if (v->code == vu1_code) { extern u32 vu1_code_gen; vu1_code_gen++; } }
        v->mpg_addr += 4; break; }
    case 0x50: case 0x51:
        v->buf[v->nbuf++] = w;
        if (v->nbuf == 4) { if (!v->scan) gif_transfer((const u8 *)v->buf, 1, 2); v->nbuf = 0; }
        break;
    }
    v->remain--;
}

/* i bit: the VIF stops after the command and raises its interrupt (INTC 4/5);
   the handler resumes it with FBRST.STC. The handler runs here, so whatever
   it changes in memory the rest of the DMA sees. */
static void cmd_done(vif_t *v) {
    if (!v->irq) return;
    if (v->no_irq) { v->irq = 0; return; }
    v->irq = 0;
    if (v->err & 1) return;          /* ERR.MII masks i-bit interrupts */
    v->stat_int = 1;
    v->stalled = 1;
    v->n_irq++;
    intc_raise(v->unit ? 5 : 4);
}

static void vif_word(vif_t *v, u32 w, u32 k) {
    if (v->remain) {
        if (v->align) { if (k != 0) return; v->align = 0; }
        data_word(v, w);
        if (!v->remain) cmd_done(v);
        return;
    }
    start_cmd(v, w);
    v->irq = w >> 31;
    if (!v->remain) cmd_done(v);
}

/* words of a quadword taken in but not yet processed when the VIF stalled */
static void keep_rest(vif_t *v, const u8 *q, u32 k) {
    v->npend = 0;
    for (u32 j = k + 1; j < 4; j++) { memcpy(&v->pend[v->npend], q + 4 * j, 4); v->pendk[v->npend] = j; v->npend++; }
}

static int drain_pending(vif_t *v) {
    while (v->npend && !v->stalled) {
        u32 w = v->pend[0], k = v->pendk[0];
        v->npend--;
        memmove(v->pend, v->pend + 1, v->npend * 4); memmove(v->pendk, v->pendk + 1, v->npend * 4);
        vif_word(v, w, k);
    }
    return !v->stalled;
}

static u32 vif_feed(vif_t *v, const u8 *p, u32 nqw) {
    if (v->stalled || !drain_pending(v)) return 0;
    u32 n = nqw * 4;
    for (u32 i = 0; i < n; ) {
        if (v->remain && v->cmd >= 0x60 && !v->nebuf && v->wl <= v->cl) {   /* UNPACK data: in bulk */
            i += unpack_bulk(v, p + 4 * i, n - i);
            if (!v->remain) cmd_done(v);
            if (v->stalled) { u32 last = i - 1; keep_rest(v, p + 16 * (last / 4), last & 3); return last / 4 + 1; }
            continue;
        }
        u32 w; memcpy(&w, p + 4 * i, 4);
        vif_word(v, w, i & 3);
        if (v->stalled) { keep_rest(v, p + 16 * (i / 4), i & 3); return i / 4 + 1; }
        i++;
    }
    return nqw;
}

u32 vif1_transfer(const u8 *qw, u32 nqw) {
    if (g_gs_mt) {               /* scan on the EE (i bits, registers), the real work on the graphics thread */
        vif_init_unit(2);
        u32 taken = vif_feed(&vif[2], qw, nqw);
        gst_vif1(qw, taken);
        return taken;
    }
    vif_init_unit(1); return vif_feed(&vif[1], qw, nqw);
}
/* graphics thread */
u32 vif1_worker_feed(const u8 *p, u32 nqw) { vif_init_unit(1); vif[1].no_irq = 1; return vif_feed(&vif[1], p, nqw); }
void vif1_worker_reset(void) { vif_t *v = &vif[1]; vif_init_unit(1); v->remain = 0; v->align = 0; v->stat_int = 0; v->irq = 0; v->stalled = 0; v->npend = 0; }
void vif1_worker_reg(u32 off, u32 val) { vif_init_unit(1); if (off == 0x020) vif[1].err = val & 7; else if (off == 0x030) vif[1].mark = val & 0xffff; }
u32 vif0_transfer(const u8 *qw, u32 nqw) { vif_init_unit(0); return vif_feed(&vif[0], qw, nqw); }

/* VIFn registers at 0x10003800 / 0x10003c00 */
u32 vif_reg_read(int u, u32 off) {
    if (u == 1 && g_gs_mt) u = 2;   /* the scan copy is where the EE's view of VIF1 lives */
    vif_init_unit(u);
    vif_t *v = &vif[u];
    switch (off) {
    case 0x000: return (v->stat_int ? 0x800 : 0) | (v->stalled ? 0x400 : 0) | (v->npend ? (1u << 24) : 0);
    case 0x020: return v->err;
    case 0x030: return v->mark;
    case 0x040: return v->cl | (v->wl << 8);
    case 0x050: return v->mode;
    case 0x070: return v->mask;
    case 0x080: return v->code_w;
    case 0x090: return v->itops;
    case 0x0a0: return v->base;
    case 0x0b0: return v->ofst;
    case 0x0c0: return v->tops;
    case 0x0d0: return v->itop;
    case 0x0e0: return v->top;
    }
    if (off >= 0x100 && off < 0x140) return v->row[(off - 0x100) >> 4];
    if (off >= 0x140 && off < 0x180) return v->col[(off - 0x140) >> 4];
    return 0;
}
void vif_reg_write(int u, u32 off, u32 val) {
    int mt = u == 1 && g_gs_mt;
    if (mt) {
        if (off == 0x010 && (val & 1)) gst_vif1_reset();
        if (off == 0x020 || off == 0x030) gst_vif1_reg(off, val);
        vif_init_unit(2);
        vif_t *v = &vif[2];
        switch (off) {
        case 0x010:
            if (val & 1) { v->remain = 0; v->align = 0; v->stat_int = 0; v->irq = 0; v->stalled = 0; v->npend = 0; }
            if (val & 8 && v->stalled) { v->stat_int = 0; v->stalled = 0; if (drain_pending(v)) dma_resume(1); }
            else if (val & 8) v->stat_int = 0;
            return;
        case 0x020: v->err = val & 7; return;
        case 0x030: v->mark = val & 0xffff; return;
        }
        return;
    }
    vif_init_unit(u);
    vif_t *v = &vif[u];
    switch (off) {
    case 0x010:                      /* FBRST */
        if (val & 1) { v->remain = 0; v->align = 0; v->stat_int = 0; v->irq = 0; v->stalled = 0; v->npend = 0; }
        if (val & 8 && v->stalled) {  /* STC: go on with the rest of the qword, then the DMA */
            v->stat_int = 0; v->stalled = 0;
            if (drain_pending(v)) dma_resume(u);
        } else if (val & 8) v->stat_int = 0;
        return;
    case 0x020: v->err = val & 7; return;
    case 0x030: v->mark = val & 0xffff; return;
    }
    if (g_log_vif) LOG("vif%d: write %03x = %08x ignored\n", u, off, val);
}

void vif_stats(void) {
    for (int u = 0; u < 3; u++)
        if (vif[u].inited || u < 2) LOG("vif%s: %llu unpacks, %llu direct, %llu mpg, %llu mscal, %llu irq\n", u == 2 ? "1 (EE scan)" : u ? "1" : "0", (unsigned long long)vif[u].n_unpack,
            (unsigned long long)vif[u].n_direct, (unsigned long long)vif[u].n_mpg, (unsigned long long)vif[u].n_mscal,
            (unsigned long long)vif[u].n_irq);
}
