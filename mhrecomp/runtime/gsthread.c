/* Graphics worker threads (like PCSX2's MTVU, wider). The EE thread only
 * queues; the work runs in two stages on two threads:
 *   VU stage: VIF1 and VU1. GIF data it produces (XGKICK path 1, DIRECT path 2)
 *             and what the EE queued for the GS (path 3, PRESENT) go on, in
 *             order, to the GS stage through a second ring.
 *   GS stage: the GS front end (gs.c) and all OpenGL work (gs_gl.c).
 * GS_SPLIT=0 runs both stages on one thread, as before 2026-11-22.
 *
 * The EE side copies what it hands over (VIF1 DMA data, GIF path 3 data) into
 * a ring, so the game may reuse its buffers at once, as after a finished DMA.
 * VIF1 interrupts (the i bit, whose handler patches the rest of the DMA data)
 * stay on the EE side: a scan-only copy of VIF1 (vif.c unit 2) follows the
 * same stream, stops at the i bit exactly as before and answers the EE's
 * VIF1 register reads. The worker's VIF1 ignores i bits. GS SIGNAL/FINISH
 * bits are posted by the GS stage and applied to CSR by the EE (gst_apply_csr).
 * At each vblank the EE queues a PRESENT with a copy of the display registers.
 * The EE waits when the GS stage is more than 2 frames behind, and for a full
 * drain of both rings (gst_sync) before stats and exit.
 *
 * Off with `gs_thread = 0` in mh2.ini or GS_THREAD=0: everything runs on the
 * EE thread as before. */
#include "rt.h"
#include <windows.h>
#include <stdlib.h>

int g_gs_mt;
__thread int g_on_vu_stage;       /* gs.c gif_transfer: on the VU stage of a split, forward instead */
static int split;

enum { C_VIF1 = 1, C_GIF, C_VU1, C_VIF1_RESET, C_VIF1_REG, C_PRESENT, C_WRAP };
typedef struct { u32 type, len, a, b; } cmd_t;        /* len = payload bytes after the header */

#define QSIZE (64u << 20)
typedef struct {
    u8 *q;
    volatile u64 head, tail;                          /* bytes written by the producer / consumed (monotonic) */
    volatile LONG consumer_sleeping, producer_waiting;
    HANDLE ev_work, ev_space;
    u64 n_waits_full;
} ring_t;
static ring_t r1;                                     /* EE -> VU stage */
static ring_t r2;                                     /* VU stage -> GS stage (split) */
static volatile u64 presents_queued, presents_done;
static volatile u32 csr_pending;
static u64 n_cmds, n_bytes, n_waits_frames, n_syncs, n_fwd_bytes;
u64 g_gst_idle_ticks;   /* QPC ticks the GS stage slept with an empty queue (GS_FRAME_TIMES, gs_gl.c) */
/* GS_FRAME_TIMES (gs_gl.c sets g_ft_on): worker time by command kind, in QPC ticks */
int g_ft_on;
u64 g_ft_kind[8];   /* 0 vif1 (VU1 runs inside), 1 gif, 2 vu1 start, 3 present, 4 flush before sleep, 5 other */
HANDLE g_gst_vu_thread;   /* for the VU stage's cycle count (gs_gl.c) */

/* worker side, in vif.c / gs.c / gs_gl.c / vu.c */
u32 vif1_worker_feed(const u8 *p, u32 nqw);
void vif1_worker_reset(void);
void vif1_worker_reg(u32 off, u32 val);
void gif_transfer(const u8 *p, u32 nqw, int path);
void vu1_start(u32 pc);
void gs_present_from(const u64 *priv);
void gsgl_bind_context(void);
void gsgl_unbind_context(void);
void gsgl_flush(void);
void prof_here(int stage);

