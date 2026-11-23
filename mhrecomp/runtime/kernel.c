/* EE kernel HLE: syscalls, threads (one host fiber per guest thread),
 * semaphores, INTC/DMAC handlers, alarms and the vblank clock. */
#include "rt.h"
#include <windows.h>
#include <stdlib.h>

#define EE_HZ        294912000ull
#define FRAME_CYC    (EE_HZ * 1001 / 60000)
#define HSYNC_CYC    (FRAME_CYC / 262)
#define INTR_STACK   0x00078000u          /* kernel area, unused by the game */

typedef struct {
    r128 r[32]; r128 hi, lo; u32 sa; fr fpr[32]; fr acc; u32 fcr31;
} ctx_t;

enum { TS_RUN = 1, TS_READY = 2, TS_WAIT = 4, TS_SUSPEND = 8, TS_WAITSUSPEND = 12, TS_DORMANT = 16 };
enum { WT_NONE, WT_SLEEP, WT_SEMA };

typedef struct {
    int used, status, prio, init_prio, wakeups, wait_type, wait_id;
    u32 entry, stack, stack_size, gp, attr, option;
    u64 order;
    void *fiber;
    ctx_t ctx;
} thread_t;

typedef struct { int used, count, max, init, waiters; u32 attr, option; } sema_t;
typedef struct { int used; int cause; u32 handler, arg, gp; int enabled; } handler_t;
typedef struct { int used; u64 at; u32 handler, arg, gp, time; } alarm_t;

#define MAXT 256
#define MAXS 256
#define MAXH 64
static thread_t th[MAXT];
static sema_t sem[MAXS];
static handler_t intc_h[MAXH], dmac_h[MAXH];
static alarm_t alarms[64];
static int cur = 1;
static u64 order_ctr = 1;
static int in_intr, in_idle;
static u32 pending_intc, pending_dmac;
static u64 next_vbon, next_vboff;
static u32 heap_start, heap_end, vsync_flag_addr, vsync_field_addr;
static void *main_fiber;
static LARGE_INTEGER perf_freq, perf_last;
u64 g_frame;

static void save_ctx(ctx_t *c) {
    memcpy(c->r, ee.r, sizeof ee.r); c->hi = ee.hi; c->lo = ee.lo; c->sa = ee.sa;
    memcpy(c->fpr, ee.fpr, sizeof ee.fpr); c->acc = ee.acc; c->fcr31 = ee.fcr31;
}
static void load_ctx(const ctx_t *c) {
    memcpy(ee.r, c->r, sizeof ee.r); ee.hi = c->hi; ee.lo = c->lo; ee.sa = c->sa;
    memcpy(ee.fpr, c->fpr, sizeof ee.fpr); ee.acc = c->acc; ee.fcr31 = c->fcr31;
}

#define A0 GW(4)
#define A1 GW(5)
#define A2 GW(6)
#define A3 GW(7)
#define T0 GW(8)
static void ret(s32 v) { GD(2) = (u64)(s64)v; }

/* ---- scheduling ------------------------------------------------------- */
static void schedule(void);

static void WINAPI thread_fiber(void *p) {
    thread_t *t = p;
    for (;;) {
        call_guest(t->entry);
        if (g_log_sys) LOG("kernel: thread %d returned\n", (int)(t - th));
        t->status = TS_DORMANT;
        schedule();
    }
}

static int pick(void) {
    int best = -1;
    for (int i = 1; i < MAXT; i++) {
        if (!th[i].used || !(th[i].status & (TS_RUN | TS_READY))) continue;
        if (best < 0 || th[i].prio < th[best].prio ||
            (th[i].prio == th[best].prio && th[i].order < th[best].order)) best = i;
    }
    return best;
}

static void switch_to(int n) {
    if (n == cur) { th[cur].status = TS_RUN; return; }
    thread_t *o = &th[cur], *t = &th[n];
    save_ctx(&o->ctx);
    if (o->status == TS_RUN) o->status = TS_READY;
    t->status = TS_RUN;
    cur = n;
    load_ctx(&t->ctx);
    if (!t->fiber) t->fiber = CreateFiberEx(256 << 10, 32 << 20, FIBER_FLAG_FLOAT_SWITCH, thread_fiber, t);
    if (g_log_sys > 1) LOG("kernel: switch %d -> %d\n", (int)(o - th), n);
    SwitchToFiber(t->fiber);
    /* resumed: our context was loaded by whoever switched back */
}

