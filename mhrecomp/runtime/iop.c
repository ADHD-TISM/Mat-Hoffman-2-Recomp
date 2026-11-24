/* IOP runtime: recompiled IRX modules run here against a high-level IOP
 * kernel (thbase, thsemap, sifcmd, sifman, cdvdman, sysclib, ...). Each IOP
 * thread is a host fiber; the IOP gets to run from the EE event loop and
 * hands control back when every thread is blocked. */
#include "rt.h"
#include "iop.h"
#include <windows.h>
#include <stdlib.h>
#include <stdarg.h>

iop_state iop;
int g_log_iop, g_no_movies;

/* recompiled IOP modules (tools/recomp_iop.py): the game's sound driver */
extern const iop_module iop_mod_ps2sound;
static const iop_module *const modules[] = { &iop_mod_ps2sound };
#define NMOD (sizeof modules / sizeof modules[0])
static int loaded[NMOD];

#define HEAP_BASE 0x000b0000u   /* above PS2SOUND.IRX (0x80000 + 0x2b900 with its bss) */
#define HEAP_END  0x001c0000u   /* above: sif.c heap for the EE (sid 0x80000003) */
static u32 heap_ptr = HEAP_BASE;

enum { TS_RUN = 1, TS_READY = 2, TS_WAIT = 4, TS_DORMANT = 16 };
enum { WT_NONE, WT_SLEEP, WT_SEMA, WT_DELAY, WT_RPCQ, WT_RPCCALL, WT_EVF };
typedef struct {
    int used, status, prio, wait_type, wait_id, yielded, wakeups;
    u32 entry, stack, stack_size, gp, arg;
    u32 evf_bits, evf_mode, evf_res;   /* WT_EVF: what the thread waits for */
    u64 wake_at, order;
    void *fiber;
    iop_state ctx;
} ithread;
typedef struct { int used, count, max; } isema;
typedef struct { int used; u32 bits; } ievf;
static ievf evf[32];
typedef struct { int used; u32 lsn, size, pos; } ifile;
static ifile ifd[16];
/* sceSdSetTransIntrHandler: called after a voice transfer, like an interrupt */
static struct { u32 handler, arg, gp; int pending; } trans_h[2];
typedef struct { u32 sid, func, buf, qd; } iserver;
typedef struct { u32 sid, fn, size, ee_recv, recv_size, echo[4]; } irequest;

#define MAXT 64
static ithread th[MAXT];
static isema sem[128];
static iserver srv[32]; static int nsrv;
static irequest rq[64]; static int rq_head, rq_tail;
static int icur, in_iop;
static void *ret_fiber;
static u64 order_ctr = 1;
static FILE *disc_image;
const char *g_disc_image = "..\\SLUS-20335 (1.00).iso";

#define A0 IR[4]
#define A1 IR[5]
#define A2 IR[6]
#define A3 IR[7]
#define SARG(n) ir32(IR[29] + 16 + 4 * (n))     /* 5th argument onwards */

/* IOP heap: first fit over HEAP_BASE..HEAP_END with real frees. The sound
 * driver allocates and frees a stream buffer set per level; a bump pointer ran
 * out at the first level load. heap_ptr is kept as the highest address used. */
static struct { u32 addr, size; } iblk[256]; static int niblk;
static u32 heap_used(void) { u32 n = 0; for (int i = 0; i < niblk; i++) n += iblk[i].size; return n; }
static u32 heap_alloc(u32 size) {
    size = (size + 15) & ~15u; if (!size) size = 16;
    u32 at = HEAP_BASE;
    for (;;) {
        int hit = 0;
        for (int i = 0; i < niblk; i++)
            if (at < iblk[i].addr + iblk[i].size && iblk[i].addr < at + size) { at = iblk[i].addr + iblk[i].size; hit = 1; }
        if (!hit) break;
    }
    if (at + size > HEAP_END || niblk == 256) { LOG("iop: out of heap (%u bytes asked, %u in use)\n", size, heap_used()); exit(1); }
    iblk[niblk].addr = at; iblk[niblk].size = size; niblk++;
    if (at + size > heap_ptr) heap_ptr = at + size;
    return at;
}
static void heap_free(u32 addr) {
    for (int i = 0; i < niblk; i++) if (iblk[i].addr == addr) { iblk[i] = iblk[--niblk]; return; }
}

/* ---- code lookup ---------------------------------------------------------- */
void iop_call(u32 addr) {
    addr &= 0x1fffff;
    for (unsigned m = 0; m < NMOD; m++) {
        const iop_module *mod = modules[m];
        if (!loaded[m] || addr < mod->base || addr >= mod->base + mod->size) continue;
        unsigned lo = 0, hi = mod->count;
        while (lo < hi) {
            unsigned mid = (lo + hi) / 2;
            if (mod->table[mid].addr == addr) { mod->table[mid].fn(); return; }
            if (mod->table[mid].addr < addr) lo = mid + 1; else hi = mid;
        }
    }
    LOG("iop: call to unknown address %08x (ra %08x)\n", addr, IR[31]);
    exit(3);
}
void iop_bad(u32 pc, u32 w) { LOG("iop: bad instruction %08x at %08x\n", w, pc); exit(4); }
void iop_fallthrough(u32 pc) { LOG("iop: fell off function into %08x\n", pc); exit(3); }
void iop_syscall(u32 pc) { LOG("iop: syscall at %08x\n", pc); }
void iop_break(u32 pc) { LOG("iop: break at %08x\n", pc); }
void iop_div(u32 a, u32 b) {
    if (b == 0) { iop.lo = (s32)a < 0 ? 1 : 0xffffffffu; iop.hi = a; }
    else if (a == 0x80000000u && b == 0xffffffffu) { iop.lo = a; iop.hi = 0; }
    else { iop.lo = (u32)((s32)a / (s32)b); iop.hi = (u32)((s32)a % (s32)b); }
}
void iop_divu(u32 a, u32 b) { if (b == 0) { iop.lo = 0xffffffffu; iop.hi = a; } else { iop.lo = a / b; iop.hi = a % b; } }
u32 iop_cop0_read(int r) { (void)r; return 0; }
void iop_cop0_write(int r, u32 v) { (void)r; (void)v; }
u32 iop_io_read(u32 a, int size) { if (g_log_io) LOG("iop: io read%d %08x\n", size * 8, a); return 0; }
void iop_io_write(u32 a, u32 v, int size) { if (g_log_io) LOG("iop: io write%d %08x = %x\n", size * 8, a, v); }

