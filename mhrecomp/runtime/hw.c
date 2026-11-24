/* EE memory map and hardware registers: timers, DMAC, INTC, GS privileged
 * registers, SIF registers. Unknown registers are kept in a sparse store so
 * reads give back what was written. */
#include "rt.h"
#include <stdlib.h>

u8 *ee_page[1 << 20];
u8 ee_ram[32 << 20] __attribute__((aligned(64)));
u8 ee_spr[16 << 10] __attribute__((aligned(64)));
u8 vu0_mem[0x1000], vu0_code[0x1000], vu1_mem[0x4000], vu1_code[0x4000];

u32 intc_stat, intc_mask;
u32 dmac_stat, dmac_ctrl, dmac_pcr;
u32 gs_csr = 0x551b0000, gs_imr = 0x7f00;
u64 gs_priv[0x20];

static u32 io_store[0x10000 / 4];      /* 0x10000000 - 0x1000ffff */
static u32 dma_regs[10][0x10];         /* per channel, 0x10 stride */
static const u32 dma_base[10] = { 0x8000, 0x9000, 0xa000, 0xb000, 0xb400, 0xc000, 0xc400, 0xc800, 0xd000, 0xd400 };
static u32 tim_mode[4], tim_comp[4], tim_hold[4];
static u64 tim_base[4];

u32 *hw_dma_regs(int ch) { return dma_regs[ch]; }

static void map(u32 va, u8 *host, u32 size) {
    for (u32 o = 0; o < size; o += 0x1000) ee_page[(va + o) >> 12] = host + o;
}

void hw_init(void) {
    static const u32 ram_alias[] = { 0x00000000, 0x20000000, 0x30000000, 0x80000000, 0xa0000000 };
    for (unsigned k = 0; k < sizeof ram_alias / 4; k++) map(ram_alias[k], ee_ram, 32 << 20);
    map(0x70000000, ee_spr, 16 << 10);
    map(0x11000000, vu0_code, 0x1000);
    map(0x11004000, vu0_mem, 0x1000);
    map(0x11008000, vu1_code, 0x4000);
    map(0x1100c000, vu1_mem, 0x4000);
}

/* ---- interrupts ------------------------------------------------------- */
void intc_raise(int bit) {
    intc_stat |= 1u << bit;
    if (intc_mask & (1u << bit)) kernel_run_intc_handlers(bit);
}

void dmac_raise(int ch) {
    dmac_stat |= 1u << ch;
    if (dmac_stat & (1u << (16 + ch))) kernel_run_dmac_handlers(ch);
}

/* ---- DMA ---------------------------------------------------------------- */
static u8 *dma_ptr(u32 a, int spr) {
    if (spr || (a & 0x80000000u)) return ee_spr + (a & 0x3ff0);
    return ee_ram + (a & 0x01fffff0);
}

/* Hand nqw quadwords to the channel's device; returns how many it took (a
 * VIF stops early when an i-bit command raised its interrupt). */
static u32 dma_send(int ch, const u8 *p, u32 nqw) {
    if (!nqw) return 0;
    switch (ch) {
    case 0: return vif0_transfer(p, nqw);
    case 1: return vif1_transfer(p, nqw);
    case 2: gif_dma(p, nqw, 3); return nqw;
    case 9: {
        u32 sadr = dma_regs[9][8] & 0x3ff0;
        for (u32 k = 0; k < nqw; k++) { memcpy(ee_spr + sadr, p + 16 * k, 16); sadr = (sadr + 16) & 0x3ff0; }
        dma_regs[9][8] = sadr;
        return nqw; }
    default:
        if (g_log_io) LOG("dma: ch%d send %u qw ignored\n", ch, nqw);
        return nqw;
    }
}

/* State lives in MADR/QWC/TADR/ASR like on the DMAC, so a channel whose
 * device stalls (VIF interrupt) stops with STR still set and continues from
 * dma_resume(). dma_tag_end marks "last tag read, finish QWC and stop". */
static int dma_tag_end[10];

/* move QWC from MADR; returns 0 if the device stalled */
static int dma_block(int ch) {
    u32 *r = dma_regs[ch];
    u32 q = r[2] & 0xffff;
    if (!q) return 1;
    u32 n = dma_send(ch, dma_ptr(r[1], r[1] >> 31), q);
    r[1] += n * 16; r[2] = q - n;
    return n == q;
}