static void schedule(void) {
    if (in_intr) return;
    for (;;) {
        int n = pick();
        if (n > 0) { switch_to(n); return; }
        /* nothing runnable: let time pass until an interrupt readies someone */
        in_idle = 1;
        if (ee.cycles < ee.next_event) ee.cycles = ee.next_event;
        ee_event();
        in_idle = 0;
    }
}

static void preempt_check(void) {
    if (in_intr) return;
    int n = pick();
    if (n > 0 && n != cur && th[n].prio < th[cur].prio) switch_to(n);
}

/* ---- guest callbacks from interrupt context --------------------------- */
static void run_handler(u32 fn, u32 gp, u32 a0, u32 a1, u32 a2) {
    ctx_t save; save_ctx(&save);
    int was = in_intr; in_intr = 1;
    GD(4) = a0; GD(5) = a1; GD(6) = a2; GD(28) = (u64)(s64)(s32)gp;
    GD(29) = INTR_STACK - 0x100;
    call_guest(fn);
    in_intr = was;
    load_ctx(&save);
}

void kernel_run_intc_handlers(int cause) { pending_intc |= 1u << cause; ee.next_event = 0; }
void kernel_run_dmac_handlers(int ch)    { pending_dmac |= 1u << ch;    ee.next_event = 0; }

static void dispatch_interrupts(void) {
    if (in_intr || !ee.intr_enabled) return;
    for (int guard = 0; guard < 64 && ((pending_intc & intc_mask) | (pending_dmac & (dmac_stat >> 16))); guard++) {
        u32 pi = pending_intc & intc_mask; pending_intc &= ~pi;
        for (int b = 0; b < 15; b++) {
            if (!(pi & (1u << b))) continue;
            intc_stat &= ~(1u << b);
            for (int k = 0; k < MAXH; k++)
                if (intc_h[k].used && intc_h[k].cause == b && intc_h[k].enabled)
                    run_handler(intc_h[k].handler, intc_h[k].gp, b, intc_h[k].arg, 0);
        }
        u32 pd = pending_dmac & (dmac_stat >> 16); pending_dmac &= ~pd;
        for (int c = 0; c < 15; c++) {
            if (!(pd & (1u << c))) continue;
            dmac_stat &= ~(1u << c);
            for (int k = 0; k < MAXH; k++)
                if (dmac_h[k].used && dmac_h[k].cause == c && dmac_h[k].enabled)
                    run_handler(dmac_h[k].handler, dmac_h[k].gp, c, dmac_h[k].arg, 0);
        }
    }
}

/* Frame work time: from the end of the last pace() to the start of this one,
 * i.e. what emulating and drawing one frame cost, without the wait. A frame
 * over 16.7 ms is a dropped frame on screen and an audio underrun.
 * --log-frames logs each frame over 17 ms; the exit report counts them. */
int g_log_frames;
static u64 fr_n, fr_17, fr_25, fr_50; static double fr_worst[8]; static u64 fr_worst_at[8];
static void frame_report(void) {
    if (!fr_n) return;
    LOG("frames: %llu timed, %llu over 17 ms, %llu over 25 ms, %llu over 50 ms; worst:", (unsigned long long)fr_n,
        (unsigned long long)fr_17, (unsigned long long)fr_25, (unsigned long long)fr_50);
    for (int k = 0; k < 8 && fr_worst[k] > 0; k++) LOG(" %.1f@%llu", fr_worst[k], (unsigned long long)fr_worst_at[k]);
    LOG("\n");
}
static void frame_stat(double ms) {
    static int reg; if (!reg) { reg = 1; atexit(frame_report); }
    if (g_frame < 60) return;                       /* boot */
    fr_n++;
    if (ms > 17) fr_17++; if (ms > 25) fr_25++; if (ms > 50) fr_50++;
    for (int k = 0; k < 8; k++) if (ms > fr_worst[k]) {
        for (int j = 7; j > k; j--) { fr_worst[j] = fr_worst[j - 1]; fr_worst_at[j] = fr_worst_at[j - 1]; }
        fr_worst[k] = ms; fr_worst_at[k] = g_frame; break;
    }
    if (g_log_frames && ms > 17) LOG("frames: frame %llu took %.1f ms\n", (unsigned long long)g_frame, ms);
}