static inline u64 ld(volatile u64 *p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static inline void st(volatile u64 *p, u64 v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }

/* ---- rings ------------------------------------------------------------------------ */
static void ring_wake(ring_t *r) {
    if (__atomic_load_n(&r->consumer_sleeping, __ATOMIC_ACQUIRE)) SetEvent(r->ev_work);
}
/* the producer waits for the consumer (room, frames, drain): 1 ms timeout, so a missed wake costs 1 ms at most */
static void ring_wait_space(ring_t *r) {
    ring_wake(r);
    __atomic_store_n(&r->producer_waiting, 1, __ATOMIC_SEQ_CST);
    WaitForSingleObject(r->ev_space, 1);   /* a wake missed between the caller's check and here costs 1 ms */
    __atomic_store_n(&r->producer_waiting, 0, __ATOMIC_RELEASE);
}
static void ring_signal_space(ring_t *r) {
    if (__atomic_load_n(&r->producer_waiting, __ATOMIC_ACQUIRE)) SetEvent(r->ev_space);
}

static void ring_put(ring_t *r, u32 type, u32 a, u32 b, const void *data, u32 len) {
    u32 need = (u32)sizeof(cmd_t) + ((len + 15) & ~15u);
    for (;;) {
        u64 head = r->head, tail = ld(&r->tail);
        u32 pos = (u32)(head % QSIZE), room_end = QSIZE - pos;
        if (room_end < need) {                        /* not enough before the end: wrap marker */
            if (QSIZE - (head - tail) < room_end + need) { r->n_waits_full++; ring_wait_space(r); continue; }
            cmd_t w = { C_WRAP, room_end - (u32)sizeof(cmd_t), 0, 0 };
            memcpy(r->q + pos, &w, sizeof w);
            st(&r->head, head + room_end);
            continue;
        }
        if (QSIZE - (head - tail) < need) { r->n_waits_full++; ring_wait_space(r); continue; }
        cmd_t c = { type, len, a, b };
        memcpy(r->q + pos, &c, sizeof c);
        if (len) memcpy(r->q + pos + sizeof c, data, len);
        st(&r->head, head + need);
        __atomic_thread_fence(__ATOMIC_SEQ_CST);      /* the head store before the sleeping-flag load */
        ring_wake(r);
        return;
    }
}

/* consumer: the next command, or 0 after an idle wait (idle_fn runs before sleeping) */
static int ring_next(ring_t *r, cmd_t *c, const u8 **data, u32 *size, int gs_stage) {
    u64 tail = r->tail, head = ld(&r->head);
    if (tail == head) {
        /* spin a little, then sleep until the producer queues something */
        int spun = 0;
        while (ld(&r->head) == tail && spun < 2000) { YieldProcessor(); spun++; }
        if (ld(&r->head) == tail) {
            __atomic_store_n(&r->consumer_sleeping, 1, __ATOMIC_SEQ_CST);
            if (__atomic_load_n(&r->head, __ATOMIC_SEQ_CST) == tail) {
                LARGE_INTEGER f0; if (g_ft_on && gs_stage) QueryPerformanceCounter(&f0);
                if (gs_stage) gsgl_flush();
                LARGE_INTEGER i0, i1; QueryPerformanceCounter(&i0);
                if (g_ft_on && gs_stage) g_ft_kind[4] += (u64)(i0.QuadPart - f0.QuadPart);
                WaitForSingleObject(r->ev_work, 5);
                QueryPerformanceCounter(&i1);
                if (gs_stage) g_gst_idle_ticks += (u64)(i1.QuadPart - i0.QuadPart);
            }
            __atomic_store_n(&r->consumer_sleeping, 0, __ATOMIC_RELEASE);
        }
        return 0;
    }
    u32 pos = (u32)(tail % QSIZE);
    memcpy(c, r->q + pos, sizeof *c);
    *data = r->q + pos + sizeof *c;
    *size = c->type == C_WRAP ? QSIZE - pos : (u32)sizeof(cmd_t) + ((c->len + 15) & ~15u);
    return 1;
}
static void ring_done(ring_t *r, u32 size) {
    st(&r->tail, r->tail + size);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);          /* the tail store before the waiting-flag load */
    ring_signal_space(r);
}

/* ---- EE side ------------------------------------------------------------------ */
static void ee_wait(void) { ring_wait_space(&r1); }
static void put(u32 type, u32 a, u32 b, const void *data, u32 len) {
    ring_put(&r1, type, a, b, data, len);
    n_cmds++; n_bytes += sizeof(cmd_t) + ((len + 15) & ~15u);
}