static void dma_continue(int ch) {
    u32 *r = dma_regs[ch];
    u32 chcr = r[0];
    int mode = (chcr >> 2) & 3;
    if (mode == 0) {
        if (!dma_block(ch)) return;
    } else if (mode == 1) {
        int tte = (chcr >> 6) & 1;
        for (int guard = 0; guard < 1 << 20; guard++) {
            if (!dma_block(ch)) return;
            if (dma_tag_end[ch]) break;
            u32 tadr = r[3];
            const u8 *tp = dma_ptr(tadr, tadr >> 31);
            u64 tag; memcpy(&tag, tp, 8);
            u32 qwc = tag & 0xffff, id = (tag >> 28) & 7, addr = (u32)(tag >> 32) & 0xfffffff0u;
            if (tag >> 63) addr |= 0x80000000u;
            int asp = (r[0] >> 4) & 3, end = 0;
            if (tte && (ch == 0 || ch == 1)) {
                /* upper 64 bits of the tag go to the VIF as two codes */
                u8 q[16] = {0}; memcpy(q + 8, tp + 8, 8);
                (ch == 1 ? vif1_transfer : vif0_transfer)(q, 1);
            }
            r[2] = qwc;
            switch (id) {
            case 0: r[1] = addr; r[3] = tadr + 16; end = 1; break;                       /* refe */
            case 1: r[1] = tadr + 16; r[3] = tadr + 16 + qwc * 16; break;                 /* cnt  */
            case 2: r[1] = tadr + 16; r[3] = addr; break;                                 /* next */
            case 3: case 4: r[1] = addr; r[3] = tadr + 16; break;                         /* ref/refs */
            case 5:                                                                       /* call */
                r[1] = tadr + 16;
                if (asp < 2) { r[4 + asp] = tadr + 16 + qwc * 16; asp++; }
                r[3] = addr; break;
            case 6:                                                                       /* ret  */
                r[1] = tadr + 16;
                if (asp > 0) { asp--; r[3] = r[4 + asp]; } else end = 1;
                break;
            case 7: r[1] = tadr + 16; end = 1; break;                                     /* end  */
            }
            if ((tag >> 31) & 1 && (chcr & 0x80)) end = 1;   /* IRQ with TIE */
            r[0] = (r[0] & 0x0000ffcfu) | (asp << 4) | ((u32)(tag >> 16) & 0xffff0000u);
            dma_tag_end[ch] = end;
        }
        if (!dma_block(ch)) return;
    } else {
        if (g_log_io) LOG("dma: ch%d interleave mode unsupported\n", ch);
    }
    r[2] = 0;
    r[0] &= ~0x100u;
    dmac_raise(ch);
}

static void dma_run(int ch) {
    u32 *r = dma_regs[ch];
    if (ch == 8) {   /* fromSPR: normal mode only */
        u32 sadr = r[8] & 0x3ff0, madr = r[1];
        for (u32 k = 0; k < (r[2] & 0xffff); k++) {
            memcpy(dma_ptr(madr, 0), ee_spr + sadr, 16);
            madr += 16; sadr = (sadr + 16) & 0x3ff0;
        }
        r[1] = madr; r[8] = sadr; r[2] = 0;
        r[0] &= ~0x100u;
        dmac_raise(ch);
        return;
    }
    if (ch == 5 || ch == 6 || ch == 7) {
        if (g_log_io) LOG("dma: direct SIF%d start ignored\n", ch - 5);
        r[0] &= ~0x100u;
        dmac_raise(ch);
        return;
    }
    if (((r[0] >> 2) & 3) == 1) { r[2] = 0; dma_tag_end[ch] = 0; }   /* chain: start at TADR */
    dma_continue(ch);
}

/* a stalled device let go: carry on if the channel is still running */
void dma_resume(int ch) {
    if ((dma_regs[ch][0] & 0x100) && (dmac_ctrl & 1)) dma_continue(ch);
}

/* ---- timers ------------------------------------------------------------- */
static const u32 tim_div[4] = { 2, 32, 512, 9371 };     /* EE cycles per tick */
static u32 tim_count(int t) {
    return (u32)((ee.cycles - tim_base[t]) / tim_div[tim_mode[t] & 3]) & 0xffff;
}
/* Timer interrupts (INTC 9 + timer): overflow with T_MODE.OVFE (0x200) and compare
 * with CMPE (0x100), while the timer counts (CUE, 0x80). Mat Hoffman 2 counts timer 0
 * overflows for its clock. tim_irq_at = when the next one is due, 0 = none. ZRET
 * (count reset at compare) is not modelled. */