/* ---- threads ------------------------------------------------------------ */
static void switch_out(void);
static void run_timers(void);

static void WINAPI thread_fiber(void *p) {
    ithread *t = p;
    for (;;) {
        iop_call(t->entry);
        t->status = TS_DORMANT;
        switch_out();
    }
}

static int pick(void) {
    int best = -1;
    for (int i = 1; i < MAXT; i++) {
        if (!th[i].used || th[i].yielded || !(th[i].status & (TS_RUN | TS_READY))) continue;
        if (best < 0 || th[i].prio < th[best].prio ||
            (th[i].prio == th[best].prio && th[i].order < th[best].order)) best = i;
    }
    return best;
}

static void enter(int n) {
    icur = n; th[n].status = TS_RUN; iop = th[n].ctx; iop.spin = 0;
    if (!th[n].fiber) th[n].fiber = CreateFiberEx(64 << 10, 4 << 20, 0, thread_fiber, &th[n]);
    SwitchToFiber(th[n].fiber);
}

/* current thread stops running (blocked, yielded or finished) */
static void switch_out(void) {
    ithread *o = &th[icur];
    o->ctx = iop;
    if (o->status == TS_RUN) o->status = TS_READY;
    int n = pick();
    if (n == icur) { o->status = TS_RUN; return; }
    if (n > 0) enter(n); else SwitchToFiber(ret_fiber);
}

static void block(int wt, int id) {
    th[icur].status = TS_WAIT; th[icur].wait_type = wt; th[icur].wait_id = id;
    switch_out();
}
static void wake(int i) {
    if (th[i].status != TS_WAIT) return;
    th[i].status = TS_READY; th[i].wait_type = WT_NONE; th[i].order = order_ctr++;
}
static void preempt(void) {
    int n = pick();
    if (in_iop && n > 0 && n != icur && th[n].prio < th[icur].prio) switch_out();
}

void iop_spin_yield(void) {
    iop.spin = 0; th[icur].yielded = 1; switch_out();
}

void iop_run(void) {
    if (in_iop) return;
    run_timers();
    int any = 0;
    for (int i = 1; i < MAXT; i++) {
        if (!th[i].used) continue;
        th[i].yielded = 0;
        if (th[i].status == TS_WAIT && th[i].wait_type == WT_DELAY) {
            if (ee.cycles >= th[i].wake_at) wake(i); else ee_set_event(th[i].wake_at);
        }
        if (th[i].status & (TS_RUN | TS_READY)) any = 1;
    }
    if (!any) return;
    in_iop = 1;
    ret_fiber = GetCurrentFiber();
    enter(pick());
    in_iop = 0;
    for (int i = 1; i < MAXT; i++)
        if (th[i].used && ((th[i].status & (TS_RUN | TS_READY)) || (th[i].status == TS_WAIT && th[i].wait_type == WT_DELAY)))
            ee_set_event(th[i].wait_type == WT_DELAY && th[i].status == TS_WAIT ? th[i].wake_at : ee.cycles + 20000);
}

static int create_thread(u32 entry, u32 stack_size, int prio, u32 gp) {
    for (int i = 1; i < MAXT; i++) if (!th[i].used) {
        ithread *t = &th[i]; void *fiber = t->fiber;
        memset(t, 0, sizeof *t); t->fiber = fiber;
        if (stack_size < 0x1000) stack_size = 0x1000;
        t->used = 1; t->status = TS_DORMANT; t->entry = entry; t->prio = prio; t->gp = gp;
        t->stack_size = stack_size; t->stack = heap_alloc(stack_size);
        return i;
    }
    return -1;
}
static void start_thread(int i, u32 a0, u32 a1) {
    ithread *t = &th[i];
    memset(&t->ctx, 0, sizeof t->ctx);
    t->ctx.r[4] = a0; t->ctx.r[5] = a1; t->ctx.r[28] = t->gp;
    t->ctx.r[29] = t->stack + t->stack_size - 0x40;
    t->status = TS_READY; t->order = order_ctr++;
}