void gst_vif1(const u8 *p, u32 nqw) { if (nqw) put(C_VIF1, nqw, 0, p, nqw * 16); }
/* Early FINISH. Mat Hoffman 2 ends each frame by sending a GIF path 3 packet
 * that writes FINISH (A+D, register 0x61), then spins on CSR.FINISH. Waiting
 * for the graphics thread there means a full drain each frame: the EE and
 * the graphics stages never overlap (the EE waited ~55 s of ~100 s in Chicago).
 * Instead the EE counts the FINISH writes it queues on path 3 (a copy of the
 * GIF tag parse, below) and, when the game spins on one that is queued but not
 * yet reached, sets the bit at once (gst_take_finish). The GS stage swallows
 * the post for each FINISH answered that way (gst_finish_reached). Everything
 * the GS does stays in order; the frames-ahead limit still holds the EE back.
 * GS_EARLY_FINISH=0 turns it off. */
static CRITICAL_SECTION fin_cs;
static int fin_on = -1, fin_pend, fin_credit;
static u64 n_fin_early;
static struct { int active, flg; u32 nloop, nreg, loop, reg; u64 regs; } p3;
static int count_finish(const u8 *p, u32 nqw) {
    int n = 0;
    for (u32 i = 0; i < nqw; i++, p += 16) {
        u64 lo, hi; memcpy(&lo, p, 8); memcpy(&hi, p + 8, 8);
        if (!p3.active) {
            p3.nloop = (u32)(lo & 0x7fff); p3.flg = (int)((lo >> 58) & 3);
            p3.nreg = (u32)((lo >> 60) & 15); if (!p3.nreg) p3.nreg = 16;
            p3.regs = hi; p3.loop = 0; p3.reg = 0;
            p3.active = p3.nloop != 0;
            continue;
        }
        if (p3.flg == 0) {
            if (((p3.regs >> (4 * p3.reg)) & 15) == 0xe && (hi & 0x7f) == 0x61) n++;
            if (++p3.reg >= p3.nreg) { p3.reg = 0; if (++p3.loop >= p3.nloop) p3.active = 0; }
        } else if (p3.flg == 1) {
            for (int h = 0; h < 2 && p3.active; h++)
                if (++p3.reg >= p3.nreg) { p3.reg = 0; if (++p3.loop >= p3.nloop) p3.active = 0; }
        } else {
            if (++p3.loop >= p3.nloop) p3.active = 0;     /* IMAGE: nloop qwords */
        }
    }
    return n;
}
void gst_gif(const u8 *p, u32 nqw, int path) {
    if (!nqw) return;
    if (fin_on && path == 3) {
        int k = count_finish(p, nqw);
        if (k) { EnterCriticalSection(&fin_cs); fin_pend += k; LeaveCriticalSection(&fin_cs); }
    }
    put(C_GIF, nqw, (u32)path, p, nqw * 16);
}
/* EE, spinning on CSR.FINISH: 1 = a queued FINISH was taken, set the bit now */
int gst_take_finish(void) {
    if (!g_gs_mt || !fin_on) return 0;
    int r = 0;
    EnterCriticalSection(&fin_cs);
    if (fin_pend > 0) { fin_pend--; fin_credit++; r = 1; n_fin_early++; }
    LeaveCriticalSection(&fin_cs);
    return r;
}
/* GS stage, at a FINISH write from GIF path `path`: 1 = post it to the EE */
int gst_finish_reached(int path) {
    if (!fin_on || path != 3) return 1;
    int r = 1;
    EnterCriticalSection(&fin_cs);
    if (fin_credit > 0) { fin_credit--; r = 0; }
    else if (fin_pend > 0) fin_pend--;
    LeaveCriticalSection(&fin_cs);
    return r;
}
void gst_vu1_start(u32 pc) { put(C_VU1, pc, 0, 0, 0); }
void gst_vif1_reset(void) { put(C_VIF1_RESET, 0, 0, 0, 0); }
void gst_vif1_reg(u32 off, u32 val) { put(C_VIF1_REG, off, val, 0, 0); }

int gst_busy(void) { return g_gs_mt && (ld(&r1.tail) != r1.head || (split && ld(&r2.tail) != ld(&r2.head))); }

void gst_present(const u64 *priv) {
    /* at most 2 frames in flight: the picture may lag, the game may not run away */
    while (ld(&presents_done) + 2 < presents_queued) { n_waits_frames++; ee_wait(); }
    presents_queued++;
    LARGE_INTEGER t; QueryPerformanceCounter(&t);   /* when the EE queued it (latency log) */
    put(C_PRESENT, (u32)t.QuadPart, (u32)(t.QuadPart >> 32), priv, 0x20 * 8);
}

/* EE time spent in gst_sync; _late: after STATS_FROM (default 6600, past the
   boot movies and menus of the test routes) */