int g_pace_restart;   /* menu.c: the game was paused; the next frame is not timed */
static void pace(void) {
    static LARGE_INTEGER done; static int have;
    if (g_pace_restart) { g_pace_restart = 0; have = 0; }
    extern int gsgl_vsync_paced(void); extern double gsgl_swap_wait_ms(void);
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    int vs = gsgl_vsync_paced();
    if (have) frame_stat((double)(now.QuadPart - done.QuadPart) * 1000.0 / perf_freq.QuadPart - (vs && !g_gs_mt ? gsgl_swap_wait_ms() : 0));
    have = 1;
    if (g_turbo) { done = now; return; }
    static int period;
    if (!period) { period = 1; timeBeginPeriod(1); }   /* else Sleep() rounds to ~15.6 ms and frames run late */
    /* 59.94 Hz like the PS2, or 60.00 with vsync on a 60 Hz display: the swap
     * alone does not hold the rate (in a window, the compositor followed the
     * 120 Hz primary monitor), so the timer keeps the cadence and vsync only
     * lines presentation up with the scan-out. */
    double target = vs ? (double)perf_freq.QuadPart / 60.0 : (double)perf_freq.QuadPart * 1001.0 / 60000.0;
    double el = (double)(now.QuadPart - perf_last.QuadPart);
    DWORD slept = 0;
    if (el < target) {
        DWORD ms = (DWORD)((target - el) * 1000.0 / perf_freq.QuadPart);
        if (ms > 1) { slept = ms - 1; Sleep(ms - 1); }
        do { QueryPerformanceCounter(&now); } while ((double)(now.QuadPart - perf_last.QuadPart) < target);
        perf_last.QuadPart += (LONGLONG)target;
    } else if (el < 4 * target) {
        perf_last.QuadPart += (LONGLONG)target;     /* a slow frame: keep the schedule, later frames catch up */
    } else perf_last = now;                         /* far behind (load, debugger): start again from now */
    static LARGE_INTEGER prev_done;
    QueryPerformanceCounter(&done);
    if (g_log_frames && prev_done.QuadPart) {
        double ms = (double)(done.QuadPart - prev_done.QuadPart) * 1000.0 / perf_freq.QuadPart;
        if (ms > 20) LOG("frames: pace gap %.1f ms at frame %llu (slept %lu, late by %.1f)\n", ms, (unsigned long long)g_frame,
                         (unsigned long)slept, (double)(now.QuadPart - perf_last.QuadPart) * 1000.0 / perf_freq.QuadPart);
    }
    prev_done = done;
}

static void recompute_next(void) {
    u64 n = next_vbon < next_vboff ? next_vbon : next_vboff;
    for (int i = 0; i < 64; i++) if (alarms[i].used && alarms[i].at < n) n = alarms[i].at;
    { u64 hw_timer_next(void); u64 t = hw_timer_next(); if (t < n) n = t; }
    ee.next_event = n;
}

void ee_set_event(u64 at) { if (at < ee.next_event) ee.next_event = at; }