/* ---- guest strings / printf ---------------------------------------------- */
static void gstr(u32 a, char *out, int max) {
    int k = 0; for (; k < max - 1; k++) { char c = (char)ir8(a + k); if (!c) break; out[k] = c; } out[k] = 0;
}
/* format a guest printf call into out[1024]; returns the length */
static int guest_format(u32 fmt_addr, int first_arg, char *out_buf) {
    char fmt[512]; gstr(fmt_addr, fmt, sizeof fmt);
    char out[1024];
    int n = 0, arg = first_arg;
    for (const char *p = fmt; *p && n < 1000; p++) {
        if (*p != '%') { out[n++] = *p; continue; }
        char spec[32]; int s = 0; spec[s++] = *p++;
        while (*p && strchr("-+ #0123456789.l", *p) && s < 28) spec[s++] = *p++;
        if (!*p) break;
        spec[s++] = *p; spec[s] = 0;
        u32 v = arg < 4 ? IR[4 + arg] : ir32(IR[29] + 4 * arg);
        if (*p == '%') { out[n++] = '%'; continue; }
        arg++;
        if (*p == 's') { char str[256]; gstr(v, str, sizeof str); n += snprintf(out + n, sizeof out - n, spec, str); }
        else if (strchr("dixXucp", *p)) n += snprintf(out + n, sizeof out - n, spec, v);
        else out[n++] = '?';
        if (n > 1000) n = 1000;   /* snprintf returns the wanted length, not the written one */
        if (n < 0) n = 0;         /* ... or -1 for a bad conversion */
    }
    out[n < 1023 ? n : 1023] = 0;
    memcpy(out_buf, out, 1024);
    return n < 1023 ? n : 1023;
}
static void guest_printf(u32 fmt_addr, int first_arg) {
    char out[1024]; guest_format(fmt_addr, first_arg, out);
    LOG("iop> %s", out);
}

/* ---- CD/DVD ---------------------------------------------------------------- */
static int cd_read(u32 lsn, u32 count, u32 dst) {
    if (!disc_image) {
        disc_image = fopen(g_disc_image, "rb");
        if (!disc_image) { LOG("iop: cannot open disc image %s\n", g_disc_image); exit(1); }
    }
    /* raw .bin (2352-byte MODE2 sectors, data at 24) or a plain .iso (2048-byte sectors) */
    static int iso = -1;
    if (iso < 0) {
        _fseeki64(disc_image, 0, SEEK_END);
        long long n = _ftelli64(disc_image);
        iso = n % 2352 != 0 && n % 2048 == 0;
    }
    /* one seek + read per 64 sectors (was one per sector, ~170 ms at movie starts) */
    static u8 buf[64 * 2352];
    static long long pos = -1;                  /* file position after the last read, to skip seeks */
    int ss = iso ? 2048 : 2352, off = iso ? 0 : 24;
    for (u32 k = 0; k < count; ) {
        u32 n = count - k < 64 ? count - k : 64;
        long long at = (long long)(lsn + k) * ss;
        if (at != pos) _fseeki64(disc_image, at, SEEK_SET);
        size_t got = fread(buf, ss, n, disc_image);
        pos = got == n ? at + (long long)n * ss : -1;
        if (got < n) memset(buf + got * ss, 0, (size_t)(n - got) * ss);
        for (u32 j = 0; j < n; j++, k++) {
            u32 d = dst + k * 2048;
            const u8 *s = buf + j * ss + off;
            if (iop_is_ram(d) && iop_is_ram(d + 2047) && (d & 0x1fffff) + 2048 <= sizeof iop_ram)
                memcpy(iop_ram + (d & 0x1fffff), s, 2048);
            else for (u32 o = 0; o < 2048; o++) iw8(d + o, s[o]);
        }
    }
    return 1;
}

/* ---- SIF RPC bridge --------------------------------------------------------- */
/* ZAUDIO_HLE=1: leave ZAUDIO to the old RPC stub in sif.c (debugging) */
static int zaudio_hle(void) { static int v = -1; if (v < 0) v = getenv("ZAUDIO_HLE") != 0; return v; }
int iop_is_lle_sid(u32 sid) { return sid < 0x80000000u; }   /* the game's own servers (PS2SOUND.IRX) run as recompiled code */

/* Each IOP RPC client gets its own packet buffer (pkt_addr), as in libsif:
 * for rmode-0 calls the EE writes the end packet there instead of sending
 * RPC_END, and sceSifCheckStatRpc looks at it. 32 slots of 0x40 at 0x1f000. */
#define PKT_BASE 0x0001f000u
static u32 pkt_owner[32], rpc_seq;
static u32 client_pkt(u32 c) {
    for (int i = 0; i < 32; i++) if (pkt_owner[i] == c) return PKT_BASE + 0x40u * i;
    for (int i = 0; i < 32; i++) if (!pkt_owner[i]) { pkt_owner[i] = c; return PKT_BASE + 0x40u * i; }
    LOG("iop: out of RPC packet slots\n");
    return PKT_BASE + 0x40u * 31;
}
int iop_server_lookup(u32 sid, u32 *buf) {
    for (int i = 0; i < nsrv; i++) if (srv[i].sid == sid) { *buf = srv[i].buf; return 1; }
    return 0;
}
void iop_rpc_request(u32 sid, u32 fn, u32 size, u32 ee_recv, u32 recv_size, const u32 *echo) {
    irequest *r = &rq[rq_tail++ & 63];
    r->sid = sid; r->fn = fn; r->size = size; r->ee_recv = ee_recv; r->recv_size = recv_size;
    memcpy(r->echo, echo, 16);
    for (int i = 1; i < MAXT; i++) if (th[i].used && th[i].status == TS_WAIT && th[i].wait_type == WT_RPCQ) wake(i);
    ee_set_event(ee.cycles + 2000);
}
/* the EE answered a bind or call made by an IOP client */
void iop_rpc_end(u32 client, u32 cid, u32 server, u32 buff) {
    if (cid == 0x80000009u) { iw32(client + 0x24, server); iw32(client + 0x14, buff); iw32(client + 0x18, buff); }
    iw32(client, 0);
    for (int i = 1; i < MAXT; i++)
        if (th[i].used && th[i].status == TS_WAIT && th[i].wait_type == WT_RPCCALL && (u32)th[i].wait_id == client) wake(i);
    ee_set_event(ee.cycles + 2000);
}