static u64 tim_ovf_at[4], tim_cmp_at[4];
static void tim_plan(int t) {
    u32 m = tim_mode[t]; u64 d = tim_div[m & 3], period = 0x10000ull * d;
    tim_ovf_at[t] = tim_cmp_at[t] = 0;
    if (!(m & 0x80)) return;
    u64 el = ee.cycles - tim_base[t];
    if (m & 0x200) tim_ovf_at[t] = tim_base[t] + (el / period + 1) * period;
    if (m & 0x100) {
        u64 at = tim_base[t] + (el / period) * period + (u64)(tim_comp[t] & 0xffff) * d;
        if (at <= ee.cycles) at += period;
        tim_cmp_at[t] = at;
    }
    if (tim_ovf_at[t]) ee_set_event(tim_ovf_at[t]);
    if (tim_cmp_at[t]) ee_set_event(tim_cmp_at[t]);
}
u64 hw_timer_next(void) {
    u64 n = ~0ull;
    for (int t = 0; t < 4; t++) {
        if (tim_ovf_at[t] && tim_ovf_at[t] < n) n = tim_ovf_at[t];
        if (tim_cmp_at[t] && tim_cmp_at[t] < n) n = tim_cmp_at[t];
    }
    return n;
}
void hw_timer_event(void) {
    for (int t = 0; t < 4; t++) {
        int hit = (tim_ovf_at[t] && ee.cycles >= tim_ovf_at[t]) || (tim_cmp_at[t] && ee.cycles >= tim_cmp_at[t]);
        if (!hit) continue;
        intc_raise(9 + t);
        tim_plan(t);
    }
}

/* ---- register access ---------------------------------------------------- */
static int dma_chan(u32 off, int *reg) {
    for (int c = 0; c < 10; c++)
        if (off >= dma_base[c] && off < dma_base[c] + 0x100) { *reg = (off - dma_base[c]) >> 4; return c; }
    return -1;
}

static u32 io_read32_raw(u32 a) {
    u32 pa = a & 0x1fffffff;
    if (pa >= 0x10000000 && pa < 0x10010000) {
        u32 off = pa & 0xffff;
        if (off < 0x2000) {
            int t = off >> 11, reg = (off >> 4) & 3;
            if (reg == 0) return tim_count(t);
            if (reg == 1) return tim_mode[t];
            if (reg == 2) return tim_comp[t];
            return tim_hold[t];
        }
        if (off >= 0x2000 && off < 0x3000) return ipu_read32(off);
        int reg, c = dma_chan(off, &reg);
        if (c >= 0) return dma_regs[c][reg];
        switch (off) {
        case 0xe000: return dmac_ctrl;
        case 0xe010: return dmac_stat;
        case 0xe020: return dmac_pcr;
        case 0xf000: return intc_stat;
        case 0xf010: return intc_mask;
        case 0xf200: return sif_get_reg(1);
        case 0xf210: return sif_get_reg(2);
        case 0xf220: return sif_get_reg(3);
        case 0xf230: return sif_get_reg(4);
        case 0xf520: return io_store[0xf590 / 4];
        case 0x3020: return 0;       /* GIF_STAT: idle */
        }
        if (off >= 0x3800 && off < 0x4000) { u32 vif_reg_read(int, u32); return vif_reg_read(off >= 0x3c00, off & 0x3ff); }
        if (g_log_io) LOG("io: r32 %08x\n", a);
        return io_store[off / 4];
    }
    if (pa >= 0x12000000 && pa < 0x12002000) {
        if (pa == 0x12001000) { gst_apply_csr(); return gs_csr; }
        if (pa == 0x12001010) return gs_imr;
        return (u32)gs_priv[(pa >> 4) & 0x1f];
    }
    if (pa >= 0x1fc00000 && pa < 0x20000000) return 0;
    LOG("io: r32 unmapped %08x\n", a);
    return 0;
}

/* Poll loops: the game waits for vblank by re-reading INTC_STAT in a tight
 * loop (WaitVSync 0x1de520), and similar for DMA/VIF/SIF status. Nothing a
 * status register shows can change before the next scheduled event, so after
 * 4 identical reads of one register within 256 cycles of each other, time
 * jumps to that event. Timers (their counts move with time) and the IPU are
 * left alone. --log-io turns it off only for logging clarity. */
