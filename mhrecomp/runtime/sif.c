/* IOP side of the SIF, high-level emulated: the game's own EE libsif code
 * runs unchanged and talks the SIF command / RPC packet protocol to this file
 * instead of a real IOP. RPC servers (loadfile, heap, the game's ZCDDVD /
 * ZAUDIO modules, pad, memory card...) are answered here. */
#include "rt.h"
#include "iop.h"
#include <stdlib.h>

u8 iop_ram[2 << 20];

#define IOP_CMDBUF   0x0001e000u
#define IOP_SRVBASE  0x00020000u
#define IOP_HEAP     0x001c0000u   /* EE-side IOP heap RPC; iop.c owns 0xb0000-0x1bffff */

#define SIF_CMD_CHANGE_SADDR 0x80000000u
#define SIF_CMD_SET_SREG     0x80000001u
#define SIF_CMD_INIT_CMD     0x80000002u
#define SIF_CMD_RESET_CMD    0x80000003u
#define SIF_CMD_RPC_END      0x80000008u
#define SIF_CMD_RPC_BIND     0x80000009u
#define SIF_CMD_RPC_CALL     0x8000000Au
#define SIF_CMD_RPC_RDATA    0x8000000Cu

static u32 sif_hw[5];           /* 1 MSCOM 2 SMCOM 3 MSFLG 4 SMFLG */
static u32 sysreg[32];
static u32 ee_buf;              /* EE packet receive buffer */
static u32 dma_id = 1;
/* sceSifAllocIopHeap / sceSifFreeIopHeap: first fit over 0x1c0000-0x1effff with
 * real frees (Mat Hoffman 2 allocates and frees a 160 KB buffer per sound bank;
 * a bump pointer walked off the end of IOP RAM). */
#define IOP_HEAP_END 0x001f0000u
static struct { u32 addr, size; } hblk[64]; static int nhblk;
static u32 eeheap_alloc(u32 size) {
    size = (size + 63) & ~63u;
    u32 at = IOP_HEAP;
    for (;;) {
        int hit = 0;
        for (int i = 0; i < nhblk; i++)
            if (at < hblk[i].addr + hblk[i].size && hblk[i].addr < at + size) { at = hblk[i].addr + hblk[i].size; hit = 1; }
        if (!hit) break;
    }
    if (at + size > IOP_HEAP_END || nhblk == 64) { LOG("sif: IOP heap full (%u bytes asked)\n", size); return 0; }
    hblk[nhblk].addr = at; hblk[nhblk].size = size; nhblk++;
    return at;
}
static void eeheap_free(u32 addr) {
    for (int i = 0; i < nhblk; i++) if (hblk[i].addr == addr) { hblk[i] = hblk[--nhblk]; return; }
}

typedef struct { u32 sid; u32 buf; int ncalls; int lle; } server_t;
static server_t srv[64];
static int nsrv;

typedef struct { u8 data[128]; u32 size; } pkt_t;
static pkt_t queue[64];
static int qhead, qtail, awaiting;
static u64 reboot_at;

static inline u32 iop32(u32 a) { u32 v; memcpy(&v, iop_ram + (a & 0x1fffff), 4); return v; }
static inline void iopw32(u32 a, u32 v) { memcpy(iop_ram + (a & 0x1fffff), &v, 4); }

static void ee_copy_to(u32 ea, const u8 *src, u32 n) {
    for (u32 k = 0; k < n; k++) mw8(ea + k, src[k]);
}

void sif_send_to_ee(u32 cid, const u32 *words, int nwords) {
    pkt_t *p = &queue[qtail & 63];
    u32 size = 16 + 4 * nwords;
    memset(p, 0, sizeof *p);
    u32 h[4] = { size, 0, cid, 0 };
    memcpy(p->data, h, 16); memcpy(p->data + 16, words, 4 * nwords);
    p->size = size;
    qtail++;
    ee_set_event(ee.cycles + 4000);
}

void sif_poll(void) {
    if (reboot_at && ee.cycles >= reboot_at) {
        reboot_at = 0; sif_hw[4] |= 0x70000; sif_hw[2] = IOP_CMDBUF;
        if (g_log_sys) LOG("sif: IOP reboot done\n");
    }
    if (reboot_at) ee_set_event(reboot_at);
    iop_run();
    if (awaiting) {
        if (dmac_stat & (1u << 5)) return;       /* EE handler has not run yet */
        awaiting = 0;
    }
    if (qhead == qtail || !ee_buf) return;
    pkt_t *p = &queue[qhead & 63]; qhead++;
    ee_copy_to(ee_buf, p->data, p->size);
    awaiting = 1;
    dmac_raise(5);
    if (qhead != qtail) ee_set_event(ee.cycles + 4000);
}

static server_t *find_srv(u32 sid) {
    for (int i = 0; i < nsrv; i++) if (srv[i].sid == sid) return &srv[i];
    if (nsrv == 64) return &srv[63];
    server_t *s = &srv[nsrv++];
    s->sid = sid; s->buf = IOP_SRVBASE + 0x2000u * (u32)(nsrv - 1); s->ncalls = 0; s->lle = 0;
    return s;
}