/* Each server thread loops on its own queue (qd): it takes only requests for
 * servers registered on that queue, oldest first. With one shared queue,
 * ZAUDIO's thread ran ZCDDVD's handler with ZAUDIO's gp. */
static int take_request(u32 qd, irequest *out) {
    for (int k = rq_head; k != rq_tail; k++) {
        irequest *r = &rq[k & 63];
        if (!r->sid) continue;                    /* already taken by another queue */
        for (int i = 0; i < nsrv; i++)
            if (srv[i].sid == r->sid && srv[i].qd == qd) {
                *out = *r; r->sid = 0;
                while (rq_head != rq_tail && !rq[rq_head & 63].sid) rq_head++;
                return 1;
            }
    }
    return 0;
}

/* run one request through its server function and answer the EE */
static void exec_request(const irequest *rp) {
    irequest r = *rp;
    iserver *s = 0;
    for (int i = 0; i < nsrv; i++) if (srv[i].sid == r.sid) s = &srv[i];
    if (!s) return;
    if (g_log_sys) LOG("iop: rpc sid=%x fn=%u size=%u : %08x %08x %08x %08x\n", r.sid, r.fn, r.size,
                       ir32(s->buf), ir32(s->buf + 4), ir32(s->buf + 8), ir32(s->buf + 12));
    u32 sp = IR[29], ra = IR[31];
    A0 = r.fn; A1 = s->buf; A2 = r.size;
    iop_call(s->func);
    u32 res = IR[2];
    IR[29] = sp; IR[31] = ra;
    if (r.recv_size && r.ee_recv && res)
        for (u32 k = 0; k < r.recv_size; k++) mw8(r.ee_recv + k, ir8(res + k));
    u32 w[8] = { r.echo[0], r.echo[1], r.echo[2], r.echo[3], 0x8000000Au, 0, s->buf, s->buf };
    sif_send_to_ee(0x80000008u, w, 8);
}

/* sceSifRpcLoop */
static void rpc_loop(u32 qd) {
    for (;;) {
        irequest r;
        if (!take_request(qd, &r)) { block(WT_RPCQ, qd); continue; }
        exec_request(&r);
    }
}

/* sceSifGetNextRequest / sceSifExecRequest: a module with its own polling loop
 * (PS2SOUND.IRX). The handle is an index into a small ring of taken requests. */
static irequest taken[16]; static u32 taken_n;
static u32 next_request(u32 qd) {
    irequest r;
    if (!take_request(qd, &r)) return 0;
    u32 h = taken_n++ & 15;
    taken[h] = r;
    return h + 1;
}

/* ---- imports ---------------------------------------------------------------- */
typedef struct { int used, running; u32 compare, prescale, handler, arg, gp; u64 next; } itimer_t;
static itimer_t itimer[4];
static u64 timer_period(const itimer_t *t);
static void run_timers(void);

static void unknown_import(const char *lib, int idx) {
    static int warned;
    if (warned++ < 40) LOG("iop: unimplemented import %s_%d (ra %08x)\n", lib, idx, IR[31]);
    IR[2] = 0;
}