u64 n_poll_skips, n_poll_skipped_cycles;
static inline int pollable(u32 pa) {
    if (pa >= 0x10000000 && pa < 0x10003000) return 0;     /* timers, IPU */
    /* GS CSR has its own rule in io_read32 (it depends on the graphics thread) */
    return pa >= 0x10003000 && pa < 0x10010000;
}
u32 io_read32(u32 a) {
    u32 v = io_read32_raw(a), pa = a & 0x1fffffff;
    static u32 last_pa, last_v; static u64 last_cyc; static int reps;
    if (pa == last_pa && v == last_v && ee.cycles - last_cyc < 256 && pollable(pa)) {
        if (++reps >= 4 && ee.next_event > ee.cycles) {
            n_poll_skips++; n_poll_skipped_cycles += ee.next_event - ee.cycles;
            ee.cycles = ee.next_event;
            reps = 0;
        }
    } else if (pa == 0x12001000 && pa == last_pa && v == last_v && ee.cycles - last_cyc < 256) {
        /* The game spins on CSR.FINISH while the graphics thread still has its list
           (Mat Hoffman 2 does at every frame end). Wait for the thread here instead of
           burning emulated time: a slow host then no longer stretches the game's frames
           over many vblanks, and runs are repeatable. */
        /* Always the same steps, whatever the thread was doing: drain it, read
           again, and only if nothing changed jump to the next event. (Skipping
           only when the thread happened to be idle made emulated time depend on
           host speed, so scripted runs drifted apart.) */
        if (++reps >= 4) {
            int gst_take_finish(void);
            reps = 0;
            if (!(v & 2) && gst_take_finish()) {     /* the FINISH it waits for is queued: answer now (gsthread.c) */
                gs_csr |= 2;
                if (!(gs_imr & 0x200)) intc_raise(0);
                last_pa = pa; last_v = v = io_read32_raw(a); last_cyc = ee.cycles;
                return v;
            }
            gst_sync(); u32 v2 = io_read32_raw(a);
            if (v2 == v && ee.next_event > ee.cycles) {
                n_poll_skips++; n_poll_skipped_cycles += ee.next_event - ee.cycles;
                ee.cycles = ee.next_event;
            }
            v = v2;
        }
    } else reps = 0;
    last_pa = pa; last_v = v; last_cyc = ee.cycles;
    return v;
}

void io_write32(u32 a, u32 v) {
    u32 pa = a & 0x1fffffff;
    if (pa >= 0x10000000 && pa < 0x10010000) {
        u32 off = pa & 0xffff;
        if (off < 0x2000) {
            int t = off >> 11, reg = (off >> 4) & 3;
            if (reg == 0) { tim_base[t] = ee.cycles - (u64)(v & 0xffff) * tim_div[tim_mode[t] & 3]; tim_plan(t); return; }
            if (reg == 1) {
                u32 cnt = tim_count(t);                       /* a new clock keeps the count */
                tim_mode[t] = v & ~0xc00u;
                tim_base[t] = ee.cycles - (u64)cnt * tim_div[tim_mode[t] & 3];
                tim_plan(t); return;
            }
            if (reg == 2) { tim_comp[t] = v; tim_plan(t); return; }
            tim_hold[t] = v; return;
        }
        if (off >= 0x4000 && off < 0x8000) {   /* FIFOs written word-wise: rare */
            if (g_log_io) LOG("io: fifo w32 %08x\n", a);
            return;
        }
        if (off >= 0x3800 && off < 0x4000) { void vif_reg_write(int, u32, u32); vif_reg_write(off >= 0x3c00, off & 0x3ff, v); return; }
        if (off == 0x2000) { ipu_write_cmd(v); return; }
        if (off == 0x2010) { ipu_write_ctrl(v); return; }
        int reg, c = dma_chan(off, &reg);
        if (c >= 0) {
            dma_regs[c][reg] = v;
            if (reg == 0 && (v & 0x100) && (dmac_ctrl & 1)) {
                if (c == 3 || c == 4) ipu_dma_start(c); else dma_run(c);
            }
            return;
        }
        switch (off) {
        case 0xe000: dmac_ctrl = v; return;
        case 0xe010:
            dmac_stat &= ~(v & 0xffff);
            dmac_stat ^= v & 0xffff0000u;
            return;
        case 0xe020: dmac_pcr = v; return;
        case 0xf000: intc_stat &= ~v; return;
        case 0xf010: intc_mask ^= v; return;
        case 0xf200: sif_set_reg(1, v); return;
        case 0xf210: sif_set_reg(2, v); return;
        case 0xf220: sif_set_reg(3, v); return;
        case 0xf230: sif_set_reg(4, v); return;
        }
        if (g_log_io) LOG("io: w32 %08x = %08x\n", a, v);
        io_store[off / 4] = v;
        return;
    }
    if (pa >= 0x12000000 && pa < 0x12002000) {
        if (pa == 0x12001000) {
            if (v & 0x200) gs_csr &= ~0x1fu;      /* reset */
            gs_csr &= ~(v & 0x1f);
            return;
        }
        if (pa == 0x12001010) { gs_imr = v; return; }
        /* the rest of 0x12001000+ (upper halves of CSR/IMR from a 64-bit store, BUSDIR,
           SIGLBLID) must not land in the display registers: a `sd` to CSR used to clear
           PMODE through the index wrap and the picture went off */
        if (pa >= 0x12001000) return;
        if (pa & 4) { gs_priv[(pa >> 4) & 0x1f] = (gs_priv[(pa >> 4) & 0x1f] & 0xffffffffull) | ((u64)v << 32); return; }
        gs_priv[(pa >> 4) & 0x1f] = (gs_priv[(pa >> 4) & 0x1f] & ~0xffffffffull) | v;
        return;
    }
    LOG("io: w32 unmapped %08x = %08x\n", a, v);
}