/* returns number of bytes to send back from buf */
static void rpc_dispatch(server_t *s, u32 fn, u32 send_size, u32 recv_size) {
    u8 *buf = iop_ram + (s->buf & 0x1fffff);
    s->ncalls++;
    if (g_log_sys || s->ncalls <= 3) {
        LOG("sif: rpc sid=%08x fn=%u send=%u recv=%u :", s->sid, fn, send_size, recv_size);
        for (u32 k = 0; k < send_size && k < 32; k += 4) LOG(" %08x", iop32(s->buf + k));
        LOG("\n");
    }
    switch (s->sid) {
    case 0x80000006: {   /* loadfile */
        static int modid = 1;
        if (fn == 255) { memcpy(buf, "2430", 4); return; }   /* library version: this game's libkernel refuses to load modules unless it reads 2430 */
        const char *path = (const char *)buf + 8;
        LOG("sif: load module '%.60s'\n", path);
        iop_load_module(path);
        iopw32(s->buf, ++modid); iopw32(s->buf + 4, 0);
        return; }
    case 0x80000003:     /* iop heap */
        if (fn == 1) iopw32(s->buf, eeheap_alloc(iop32(s->buf)));
        else if (fn == 2) { eeheap_free(iop32(s->buf)); iopw32(s->buf, 0); }
        else iopw32(s->buf, 0);
        return;
    case 0x80000100: case 0x80000101: {   /* padman: pad.c */
        void pad_rpc(u8 *b); pad_rpc(buf);
        return; }
    case 0x80000400: {   /* mcserv: mc.c, memory card over host files */
        void mc_rpc(u32 fn, u8 *b); mc_rpc(fn, buf);
        return; }
    case 0x80000701: {   /* sdrdrv (libsd remote): block-transfer streams go to spu2.c */
        u32 ch = iop32(s->buf + 4) & 1, mode = iop32(s->buf + 8), addr = iop32(s->buf + 12), size = iop32(s->buf + 16);
        u32 a0 = iop32(s->buf + 4), a1 = mode, a2 = addr, a3 = size, a4 = iop32(s->buf + 20);   /* read before the reply clears them */
        if (recv_size && recv_size <= 0x10000) memset(buf, 0, recv_size);   /* same buffer as the args */
        if (fn == 0x80e0) {          /* BlockTrans(ch, mode, addr, size, ...); mode & 3: 0/3 write, 2 stop */
            if ((mode & 3) == 2) spu2_block_stop(ch); else spu2_block_start(ch, addr, size);
            if (g_log_sys) LOG("sdr: block trans ch%u mode %#x addr %08x size %#x\n", ch, mode, addr, size);
        } else if (fn == 0x8100) {   /* BlockTransStatus: (half << 24) | current address */
            iopw32(s->buf, spu2_block_status(ch));
            if (g_log_sys > 1) LOG("sdr: status ch%u -> %08x\n", ch, iop32(s->buf));
        } else {
            /* the rest of sceSdRemote: args from word 1, result in word 0 (the EE's libsdr
               numbering, 0x8000 + 0x10 * libsd ordinal - 4: SetParam 0x8010 ... VoiceTrans 0x80d0) */
            u32 r = 0;
            switch (fn) {
            case 0x8010: spu2_set_param(a0 & 0xffff, a1 & 0xffff); break;
            case 0x8020: r = spu2_get_param(a0 & 0xffff); break;
            case 0x8030: spu2_set_switch(a0 & 0xffff, a1); break;
            case 0x8040: r = spu2_get_switch(a0 & 0xffff); break;
            case 0x8050: spu2_set_addr(a0 & 0xffff, a1); break;
            case 0x8060: r = spu2_get_addr(a0 & 0xffff); break;
            case 0x8070: spu2_core_attr(a0 & 0xffff, a1); break;
            case 0x80d0: r = spu2_voice_trans((int)a0, (int)a1, a2, a3, a4); break;   /* VoiceTrans(ch, mode, iop, spu, size) */
            case 0x80f0: r = 1; break;                                                 /* VoiceTransStatus: done at once */
            default: { static int seen[256]; if (!seen[fn & 0xff]++) LOG("sdr: fn %#x not handled (args %08x %08x %08x %08x)\n", fn, a0, a1, a2, a3); } break;
            }
            iopw32(s->buf, r);
            if (g_log_sys) LOG("sdr: fn %#x args %08x %08x %08x %08x %08x -> %08x\n", fn, a0, a1, a2, a3, a4, r);
        }
        return; }
    default: {
        int cdvd_rpc(u32 sid, u32 fn, u8 *buf, u32 send_size, u32 recv_size);
        if (cdvd_rpc(s->sid, fn, buf, send_size, recv_size)) return;
        if (recv_size && recv_size <= 0x10000) memset(buf, 0, recv_size);
        return; }
    }
}