static u64 sync_ticks, sync_ticks_late;
static u64 stats_from(void) { static u64 f = ~0ull; if (f == ~0ull) { const char *e = getenv("STATS_FROM"); f = e ? strtoull(e, 0, 10) : 6600; } return f; }
void gst_sync(void) {
    if (!g_gs_mt) return;
    n_syncs++;
    LARGE_INTEGER t0, t1; QueryPerformanceCounter(&t0);
    while (ld(&r1.tail) != r1.head) ee_wait();
    while (split && ld(&r2.tail) != ld(&r2.head)) ee_wait();
    QueryPerformanceCounter(&t1);
    sync_ticks += (u64)(t1.QuadPart - t0.QuadPart);
    if (g_frame > stats_from()) sync_ticks_late += (u64)(t1.QuadPart - t0.QuadPart);
}

/* SIGNAL (1) / FINISH (2) bits the GS stage raised: CSR and the GS interrupt, on the EE */
void gst_post_csr(u32 bits) { __atomic_fetch_or(&csr_pending, bits, __ATOMIC_ACQ_REL); }
void gst_apply_csr(void) {
    if (!g_gs_mt || !__atomic_load_n(&csr_pending, __ATOMIC_ACQUIRE)) return;
    u32 b = __atomic_exchange_n(&csr_pending, 0, __ATOMIC_ACQ_REL);
    gs_csr |= b;
    if (((b & 1) && !(gs_imr & 0x100)) || ((b & 2) && !(gs_imr & 0x200))) intc_raise(0);
}

/* VU stage -> GS stage (gs.c gif_transfer calls this on the VU stage) */
void gst2_gif(const u8 *p, u32 nqw, int path) {
    if (!nqw) return;
    ring_put(&r2, C_GIF, nqw, (u32)path, p, nqw * 16);
    n_fwd_bytes += nqw * 16;
}

/* ---- GS-stage work ---------------------------------------------------------------- */
static void do_present(const cmd_t *c, const u8 *data) {
    LARGE_INTEGER t0; QueryPerformanceCounter(&t0);
    gs_present_from((const u64 *)data); __atomic_fetch_add(&presents_done, 1, __ATOMIC_ACQ_REL);
    ring_signal_space(&r1);                       /* the EE may be waiting for frames */
    extern int g_log_frames;
    if (g_log_frames) {
        LARGE_INTEGER t1, fq; QueryPerformanceCounter(&t1); QueryPerformanceFrequency(&fq);
        s64 q = (s64)(((u64)c->b << 32) | c->a);
        double wait = (double)(t0.QuadPart - q) * 1000.0 / fq.QuadPart, took = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / fq.QuadPart;
        static int all = -1; if (all < 0) all = getenv("PRESENT_LOG_ALL") != 0;
        if (wait + took > (all ? -1 : 3)) LOG("gst: present at frame %llu: %.1f ms after it was queued, %.1f ms to present\n", (unsigned long long)g_frame, wait, took);
    }
}
static void ft_add(int k, LARGE_INTEGER k0) {
    LARGE_INTEGER k1; QueryPerformanceCounter(&k1);
    g_ft_kind[k] += (u64)(k1.QuadPart - k0.QuadPart);
}

/* ---- VU stage (and, without a split, the GS stage too) -------------------------------- */
static DWORD WINAPI vu_stage(void *arg) {
    (void)arg;
    if (!split) gsgl_bind_context();
    g_on_vu_stage = split;
    prof_here(1);
    for (;;) {
        cmd_t c; const u8 *data; u32 size;
        if (!ring_next(&r1, &c, &data, &size, !split)) continue;
        LARGE_INTEGER k0; if (g_ft_on) QueryPerformanceCounter(&k0);
        int k = split ? 6 : 5;                    /* 6: the VU stage's "other" (not reported), so the stages never share a counter */
        switch (c.type) {
        case C_WRAP: break;
        case C_VIF1: vif1_worker_feed(data, c.len / 16); k = 0; break;
        case C_GIF: if (split) gst2_gif(data, c.len / 16, (int)c.b); else { gif_transfer(data, c.len / 16, (int)c.b); k = 1; } break;
        case C_VU1: vu1_start(c.a); k = 2; break;
        case C_VIF1_RESET: vif1_worker_reset(); break;
        case C_VIF1_REG: vif1_worker_reg(c.a, c.b); break;
        case C_PRESENT: if (split) ring_put(&r2, C_PRESENT, c.a, c.b, data, c.len); else { do_present(&c, data); k = 3; } break;
        default: LOG("gst: bad command %u at %llu\n", c.type, (unsigned long long)r1.tail); exit(6);
        }
        if (g_ft_on) ft_add(k, k0);
        ring_done(&r1, size);
    }
    return 0;
}