void ee_event(void) {
    gst_apply_csr();   /* SIGNAL/FINISH posted by the graphics thread */
    if (ee.cycles >= next_vbon) {
        next_vbon += FRAME_CYC; g_frame++;
        { extern void check_frame_limit(void); check_frame_limit(); }
        gs_csr ^= 0x2000; gs_csr |= 8;   /* VSINT; the GS interrupt for it is raised below */
        if (vsync_flag_addr) { mw32(vsync_flag_addr, 1); if (vsync_field_addr) mw64(vsync_field_addr, (gs_csr >> 13) & 1); }
        spu2_update();
        /* wait for the frame's deadline first, then present: the screen then gets
         * frames at even intervals however long the frame's work took (presenting
         * before the wait gave ~4 ms jitter on movie frames that decode a picture,
         * ~400 frames shown twice in boot + logos). Input is read after the wait
         * too, as late as possible. */
        pace();
        gs_vsync();
        { void menu_vsync(void); menu_vsync(); }   /* Esc / Back+Start: the game pauses in the menu here */
        { void pad_vsync(void); pad_vsync(); }
        if (!(gs_imr & 0x800)) intc_raise(0);
        intc_raise(2);
    }
    if (ee.cycles >= next_vboff) { next_vboff += FRAME_CYC; intc_raise(3); }
    for (int i = 0; i < 64 && !in_intr && ee.intr_enabled; i++)
        if (alarms[i].used && ee.cycles >= alarms[i].at) {
            alarms[i].used = 0;
            run_handler(alarms[i].handler, alarms[i].gp, i + 1, alarms[i].time, alarms[i].arg);
        }
    { void hw_timer_event(void); hw_timer_event(); }
    sif_poll();
    recompute_next();
    if (pending_intc | pending_dmac) {
        dispatch_interrupts();
        /* More arrived than one pass delivers (Mat Hoffman 2 chains one VIF1 interrupt per
           texture, dozens a frame): come straight back instead of waiting for the next
           timed event, which left the rest of the frame's list for the next vblank.
           Masked or DI: ee_ei / EnableIntc ask again. */
        if (!in_intr && ee.intr_enabled && ((pending_intc & intc_mask) | (pending_dmac & (dmac_stat >> 16)))) ee.next_event = 0;
    }
    if (!in_idle) preempt_check();
}

/* ---- syscalls ----------------------------------------------------------- */
static int new_thread(void) { for (int i = 2; i < MAXT; i++) if (!th[i].used) return i; return -1; }

static void wake(int i) {
    th[i].wait_type = WT_NONE;
    if (th[i].status == TS_WAIT) th[i].status = TS_READY;
    else if (th[i].status == TS_WAITSUSPEND) th[i].status = TS_SUSPEND;
    th[i].order = order_ctr++;
}

static void block(int wt, int id) {
    th[cur].status = TS_WAIT; th[cur].wait_type = wt; th[cur].wait_id = id;
    schedule();
}

static int sema_waiter(int s) {
    int best = -1;
    for (int i = 1; i < MAXT; i++)
        if (th[i].used && (th[i].status & TS_WAIT) && th[i].wait_type == WT_SEMA && th[i].wait_id == s)
            if (best < 0 || th[i].order < th[best].order) best = i;
    return best;
}

static u32 setup_thread(void) {
    u32 gp = A0, stack = A1, size = A2, args = A3;
    u32 top = (stack == 0xffffffffu) ? 0x02000000u : stack + size;
    thread_t *t = &th[1];
    t->used = 1; t->status = TS_RUN; t->prio = t->init_prio = 0; t->gp = gp;
    t->stack = top - size; t->stack_size = size; t->fiber = main_fiber; t->order = order_ctr++;
    cur = 1;
    if (args) {
        static const char name[] = "cdrom0:\\SLUS_203.35;1";
        mw32(args, 1);
        mw32(args + 4, args + 0x40);
        for (unsigned k = 0; k < sizeof name; k++) mw8(args + 0x40 + k, name[k]);
    }
    if (g_log_sys) LOG("kernel: SetupThread gp=%08x stack=%08x size=%x -> sp %08x\n", gp, stack, size, top - 0x2a0);
    return top - 0x2a0;
}

static int add_handler(handler_t *h, int cause, u32 fn, u32 arg) {
    for (int k = 0; k < MAXH; k++) if (!h[k].used) {
        h[k].used = 1; h[k].cause = cause; h[k].handler = fn; h[k].arg = arg; h[k].gp = GW(28); h[k].enabled = 1;
        return k + 1;
    }
    return -1;
}

static void write_thread_status(int id, u32 p) {
    thread_t *t = &th[id];
    mw32(p + 0, t->status); mw32(p + 4, t->entry); mw32(p + 8, t->stack); mw32(p + 12, t->stack_size);
    mw32(p + 16, t->gp); mw32(p + 20, t->init_prio); mw32(p + 24, t->prio);
    mw32(p + 28, t->attr); mw32(p + 32, t->option); mw32(p + 36, t->wait_type);
    mw32(p + 40, t->wait_id); mw32(p + 44, t->wakeups);
}