void iop_import(const char *lib, int idx) {
    iop.spin = 0;
    if (g_log_iop) LOG("iop: %s_%d(%08x, %08x, %08x, %08x) ra %08x thread %d\n", lib, idx, A0, A1, A2, A3, IR[31], icur);
    if (!strcmp(lib, "thbase")) {
        switch (idx) {
        case 4: { int i = create_thread(ir32(A0 + 8), ir32(A0 + 12), (int)ir32(A0 + 16), IR[28]);
            if (g_log_sys) LOG("iop: CreateThread %d entry %08x prio %d\n", i, ir32(A0 + 8), (int)ir32(A0 + 16));
            IR[2] = (u32)i; return; }
        case 5: if (A0 < MAXT) th[A0].used = 0; IR[2] = 0; return;
        case 6: start_thread(A0, A1, 0); IR[2] = 0; preempt(); return;
        case 14: { int i = A0 ? (int)A0 : icur; th[i].prio = (int)A1; IR[2] = 0; preempt(); return; }
        case 20: IR[2] = (u32)icur; return;
        case 34: { u64 c = ee.cycles / 8; iw32(A0, (u32)c); iw32(A0 + 4, (u32)(c >> 32)); IR[2] = 0; return; }   /* GetSystemTime: 36.864 MHz ticks */
        case 24: if (th[icur].wakeups > 0) { th[icur].wakeups--; IR[2] = 0; return; }
                 block(WT_SLEEP, 0); IR[2] = 0; return;
        case 25: case 26: { int i = (int)A0;
            if (th[i].status == TS_WAIT && th[i].wait_type == WT_SLEEP) wake(i); else th[i].wakeups++;
            IR[2] = 0; if (idx == 25) preempt(); return; }
        case 39: { u64 c = (u64)A0 * 36864 / 1000; if (A1) { iw32(A1, (u32)c); iw32(A1 + 4, (u32)(c >> 32)); } IR[2] = 0; return; }   /* USec2SysClock */
        case 40: { u64 c = ir32(A0) | ((u64)ir32(A0 + 4) << 32), us = c * 1000 / 36864;                                         /* SysClock2USec(clk, sec, usec) */
                   if (A1) iw32(A1, (u32)(us / 1000000)); if (A2) iw32(A2, (u32)(us % 1000000)); IR[2] = 0; return; }
        case 33: th[icur].wake_at = ee.cycles + (u64)A0 * 295; ee_set_event(th[icur].wake_at);
                 block(WT_DELAY, 0); IR[2] = 0; return;
        }
    } else if (!strcmp(lib, "thsemap")) {
        switch (idx) {
        case 4: for (int s = 1; s < 128; s++) if (!sem[s].used) {
                sem[s].used = 1; sem[s].count = (int)ir32(A0 + 8); sem[s].max = (int)ir32(A0 + 12);
                IR[2] = (u32)s; return; }
            IR[2] = (u32)-1; return;
        case 5: if (A0 < 128) sem[A0].used = 0; IR[2] = 0; return;
        case 6: case 7: { int s = (int)A0, w = -1;
            if (s <= 0 || s >= 128 || !sem[s].used) { IR[2] = (u32)-408; return; }
            for (int i = 1; i < MAXT; i++)
                if (th[i].used && th[i].status == TS_WAIT && th[i].wait_type == WT_SEMA && th[i].wait_id == s)
                    if (w < 0 || th[i].order < th[w].order) w = i;
            if (w > 0) wake(w); else if (sem[s].count < sem[s].max) sem[s].count++;
            IR[2] = 0; if (idx == 6) preempt(); return; }
        case 8: { int s = (int)A0;
            if (s <= 0 || s >= 128 || !sem[s].used) { IR[2] = (u32)-408; return; }
            if (sem[s].count > 0) { sem[s].count--; IR[2] = 0; return; }
            th[icur].order = order_ctr++; block(WT_SEMA, s); IR[2] = 0; return; }
        case 9: { int s = (int)A0; if (sem[s].count > 0) { sem[s].count--; IR[2] = 0; } else IR[2] = (u32)-420; return; }
        }
    } else if (!strcmp(lib, "intrman")) {
        switch (idx) {
        case 6: case 7: IR[2] = 0; return;                     /* EnableIntr / DisableIntr */
        case 8: case 9: IR[2] = 0; return;
        case 17: if (A0) iw32(A0, 1); IR[2] = 0; return;
        case 18: IR[2] = 0; return;
        case 23: IR[2] = 0; return;
        }
    } else if (!strcmp(lib, "sysmem")) {
        switch (idx) {
        case 4: if (g_log_sys) LOG("iop: AllocSysMemory %u bytes (heap at %x)\n", A1, heap_ptr);
                IR[2] = heap_alloc(A1); return;                /* AllocSysMemory(type, size, addr) */
        case 5: heap_free(A0); IR[2] = 0; return;              /* FreeSysMemory */
        case 6: IR[2] = 2 << 20; return;                       /* QueryMemSize */
        case 7: case 8: IR[2] = HEAP_END - HEAP_BASE - heap_used(); return;   /* QueryMaxFreeMemSize / QueryTotalFreeMemSize */
        }
    } else if (!strcmp(lib, "loadcore")) {
        IR[2] = 0; return;
    } else if (!strcmp(lib, "stdio")) {
        if (idx == 4) { guest_printf(A0, 1); IR[2] = 0; return; }
    } else if (!strcmp(lib, "sysclib")) {
        switch (idx) {
        case 12: for (u32 k = 0; k < A2; k++) iw8(A0 + k, ir8(A1 + k)); IR[2] = A0; return;
        case 14: for (u32 k = 0; k < A2; k++) iw8(A0 + k, (u8)A1); IR[2] = A0; return;
        case 19: {   /* sprintf(dst, fmt, ...): PS2SOUND.IRX imports it; was missing (returned 0, buffer untouched) */
            char out[1024]; u32 dst = A0; int n = guest_format(A1, 2, out);
            for (int k = 0; k <= n; k++) iw8(dst + (u32)k, (u8)out[k]);
            IR[2] = (u32)n; return; }
        case 20: { u32 d = A0; while (ir8(d)) d++; u32 s = A1; u8 c; do { c = ir8(s++); iw8(d++, c); } while (c); IR[2] = A0; return; }
        case 22: { u32 a = A0, b = A1; for (;;) { u8 x = ir8(a++), y = ir8(b++); if (x != y) { IR[2] = (u32)((int)x - (int)y); return; } if (!x) { IR[2] = 0; return; } } }
        case 23: { u32 d = A0, s = A1; u8 c; do { c = ir8(s++); iw8(d++, c); } while (c); IR[2] = A0; return; }
        case 27: { u32 n = 0; while (ir8(A0 + n)) n++; IR[2] = n; return; }
        case 29: { u32 a = A0, b = A1; for (u32 n = A2; n; n--) { u8 x = ir8(a++), y = ir8(b++); if (x != y) { IR[2] = (u32)((int)x - (int)y); return; } if (!x) break; } IR[2] = 0; return; }
        case 30: { u32 k = 0; for (; k < A2; k++) { u8 c = ir8(A1 + k); iw8(A0 + k, c); if (!c) break; } for (; k < A2; k++) iw8(A0 + k, 0); IR[2] = A0; return; }
        }
    } else if (!strcmp(lib, "sifman")) {
        switch (idx) {
        case 5: IR[2] = 0; return;
        case 29: IR[2] = 1; return;
        case 7: {   /* sceSifSetDma: IOP -> EE */
            for (u32 k = 0; k < A1; k++) {
                u32 e = A0 + 16 * k, src = ir32(e), dst = ir32(e + 4), size = ir32(e + 8);
                for (u32 o = 0; o < size; o++) mw8(dst + o, ir8(src + o));
            }
            IR[2] = 1; return; }
        case 8: IR[2] = (u32)-1; return;
        }
    } else if (!strcmp(lib, "sifcmd")) {
        switch (idx) {
        case 14: IR[2] = 0; return;
        case 15: {   /* sceSifBindRpc(client, sid, mode) */
            u32 c = A0; iw32(c, 1); iw32(c + 0x24, 0);
            u32 pkt = client_pkt(c), id = ++rpc_seq;   /* busy for CheckStatRpc until RPC_END */
            iw32(pkt + 0x10, 0x80000000u); iw32(pkt + 0x18, id); iw32(c + 0x4, id);
            u32 w[5] = { 0, pkt, id, c, A1 };
            sif_send_to_ee(0x80000009u, w, 5);
            if (!(A2 & 1)) block(WT_RPCCALL, (int)c);
            IR[2] = 0; return; }
        case 16: {   /* sceSifCallRpc(client, fn, mode, send, ssize, recv, rsize, endfunc, endparam) */
            u32 c = A0, send = A3, ssize = SARG(0), recv = SARG(1), rsize = SARG(2);
            u32 eebuf = ir32(c + 0x14);
            for (u32 o = 0; o < ssize; o++) mw8(eebuf + o, ir8(send + o));
            iw32(c, 1);
            /* rmode tells the EE whether to answer with an RPC_END command: a
             * waiting call needs it to wake up, a no-wait call only if it has
             * an end function. With rmode 0 the EE just writes the end packet
             * to pkt_addr and nobody is told. */
            u32 rmode = (A2 & 1) ? (SARG(3) != 0) : 1;
            u32 pkt = client_pkt(c), id = ++rpc_seq;
            iw32(pkt + 0x10, 0x80000000u); iw32(pkt + 0x18, id);   /* rec_id = PACKET_F_ALLOC, rpc_id */
            iw32(c + 0x4, id);                                       /* cd->hdr.rpc_id */
            u32 w[10] = { 0, pkt, id, c, A1, ssize, recv, rsize, rmode, ir32(c + 0x24) };
            if (g_log_sys) LOG("iop: call EE rpc fn=%u send=%u : %08x %08x %08x %08x\n", A1, ssize, ir32(send), ir32(send + 4), ir32(send + 8), ir32(send + 12));
            sif_send_to_ee(0x8000000Au, w, 10);
            if (!(A2 & 1)) block(WT_RPCCALL, (int)c);
            IR[2] = 0; return; }
        case 17: {   /* sceSifRegisterRpc(sd, sid, func, buf, cfunc, cbuf, qd) */
            if (nsrv < 32) { srv[nsrv].sid = A1; srv[nsrv].func = A2; srv[nsrv].buf = A3; srv[nsrv].qd = SARG(2); nsrv++; }
            LOG("iop: server %x registered (func %08x, buf %08x)\n", A1, A2, A3);
            IR[2] = 0; return; }
        case 18: {   /* sceSifCheckStatRpc: busy until RPC_END, or until the EE wrote the end packet (rmode 0) */
            u32 c = A0, pkt = client_pkt(c);
            if (ir32(c) && ir32(pkt + 0x18) == ir32(c + 0x4) && (ir32(pkt + 0x10) & 0x80000000u)) {
                IR[2] = 1; th[icur].yielded = 1; switch_out();
            } else { iw32(c, 0); IR[2] = 0; }
            return; }
        case 19: IR[2] = 0; return;                                   /* sceSifSetRpcQueue */
        case 20: IR[2] = next_request(A0); return;                    /* sceSifGetNextRequest(qd) */
        case 21: if (A0 >= 1 && A0 <= 16) exec_request(&taken[A0 - 1]); IR[2] = 0; return;   /* sceSifExecRequest */
        case 22: rpc_loop(A0); return;
        }
    } else if (!strcmp(lib, "cdvdman")) {
        switch (idx) {
        case 4: IR[2] = 1; return;                       /* sceCdInit */
        case 6: IR[2] = (u32)cd_read(A0, A1, A2); return; /* sceCdRead */
        case 8: IR[2] = 0; return;                       /* sceCdGetError */
        case 11: IR[2] = 0; return;                      /* sceCdSync */
        case 12: IR[2] = 0x12; return;                   /* sceCdGetDiskType: PS2 CD */
        case 13: IR[2] = 2; return;                      /* sceCdDiskReady: complete */
        case 14: if (A1) iw32(A1, 0); IR[2] = 1; return; /* sceCdTrayReq */
        case 15: IR[2] = 1; return;                      /* sceCdStop */
        case 75: IR[2] = 1; return;                      /* sceCdMmode */
        }
    } else if (!strcmp(lib, "libsd")) {   /* high-level libsd over spu2.c (LIBSD.IRX itself is not run) */
        switch (idx) {
        case 4: spu2_reset(); IR[2] = 0; return;                                   /* sceSdInit */
        case 5: spu2_set_param(A0 & 0xffff, A1 & 0xffff); IR[2] = 0; return;      /* sceSdSetParam */
        case 6: IR[2] = spu2_get_param(A0 & 0xffff); return;                      /* sceSdGetParam */
        case 7: spu2_set_switch(A0 & 0xffff, A1); IR[2] = 0; return;              /* sceSdSetSwitch */
        case 8: IR[2] = spu2_get_switch(A0 & 0xffff); return;                     /* sceSdGetSwitch */
        case 9: spu2_set_addr(A0 & 0xffff, A1); IR[2] = 0; return;                /* sceSdSetAddr */
        case 10: IR[2] = spu2_get_addr(A0 & 0xffff); return;                      /* sceSdGetAddr */
        case 11: spu2_core_attr(A0 & 0xffff, A1); IR[2] = 0; return;              /* sceSdSetCoreAttr */
        case 12: IR[2] = 0; return;                                               /* sceSdGetCoreAttr */
        case 17: IR[2] = spu2_voice_trans((int)A0, (int)A1, A2, A3, SARG(0));           /* sceSdVoiceTrans(ch, mode, iop, spu, size) */
                 trans_h[A0 & 1].pending = 1; ee_set_event(ee.cycles + 20000); return;
        case 21: IR[2] = 0; return;                                               /* sceSdSetTransCallback */
        case 22: IR[2] = 0; return;                                               /* sceSdSetIRQCallback */
        case 23: case 24: case 25: IR[2] = 0; return;                             /* effect attributes, work area: no reverb */
        case 26: trans_h[A0 & 1].handler = A1; trans_h[A0 & 1].arg = A2; trans_h[A0 & 1].gp = IR[28]; IR[2] = 0; return;   /* sceSdSetTransIntrHandler(ch, func, arg) */
        case 27: IR[2] = 0; return;                                               /* sceSdSetSpu2IntrHandler */
        case 19: IR[2] = 1; return;                                               /* sceSdVoiceTransStatus: done at once */
        case 18: { u32 ch = A0 & 1, mode = A1;                                    /* sceSdBlockTrans(ch, mode, addr, size) */
            if ((mode & 3) == 2) spu2_block_stop((int)ch); else spu2_block_start((int)ch, A2, A3);
            IR[2] = 0; return; }
        case 20: IR[2] = spu2_block_status((int)(A0 & 1)); return;                /* sceSdBlockTransStatus */
        }
    } else if (!strcmp(lib, "thevent")) {
        switch (idx) {
        case 4: for (int e = 1; e < 32; e++) if (!evf[e].used) {                     /* CreateEventFlag({attr, option, bits}) */
                evf[e].used = 1; evf[e].bits = ir32(A0 + 8); IR[2] = (u32)e; return; }
            IR[2] = (u32)-1; return;
        case 5: if (A0 < 32) evf[A0].used = 0; IR[2] = 0; return;
        case 6: case 7: { int e = (int)A0;                                           /* SetEventFlag / iSetEventFlag */
            if (e <= 0 || e >= 32 || !evf[e].used) { IR[2] = (u32)-409; return; }
            evf[e].bits |= A1;
            for (int i = 1; i < MAXT; i++) {
                ithread *t = &th[i];
                if (!t->used || t->status != TS_WAIT || t->wait_type != WT_EVF || t->wait_id != e) continue;
                u32 hit = evf[e].bits & t->evf_bits;
                if ((t->evf_mode & 1) ? !hit : hit != t->evf_bits) continue;
                if (t->evf_res) iw32(t->evf_res, evf[e].bits);
                if (t->evf_mode & 0x10) evf[e].bits = 0;
                wake(i);
            }
            IR[2] = 0; if (idx == 6) preempt(); return; }
        case 8: case 9: if (A0 < 32) evf[A0].bits &= A1; IR[2] = 0; return;          /* ClearEventFlag */
        case 10: case 11: { int e = (int)A0;                                         /* WaitEventFlag(id, bits, mode, result) / Poll */
            if (e <= 0 || e >= 32 || !evf[e].used) { IR[2] = (u32)-409; return; }
            u32 hit = evf[e].bits & A1;
            if ((A2 & 1) ? hit : hit == A1) {
                if (A3) iw32(A3, evf[e].bits);
                if (A2 & 0x10) evf[e].bits = 0;
                IR[2] = 0; return;
            }
            if (idx == 11) { IR[2] = (u32)-421; return; }
            th[icur].evf_bits = A1; th[icur].evf_mode = A2; th[icur].evf_res = A3;
            block(WT_EVF, e); IR[2] = 0; return; }
        }
    } else if (!strcmp(lib, "ioman")) {       /* files on the disc (cdrom0:), read only */
        int cdvd_find(const char *path, u32 *lsn, u32 *size);
        void cdvd_read_host(u32 lsn, u32 count, u8 *dst);
        switch (idx) {
        case 4: { char name[256]; gstr(A0, name, sizeof name);                      /* open(name, flags) */
            u32 lsn, size;
            if (!cdvd_find(name, &lsn, &size)) { LOG("iop: open '%s' failed\n", name); IR[2] = (u32)-2; return; }
            for (int f = 1; f < 16; f++) if (!ifd[f].used) {
                ifd[f].used = 1; ifd[f].lsn = lsn; ifd[f].size = size; ifd[f].pos = 0;
                if (g_log_sys) LOG("iop: open '%s' -> fd %d (%u bytes)\n", name, f, size);
                IR[2] = (u32)f; return; }
            IR[2] = (u32)-24; return; }
        case 5: if (A0 < 16) ifd[A0].used = 0; IR[2] = 0; return;                    /* close */
        case 6: { ifile *f = A0 < 16 ? &ifd[A0] : 0;                                 /* read(fd, buf, size) */
            if (!f || !f->used) { IR[2] = (u32)-9; return; }
            u32 n = A2; if (f->pos > f->size) n = 0; else if (n > f->size - f->pos) n = f->size - f->pos;
            static u8 sec[2048];
            for (u32 done = 0; done < n; ) {
                u32 at = f->pos + done, o = at & 2047, c = 2048 - o; if (c > n - done) c = n - done;
                cdvd_read_host(f->lsn + at / 2048, 1, sec);
                for (u32 k = 0; k < c; k++) iw8(A1 + done + k, sec[o + k]);
                done += c;
            }
            f->pos += n; IR[2] = n; return; }
        case 8: { ifile *f = A0 < 16 ? &ifd[A0] : 0;                                 /* lseek(fd, offset, whence) */
            if (!f || !f->used) { IR[2] = (u32)-9; return; }
            s32 off = (s32)A1;
            f->pos = A2 == 0 ? (u32)off : A2 == 1 ? f->pos + (u32)off : f->size + (u32)off;
            IR[2] = f->pos; return; }
        }
    } else if (!strcmp(lib, "timrman")) {   /* hardware timers: periodic handler, run from iop_run */
        switch (idx) {
        case 4: { for (int k = 0; k < 4; k++) if (!itimer[k].used) { memset(&itimer[k], 0, sizeof itimer[k]); itimer[k].used = 1; IR[2] = (u32)(k + 1); return; }
                  IR[2] = (u32)-1; return; }                                      /* AllocHardTimer(source, size, prescale) */
        case 6: if (A0 >= 1 && A0 <= 4) itimer[A0 - 1].used = 0; IR[2] = 0; return;   /* FreeHardTimer */
        case 20: if (A0 >= 1 && A0 <= 4) { itimer_t *t = &itimer[A0 - 1];         /* SetTimerHandler(id, compare, handler, arg) */
                     t->compare = A1; t->handler = A2; t->arg = A3; t->gp = IR[28]; }
                 IR[2] = 0; return;
        case 22: if (A0 >= 1 && A0 <= 4) itimer[A0 - 1].prescale = A3 ? A3 : 1;    /* SetupHardTimer(id, source, mode, prescale) */
                 IR[2] = 0; return;
        case 23: if (A0 >= 1 && A0 <= 4) { itimer_t *t = &itimer[A0 - 1];         /* StartHardTimer */
                     t->running = 1; t->next = ee.cycles + timer_period(t); ee_set_event(t->next); }
                 IR[2] = 0; return;
        case 24: if (A0 >= 1 && A0 <= 4) itimer[A0 - 1].running = 0; IR[2] = 0; return;   /* StopHardTimer */
        }
    }
    unknown_import(lib, idx);
}