/* ---- GS stage (split) ------------------------------------------------------------------ */
static DWORD WINAPI gs_stage(void *arg) {
    (void)arg;
    gsgl_bind_context();
    prof_here(2);
    for (;;) {
        cmd_t c; const u8 *data; u32 size;
        if (!ring_next(&r2, &c, &data, &size, 1)) continue;
        LARGE_INTEGER k0; if (g_ft_on) QueryPerformanceCounter(&k0);
        int k = 5;
        switch (c.type) {
        case C_WRAP: break;
        case C_GIF: gif_transfer(data, c.len / 16, (int)c.b); k = 1; break;
        case C_PRESENT: do_present(&c, data); k = 3; break;
        default: LOG("gst: bad GS-stage command %u at %llu\n", c.type, (unsigned long long)r2.tail); exit(6);
        }
        if (g_ft_on) ft_add(k, k0);
        ring_done(&r2, size);
        if (r2.tail == ld(&r2.head)) {            /* drained: gst_sync on the EE waits for this ring too */
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            ring_signal_space(&r1);
        }
    }
    return 0;
}

static int ring_init(ring_t *r) {
    r->q = VirtualAlloc(NULL, QSIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    r->ev_work = CreateEventA(NULL, FALSE, FALSE, NULL);
    r->ev_space = CreateEventA(NULL, FALSE, FALSE, NULL);
    return r->q != 0;
}

void gst_init(void) {
    const char *e = getenv("GS_THREAD");
    if (e && *e == '0') { LOG("gst: graphics thread off (GS_THREAD=0)\n"); return; }
    extern int g_gs_thread_cfg;
    if (!g_gs_thread_cfg) { LOG("gst: graphics thread off (gs_thread = 0)\n"); return; }
    if (!ring_init(&r1)) { LOG("gst: no memory for the queue, graphics thread off\n"); return; }
    e = getenv("GS_SPLIT");
    split = !(e && *e == '0') && ring_init(&r2);
    { void vu_init(void); vu_init(); }
    InitializeCriticalSection(&fin_cs);
    { const char *f = getenv("GS_EARLY_FINISH"); fin_on = !(f && *f == '0'); }
    gsgl_unbind_context();
    g_gs_mt = 1;
    HANDLE t = CreateThread(NULL, 64 << 20, vu_stage, NULL, 0, NULL);
    SetThreadPriority(t, THREAD_PRIORITY_ABOVE_NORMAL);
    g_gst_vu_thread = split ? t : 0;
    if (split) {
        HANDLE g = CreateThread(NULL, 64 << 20, gs_stage, NULL, 0, NULL);
        SetThreadPriority(g, THREAD_PRIORITY_ABOVE_NORMAL);
        LOG("gst: graphics threads on (VIF1 + VU1; GS + OpenGL)\n");
    } else LOG("gst: graphics thread on (VIF1, VU1, GS, OpenGL)\n");
}

void gst_stats(void) {
    if (!g_gs_mt) return;
    LOG("gst: %llu commands, %.1f MB queued, %llu waits for room, %llu waits for frames, %llu full syncs\n",
        (unsigned long long)n_cmds, n_bytes / 1048576.0, (unsigned long long)r1.n_waits_full,
        (unsigned long long)n_waits_frames, (unsigned long long)n_syncs);
    { LARGE_INTEGER f; QueryPerformanceFrequency(&f);
      LOG("gst: the EE waited %.1f s in full syncs (%.1f s after frame %llu); %llu FINISH answered early%s\n",
          (double)sync_ticks / f.QuadPart, (double)sync_ticks_late / f.QuadPart, (unsigned long long)stats_from(), (unsigned long long)n_fin_early,
          fin_on ? "" : " (GS_EARLY_FINISH=0)"); }
    if (split) LOG("gst: %.1f MB of GIF data passed to the GS stage, %llu waits for room there\n",
        n_fwd_bytes / 1048576.0, (unsigned long long)r2.n_waits_full);
}