/* DECI2 (debugger link): the game's printf sends TTY packets through it and
 * waits for the write-done event. Complete the write and print the text. */
static u32 deci2_handler, deci2_opt, deci2_gp, deci2_sent, deci2_len;
static s32 deci2_call(u32 fn, u32 p) {
    if ((s32)fn < 0) fn = (u32)-(s32)fn;   /* the game's libs call ExSend as -6 */
    switch (fn) {
    case 1: deci2_handler = mr32(p + 8); deci2_opt = mr32(p + 4); deci2_gp = GW(28); return 1;
    case 3: {   /* ReqSend: ask the handler for data until the packet is complete */
        deci2_sent = 0; deci2_len = 0;
        for (int guard = 0; guard < 64; guard++) {
            u32 before = deci2_sent;
            run_handler(deci2_handler, deci2_gp, 3, 0, deci2_opt);
            if (deci2_sent == before || (deci2_len && deci2_sent >= deci2_len)) break;
        }
        run_handler(deci2_handler, deci2_gp, 4, 0, deci2_opt);
        return 0; }
    case 6: {   /* ExSend(socket, buf, len) */
        u32 buf = mr32(p + 4), len = mr32(p + 8);
        for (u32 k = 0; k < len; k++) {
            u32 pos = deci2_sent + k; u8 c = mr8(buf + k);
            if (pos == 0) deci2_len = c; else if (pos == 1) deci2_len |= (u32)c << 8;
            if (pos >= 12) fputc(c, stderr);
        }
        deci2_sent += len;
        return (s32)len; }
    default: return 0;
    }
}