/* IOP hardware timers (timrman). The handler runs like an interrupt handler,
 * outside any thread, on its own stack; it returns the next compare value
 * (0 stops the timer). The IOP sysclock is 36.864 MHz, 8 EE cycles a tick. */
static u64 timer_period(const itimer_t *t) {
    u64 c = (u64)t->compare * (t->prescale ? t->prescale : 1) * 8;
    return c ? c : 8 * 36864;
}
static void run_trans_handlers(void) {
    for (int c = 0; c < 2; c++) {
        if (!trans_h[c].pending) continue;
        trans_h[c].pending = 0;
        if (!trans_h[c].handler) continue;
        static u32 istack;
        if (!istack) istack = heap_alloc(0x1000);
        iop_state save = iop;
        memset(&iop, 0, sizeof iop);
        IR[4] = (u32)c; IR[5] = trans_h[c].arg; IR[28] = trans_h[c].gp; IR[29] = istack + 0x1000 - 0x40;
        iop_call(trans_h[c].handler);
        iop = save;
    }
}
static void run_timers(void) {
    run_trans_handlers();
    for (int k = 0; k < 4; k++) {
        itimer_t *t = &itimer[k];
        if (!t->used || !t->running || !t->handler) continue;
        int guard = 0;
        while (t->running && ee.cycles >= t->next && guard++ < 8) {
            static u32 istack;
            if (!istack) istack = heap_alloc(0x1000);
            iop_state save = iop;
            memset(&iop, 0, sizeof iop);
            IR[4] = t->arg; IR[28] = t->gp; IR[29] = istack + 0x1000 - 0x40;
            iop_call(t->handler);
            u32 next = IR[2];
            iop = save;
            if (next) t->compare = next; else t->running = 0;
            t->next += timer_period(t);
        }
        if (t->running) { if (ee.cycles >= t->next) t->next = ee.cycles + timer_period(t); ee_set_event(t->next); }
    }
}