u64 io_read64(u32 a) {
    u32 pa = a & 0x1fffffff;
    if (pa >= 0x12000000 && pa < 0x12002000) {
        if (pa == 0x12001000) return io_read32(a);   /* same path as a 32-bit read: applies FINISH/SIGNAL posted by the graphics thread */
        if (pa == 0x12001010) return gs_imr;
        return gs_priv[(pa >> 4) & 0x1f];
    }
    return io_read32(a) | ((u64)io_read32(a + 4) << 32);
}
void io_write64(u32 a, u64 v) {
    u32 pa = a & 0x1fffffff;
    if (pa >= 0x12000000 && pa < 0x12001000) {
        gs_priv[(pa >> 4) & 0x1f] = v; return;
    }
    io_write32(a, (u32)v);
    if ((pa & 0xf) == 0 && !(pa >= 0x10000000 && pa < 0x10010000)) io_write32(a + 4, (u32)(v >> 32));
}
void io_read128(u32 a, r128 *v) {
    u32 pa = a & 0x1fffffff;
    memset(v, 0, 16);
    if (pa == 0x10007000) { ipu_fifo_read(v->b); return; }
    if (pa >= 0x10004000 && pa < 0x10008000) return;   /* FIFO reads */
    v->d[0] = io_read64(a);
}
void io_write128(u32 a, const r128 *v) {
    u32 pa = a & 0x1fffffff;
    if (pa == 0x10004000) { vif0_transfer(v->b, 1); return; }
    if (pa == 0x10005000) { vif1_transfer(v->b, 1); return; }
    if (pa == 0x10006000) { gif_dma(v->b, 1, 3); return; }
    if (pa == 0x10007010) { ipu_fifo_write(v->b); return; }
    io_write64(a, v->d[0]);
}
u16 io_read16(u32 a) { return (u16)(io_read32(a & ~3u) >> ((a & 2) * 8)); }
u8  io_read8(u32 a)  { return (u8)(io_read32(a & ~3u) >> ((a & 3) * 8)); }
void io_write16(u32 a, u16 v) {
    u32 pa = a & 0x1fffffff;
    if (pa >= 0x10000000 && pa < 0x10010000) {
        int reg, c = dma_chan(pa & 0xffff, &reg);
        if (c >= 0 && reg == 0 && (pa & 2) == 0) { io_write32(a, (dma_regs[c][0] & 0xffff0000u) | v); return; }
    }
    u32 old = io_read32(a & ~3u), sh = (a & 2) * 8;
    io_write32(a & ~3u, (old & ~(0xffffu << sh)) | ((u32)v << sh));
}
void io_write8(u32 a, u8 v) {
    u32 pa = a & 0x1fffffff;
    if (pa == 0x1000f180) { fputc(v, stdout); return; }   /* KPUTCHAR */
    u32 old = io_read32(a & ~3u), sh = (a & 3) * 8;
    if ((pa & 0xffff) >= 0xe000 && (pa & 0xffff) < 0xf100) old = 0;   /* ack-style regs */
    io_write32(a & ~3u, (old & ~(0xffu << sh)) | ((u32)v << sh));
}