void kernel_dump(void);
void ee_syscall(u32 pc) {
    s32 n = (s32)GW(3);
    int irq = n < 0; if (irq) n = -n;
    if (g_log_sys > 1) LOG("syscall %02x%s a0=%08x a1=%08x a2=%08x a3=%08x (pc %08x)\n", n, irq ? "i" : "", A0, A1, A2, A3, pc);
    /* libkernel's alarm patch (SetSyscall 0xfc-0xff) replaces the alarm calls with
       its own kernel code; here they are the HLE alarms again */
    if (n == 0xfc) n = 0x18; else if (n == 0xfd) n = 0x19; else if (n == 0xfe) n = 0x1e; else if (n == 0xff) n = 0x1f;
    switch (n) {
    case 0x02: if (g_log_sys) LOG("kernel: SetGsCrt(%u,%u,%u)\n", A0, A1, A2); ret(0); return;
    case 0x04: LOG("kernel: Exit(%d) ra %08x\n", (int)A0, GW(31)); kernel_dump(); fflush(stdout); exit(0);
    case 0x06: case 0x07: LOG("kernel: LoadExecPS2/ExecPS2 not supported\n"); exit(1);
    case 0x10: ret(add_handler(intc_h, A0, A1, A3)); if (g_log_sys) LOG("kernel: AddIntcHandler(%u, %08x)\n", A0, A1); return;
    case 0x11: if (A1 >= 1 && A1 <= MAXH) intc_h[A1 - 1].used = 0; ret(0); return;
    case 0x12: ret(add_handler(dmac_h, A0, A1, A3)); if (g_log_sys) LOG("kernel: AddDmacHandler(%u, %08x)\n", A0, A1); return;
    case 0x13: if (A1 >= 1 && A1 <= MAXH) dmac_h[A1 - 1].used = 0; ret(0); return;
    case 0x14: case 0x1a: { u32 m = 1u << (A0 & 31); int was = (intc_mask & m) != 0; intc_mask |= m; ret(!was);
        if (intc_stat & m) kernel_run_intc_handlers(A0); return; }
    case 0x15: case 0x1b: { u32 m = 1u << (A0 & 31); int was = (intc_mask & m) != 0; intc_mask &= ~m; ret(was); return; }
    case 0x16: case 0x1c: { u32 m = 1u << (16 + (A0 & 15)); int was = (dmac_stat & m) != 0; dmac_stat |= m; ret(!was);
        if (dmac_stat & (1u << (A0 & 15))) kernel_run_dmac_handlers(A0); return; }
    case 0x17: case 0x1d: { u32 m = 1u << (16 + (A0 & 15)); int was = (dmac_stat & m) != 0; dmac_stat &= ~m; ret(was); return; }
    case 0x18: case 0x1e: {
        for (int i = 0; i < 64; i++) if (!alarms[i].used) {
            alarms[i] = (alarm_t){ 1, ee.cycles + (u64)(A0 & 0xffff) * HSYNC_CYC, A1, A2, GW(28), A0 & 0xffff };
            ee_set_event(alarms[i].at); ret(i + 1); return;
        }
        ret(-1); return; }
    case 0x19: case 0x1f: if (A0 >= 1 && A0 <= 64) alarms[A0 - 1].used = 0; ret(0); return;
    case 0x20: {   /* CreateThread */
        int i = new_thread(); if (i < 0) { ret(-1); return; }
        u32 p = A0; thread_t *t = &th[i];
        memset(t, 0, sizeof *t); t->used = 1; t->status = TS_DORMANT;
        t->entry = mr32(p + 4); t->stack = mr32(p + 8); t->stack_size = mr32(p + 12);
        t->gp = mr32(p + 16); t->prio = t->init_prio = mr32(p + 20); t->attr = mr32(p + 28); t->option = mr32(p + 32);
        if (g_log_sys) LOG("kernel: CreateThread %d entry=%08x stack=%08x+%x prio=%d\n", i, t->entry, t->stack, t->stack_size, t->prio);
        ret(i); return; }
    case 0x21: if (A0 > 1 && A0 < MAXT && th[A0].status == TS_DORMANT) { th[A0].used = 0; ret(0); } else ret(-1); return;
    case 0x22: {   /* StartThread(id, arg) */
        int i = A0; if (i <= 0 || i >= MAXT || !th[i].used) { ret(-1); return; }
        thread_t *t = &th[i];
        memset(&t->ctx, 0, sizeof t->ctx);
        t->ctx.r[4].d[0] = A1; t->ctx.r[28].d[0] = (u64)(s64)(s32)t->gp;
        t->ctx.r[29].d[0] = (u64)(s64)(s32)(t->stack + t->stack_size - 0x2a0);
        t->ctx.r[31].d[0] = 0;
        t->ctx.fcr31 = 0x01000001;
        t->status = TS_READY; t->prio = t->init_prio; t->wakeups = 0; t->order = order_ctr++;
        if (g_log_sys) LOG("kernel: StartThread %d arg=%08x\n", i, A1);
        ret(0); preempt_check(); return; }
    case 0x23: case 0x24:   /* ExitThread / ExitDeleteThread */
        if (g_log_sys) LOG("kernel: ExitThread %d\n", cur);
        th[cur].status = TS_DORMANT; schedule();
        /* resumed after a restart: the fiber loop calls the entry again */
        call_guest(th[cur].entry); th[cur].status = TS_DORMANT; schedule(); return;
    case 0x25: case 0x26:
        if (A0 > 0 && A0 < MAXT && (int)A0 != cur) { th[A0].status = TS_DORMANT; th[A0].wait_type = 0; }
        ret(0); return;
    case 0x27: case 0x28: ret(0); return;
    case 0x29: case 0x2a: {   /* ChangeThreadPriority */
        int i = A0 ? (int)A0 : cur; if (i <= 0 || i >= MAXT || !th[i].used) { ret(-1); return; }
        int old = th[i].prio; th[i].prio = A1; th[i].order = order_ctr++; ret(old);
        if (!irq) { int n2 = pick(); if (n2 > 0 && n2 != cur && th[n2].prio <= th[cur].prio && (th[n2].prio < th[cur].prio || i == cur)) switch_to(n2); }
        return; }
    case 0x2b: case 0x2c: {   /* RotateThreadReadyQueue(prio) */
        if (th[cur].prio == (int)A0) th[cur].order = order_ctr++;
        for (int i = 1; i < MAXT; i++) if (i != cur && th[i].used && th[i].status == TS_READY && th[i].prio == (int)A0 && irq) { th[i].order = order_ctr++; break; }
        ret(0); if (!irq) { int n2 = pick(); if (n2 > 0 && n2 != cur) switch_to(n2); }
        return; }
    case 0x2d: case 0x2e: if (A0 < MAXT && (th[A0].status & TS_WAIT)) wake(A0); ret(0); if (!irq) preempt_check(); return;
    case 0x2f: ret(cur); return;
    case 0x30: case 0x31: {
        int i = A0 ? (int)A0 : cur; if (i <= 0 || i >= MAXT || !th[i].used) { ret(-1); return; }
        if (A1) write_thread_status(i, A1);
        ret(th[i].status); return; }
    case 0x32:   /* SleepThread */
        if (th[cur].wakeups > 0) { th[cur].wakeups--; ret(0); return; }
        block(WT_SLEEP, 0); ret(0); return;
    case 0x33: case 0x34: {
        int i = A0; if (i <= 0 || i >= MAXT || !th[i].used) { ret(-1); return; }
        if ((th[i].status & TS_WAIT) && th[i].wait_type == WT_SLEEP) wake(i); else th[i].wakeups++;
        ret(0); if (!irq) preempt_check(); return; }
    case 0x35: case 0x36: { int i = A0 ? (int)A0 : cur; int w = th[i].wakeups; th[i].wakeups = 0; ret(w); return; }
    case 0x37: case 0x38: { int i = A0; if (th[i].status == TS_WAIT) th[i].status = TS_WAITSUSPEND; else if (th[i].status & (TS_READY | TS_RUN)) th[i].status = TS_SUSPEND;
        ret(0); if (i == cur && !irq) schedule(); return; }
    case 0x39: case 0x3a: { int i = A0; if (th[i].status == TS_WAITSUSPEND) th[i].status = TS_WAIT; else if (th[i].status == TS_SUSPEND) { th[i].status = TS_READY; th[i].order = order_ctr++; }
        ret(0); if (!irq) preempt_check(); return; }
    case 0x3c: ret(setup_thread()); return;
    case 0x3d: heap_start = A0; heap_end = (A1 == 0xffffffffu) ? th[1].stack : A0 + A1;
        if (g_log_sys) LOG("kernel: SetupHeap %08x-%08x\n", heap_start, heap_end);
        ret(heap_start); return;
    case 0x3e: ret(heap_end); return;
    case 0x40: {   /* CreateSema */
        for (int s = 1; s < MAXS; s++) if (!sem[s].used) {
            u32 p = A0; sem[s] = (sema_t){ 1, (int)mr32(p + 8), (int)mr32(p + 4), (int)mr32(p + 8), 0, mr32(p + 16), mr32(p + 20) };
            if (g_log_sys) LOG("kernel: CreateSema %d init=%d max=%d\n", s, sem[s].count, sem[s].max);
            ret(s); return;
        }
        ret(-1); return; }
    case 0x41: if (A0 < MAXS) sem[A0].used = 0; ret(A0); return;
    case 0x42: case 0x43: {   /* SignalSema */
        int s = A0; if (s <= 0 || s >= MAXS || !sem[s].used) { ret(-1); return; }
        int w = sema_waiter(s);
        if (w > 0) wake(w); else if (sem[s].count < sem[s].max || sem[s].max <= 0) sem[s].count++;
        ret(s); if (!irq) preempt_check(); return; }
    case 0x44: {   /* WaitSema */
        int s = A0; if (s <= 0 || s >= MAXS || !sem[s].used) { ret(-1); return; }
        if (sem[s].count > 0) { sem[s].count--; ret(s); return; }
        block(WT_SEMA, s); ret(s); return; }
    case 0x45: case 0x46: {
        int s = A0; if (s <= 0 || s >= MAXS || !sem[s].used) { ret(-1); return; }
        if (sem[s].count > 0) { sem[s].count--; ret(s); } else ret(-1);
        return; }
    case 0x47: case 0x48: {
        int s = A0; if (s <= 0 || s >= MAXS || !sem[s].used) { ret(-1); return; }
        int w = 0; for (int i = 1; i < MAXT; i++) if (th[i].used && (th[i].status & TS_WAIT) && th[i].wait_type == WT_SEMA && th[i].wait_id == s) w++;
        u32 p = A1; mw32(p, sem[s].count); mw32(p + 4, sem[s].max); mw32(p + 8, sem[s].init);
        mw32(p + 12, w); mw32(p + 16, sem[s].attr); mw32(p + 20, sem[s].option);
        ret(s); return; }
    case 0x4b: mw32(A0, 0); ret(0); return;    /* GetOsdConfigParam: English, 4:3 */
    case 0x4a: ret(0); return;
    case 0x64: case 0x68: ret(0); return;      /* FlushCache */
    case 0x70: ret(gs_imr); return;
    case 0x71: { u32 old = gs_imr; gs_imr = A0; ret(old); return; }
    case 0x73: vsync_flag_addr = A0; vsync_field_addr = A1; ret(0); return;
    case 0x74: if (g_log_sys) LOG("kernel: SetSyscall(%x, %08x) ignored\n", A0, A1); ret(0); return;
    case 0x75: { char b[256]; int k = 0; for (; k < 255 && mr8(A0 + k); k++) b[k] = mr8(A0 + k); b[k] = 0; LOG("print: %s", b); ret(0); return; }
    case 0x76: ret(sif_dma_stat(A0)); return;
    case 0x77: ret(sif_set_dma(A0, A1)); return;
    case 0x78: ret(0); return;                 /* SifSetDChain */
    case 0x79: sif_set_reg(A0, A1); ret(0); return;
    case 0x7a: ret(sif_get_reg(A0)); return;
    case 0x7c: ret(deci2_call(A0, A1)); return;
    case 0x7e: ret(0); return;
    /* libkernel 2.4 patches the kernel at start-up (alarm, thread and TLB fixes): 0x5a copies
       its code into kernel memory, 0x5b registers it, 0x83 searches the kernel. The HLE kernel
       needs none of it; the new alarm numbers are mapped at the top of this function. */
    case 0x5a: case 0x5b: case 0x83: ret(0); return;
    case 0x6b: ret(0); return;                 /* sceSifStopDma */
    case 0x7f: ret(32 << 20); return;
    case 0x5c: case 0x5d: case 0x5e: case 0x5f: case 0x60: case 0x61: case 0x62: ret(0); return;
    default:
        LOG("kernel: unhandled syscall %#x%s a0=%08x a1=%08x (pc %08x)\n", n, irq ? " (i)" : "", A0, A1, pc);
        ret(0); return;
    }
}