/* ---- module loading ----------------------------------------------------------- */
int iop_load_module(const char *path) {
    char up[128]; int k = 0;
    for (; path[k] && k < 127; k++) up[k] = (path[k] >= 'a' && path[k] <= 'z') ? path[k] - 32 : path[k];
    up[k] = 0;
    for (unsigned m = 0; m < NMOD; m++) {
        if (!strstr(up, modules[m]->name)) continue;
        if (0) return 0;
        memcpy(iop_ram + modules[m]->base, modules[m]->image, modules[m]->size);
        loaded[m] = 1;
        int t = create_thread(modules[m]->entry, 0x4000, 8, modules[m]->gp);
        start_thread(t, 0, 0);
        LOG("iop: module %s loaded at %08x, start thread %d\n", modules[m]->name, modules[m]->base, t);
        ee_set_event(ee.cycles + 2000);
        return 1;
    }
    return 0;
}

void iop_reset(void) {
    for (int i = 1; i < MAXT; i++) {
        th[i].used = 0;
        if (th[i].fiber) { DeleteFiber(th[i].fiber); th[i].fiber = 0; }   /* always called from the EE side */
    }
    memset(sem, 0, sizeof sem); memset(loaded, 0, sizeof loaded);
    nsrv = 0; rq_head = rq_tail = 0; heap_ptr = HEAP_BASE; niblk = 0; memset(pkt_owner, 0, sizeof pkt_owner);
}