static void iop_command(const u8 *pk) {
    u32 w[16]; memcpy(w, pk, 64);
    u32 cid = w[2], opt = w[3];
    if (g_log_sys) LOG("sif: ee->iop cmd %08x opt=%u\n", cid, opt);
    switch (cid) {
    case SIF_CMD_CHANGE_SADDR: ee_buf = w[4]; return;
    case SIF_CMD_SET_SREG: return;
    case SIF_CMD_INIT_CMD:
        if (opt == 0) { ee_buf = w[4]; if (g_log_sys) LOG("sif: ee buffer %08x\n", ee_buf); }
        else { u32 r[2] = { 0, 1 }; sif_send_to_ee(SIF_CMD_SET_SREG, r, 2); }
        return;
    case SIF_CMD_RESET_CMD:
        LOG("sif: IOP reboot '%.80s'\n", (const char *)pk + 24);
        nsrv = 0; nhblk = 0; iop_reset(); ee_buf = 0; qhead = qtail = 0; awaiting = 0;
        reboot_at = ee.cycles + 3000000;    /* ~10 ms of IOP boot time */
        ee_set_event(reboot_at);
        return;
    case SIF_CMD_RPC_BIND: {
        if (iop_is_lle_sid(w[8])) {
            u32 lbuf;
            if (!iop_server_lookup(w[8], &lbuf)) {     /* module not up yet: EE retries */
                u32 r0[8] = { w[4], w[5], w[6], w[7], SIF_CMD_RPC_BIND, 0, 0, 0 };
                sif_send_to_ee(SIF_CMD_RPC_END, r0, 8);
                return;
            }
            server_t *ls = find_srv(w[8]); ls->lle = 1; ls->buf = lbuf;
        }
        server_t *s = find_srv(w[8]);
        if (g_log_sys || 1) LOG("sif: bind sid %08x\n", w[8]);
        u32 r[8] = { w[4], w[5], w[6], w[7], SIF_CMD_RPC_BIND, 0x1000u + (u32)(s - srv) * 0x40u, s->buf, s->buf };
        sif_send_to_ee(SIF_CMD_RPC_END, r, 8);
        return; }
    case SIF_CMD_RPC_CALL: {
        u32 sidx = (w[13] - 0x1000u) / 0x40u;
        if (sidx >= (u32)nsrv) { LOG("sif: call to unknown server %08x\n", w[13]); return; }
        server_t *s = &srv[sidx];
        if (s->lle) { iop_rpc_request(s->sid, w[8], w[9], w[10], w[11], &w[4]); return; }
        rpc_dispatch(s, w[8], w[9], w[11]);
        if (w[11] && w[10]) ee_copy_to(w[10], iop_ram + (s->buf & 0x1fffff), w[11]);
        u32 r[8] = { w[4], w[5], w[6], w[7], SIF_CMD_RPC_CALL, 0x1000u + sidx * 0x40u, s->buf, s->buf };
        sif_send_to_ee(SIF_CMD_RPC_END, r, 8);
        return; }
    case SIF_CMD_RPC_END: iop_rpc_end(w[7], w[8], w[9], w[10]); return;
    case SIF_CMD_RPC_RDATA: {
        u32 src = w[8], dst = w[9], n = w[10];
        ee_copy_to(dst, iop_ram + (src & 0x1fffff), n);
        u32 r[8] = { w[4], w[5], w[6], w[7], SIF_CMD_RPC_RDATA, 0, 0, 0 };
        sif_send_to_ee(SIF_CMD_RPC_END, r, 8);
        return; }
    default:
        LOG("sif: unhandled ee->iop cmd %08x\n", cid);
    }
}

int sif_set_dma(u32 list, int n) {
    int cmd = 0; u32 cmdaddr = 0;
    for (int k = 0; k < n; k++) {
        u32 e = list + 16 * k;
        u32 src = mr32(e), dst = mr32(e + 4), size = mr32(e + 8);
        if (g_log_sys > 1) LOG("sif: dma %08x -> iop %08x (%u)\n", src, dst, size);
        for (u32 o = 0; o < size; o++) iop_ram[(dst + o) & 0x1fffff] = mr8(src + o);
        if ((dst & 0x1fffff) == IOP_CMDBUF) { cmd = 1; cmdaddr = dst; }
    }
    if (cmd) iop_command(iop_ram + (cmdaddr & 0x1fffff));
    return dma_id++;
}

int sif_dma_stat(int id) { (void)id; return -1; }

u32 sif_get_reg(u32 r) {
    if (r & 0x80000000u) return sysreg[r & 31];
    if (r >= 1 && r <= 4) return sif_hw[r];
    return 0;
}

void sif_set_reg(u32 r, u32 v) {
    if (g_log_sys) LOG("sif: set reg %08x = %08x\n", r, v);
    if (r & 0x80000000u) { sysreg[r & 31] = v; return; }
    switch (r) {
    case 1: sif_hw[1] = v; ee_buf = v ? v : ee_buf; return;
    case 3: sif_hw[3] |= v; return;
    case 4: sif_hw[4] &= ~v; return;
    }
}

void sif_init(void) {
    sif_hw[2] = IOP_CMDBUF;
    sif_hw[4] = 0x70000;    /* SIFINIT | CMDINIT | BOOTEND */
}