void kernel_dump(void) {
    LOG("kernel: current thread %d, ra %08x, intr %s\n", cur, GW(31), ee.intr_enabled ? "on" : "off");
    for (int i = 1; i < MAXT; i++) {
        if (!th[i].used) continue;
        const ctx_t *c = &th[i].ctx;
        LOG("  thread %d status %d prio %d wait %d/%d entry %08x ra %08x sp %08x\n", i, th[i].status, th[i].prio,
            th[i].wait_type, th[i].wait_id, th[i].entry, (u32)c->r[31].d[0], (u32)c->r[29].d[0]);
    }
    for (int s = 1; s < MAXS; s++) if (sem[s].used) LOG("  sema %d count %d max %d\n", s, sem[s].count, sem[s].max);
    LOG("  intc mask %04x stat %04x, dmac stat %08x\n", intc_mask, intc_stat, dmac_stat);
}

void kernel_init(void) {
    main_fiber = ConvertThreadToFiberEx(NULL, FIBER_FLAG_FLOAT_SWITCH);
    th[1].used = 1; th[1].status = TS_RUN; th[1].fiber = main_fiber;
    QueryPerformanceFrequency(&perf_freq); QueryPerformanceCounter(&perf_last);
    next_vbon = FRAME_CYC; next_vboff = FRAME_CYC / 12;
    recompute_next();
    extern u32 dmac_ctrl; dmac_ctrl = 1;
}

/* ---- COP0 / misc ---------------------------------------------------------- */
u32 cop0_read(int r) {
    if (r == 9) return (u32)ee.cycles;
    if (r == 15) return 0x2e20;     /* PRId: R5900 */
    return ee.cop0[r];
}
void cop0_write(int r, u32 v) { ee.cop0[r] = v; }
void ee_ei(void) { ee.intr_enabled = 1; if (pending_intc | pending_dmac) ee.next_event = 0; }
void ee_di(void) { ee.intr_enabled = 0; }
void ee_eret(void) { LOG("ee: eret executed\n"); }
int ee_bc0(void) { return 1; }
