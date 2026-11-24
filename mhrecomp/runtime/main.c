/* Entry point: load the ELF image into EE RAM and run the recompiled code. */
#include "rt.h"
#include "config.h"
#include <stdlib.h>
#include <math.h>
#include <windows.h>

ee_state ee;
int g_log_sys, g_log_io, g_turbo;
static u64 max_frames;

static const char *elf_name = "SLUS_203.35";

static int show_window = 1;

/* The game reached EE code the recompiler did not generate. In a windowed run
 * say so on screen instead of just vanishing. */
static void missing_code(u32 addr, const char *what) {
    FILE *m = fopen("missing_entries.txt", "a");
    if (m) { fprintf(m, "%08x\n", addr); fclose(m); }
    if (show_window) {
        char msg[1024], cwd[MAX_PATH] = "";
        GetCurrentDirectoryA(sizeof cwd, cwd);
        snprintf(msg, sizeof msg, "The game %s %08x, which was not recompiled, and has to stop.\n\n"
                 "The address was added to %s\\missing_entries.txt. Please report it. (Developers: add it "
                 "to mhrecomp\\extra_ee.txt and build again; the game then gets past this point.)", what, addr, cwd);
        MessageBoxA(NULL, msg, "Mat Hoffman's Pro BMX 2: missing code", MB_OK | MB_ICONERROR);
    }
    exit(3);
}

ee_fn ee_lookup(u32 addr) {
    unsigned lo = 0, hi = ee_table_count;
    while (lo < hi) {
        unsigned m = (lo + hi) / 2;
        if (ee_table[m].addr == addr) return ee_table[m].fn;
        if (ee_table[m].addr < addr) lo = m + 1; else hi = m;
    }
    return 0;
}

void ee_call(u32 addr) {
    ee_fn f = ee_lookup(addr & 0x1fffffff);
    if (!f) {
        LOG("ee: call to unknown address %08x (ra %08x)\n", addr, GW(31));
        missing_code(addr, "called");
    }
    f();
}

void call_guest(u32 addr) { ee_call(addr); }

void ee_bad(u32 pc, u32 w) { LOG("ee: bad instruction %08x at %08x\n", w, pc); exit(4); }
void ee_fallthrough(u32 pc) {
    LOG("ee: fell off function into %08x\n", pc);
    missing_code(pc, "ran into");
}
void ee_break(u32 pc, u32 code) { LOG("ee: break %u at %08x\n", code, pc); }

static void load_elf(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { LOG("cannot open %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    u8 *d = malloc(n); if (fread(d, 1, n, f) != (size_t)n) exit(1); fclose(f);
    u32 phoff; u16 phnum; memcpy(&phoff, d + 0x1c, 4); memcpy(&phnum, d + 0x2c, 2);
    for (int k = 0; k < phnum; k++) {
        u32 ph[8]; memcpy(ph, d + phoff + 32 * k, 32);
        if (ph[0] != 1) continue;
        memcpy(ee_ram + (ph[2] & 0x1ffffff), d + ph[1], ph[4]);
        memset(ee_ram + (ph[2] & 0x1ffffff) + ph[4], 0, ph[5] - ph[4]);
    }
    free(d);
}

#ifdef EE_WATCH
u32 ee_watch_lo = 0xffffffffu, ee_watch_hi;
static int watch_init;
void ee_watch_hit(u32 a, int n, u64 v) {
    static int hits;
    if (hits++ > 400) return;
#ifdef EE_TRACE
    extern u32 ee_trace_buf[1 << 22]; extern u32 ee_trace_pos;
    LOG("watch: frame %llu write%d %08x = %llx  (fn", (unsigned long long)g_frame, n * 8, a, (unsigned long long)v);
    for (int k = 1; k <= 10; k++) LOG(" %08x", ee_trace_buf[(ee_trace_pos - k) & 0x3fffff]);   /* most recent first */
    LOG(") a0 %08x a1 %08x a2 %08x s4 %08x ra %08x\n", GW(4), GW(5), GW(6), GW(20), GW(31));
#else
    LOG("watch: frame %llu write%d %08x = %llx  ra %08x\n", (unsigned long long)g_frame, n * 8, a, (unsigned long long)v, GW(31));
#endif
}
static void watch_setup(void) {
    const char *e = getenv("EE_WATCH");
    if (watch_init++ || !e) return;
    char *c; u32 lo = (u32)strtoul(e, &c, 16), len = *c == ':' ? (u32)strtoul(c + 1, 0, 16) : 8;
    ee_watch_lo = lo & 0x1ffffff; ee_watch_hi = ee_watch_lo + len;
    LOG("watch: EE stores to %08x..%08x\n", ee_watch_lo, ee_watch_hi);
}
#else
static void watch_setup(void) {}
#endif

#ifdef EE_TRACE
u32 ee_trace_buf[1 << 22]; u32 ee_trace_pos;
u32 ee_count_addr[8], ee_count_hits[8]; int ee_count_n;
static void count_init(void) __attribute__((constructor));
static void count_init(void) {
    const char *e = getenv("EE_COUNT");
    while (e && *e && ee_count_n < 8) { char *x; ee_count_addr[ee_count_n++] = (u32)strtoul(e, &x, 16); e = *x == ',' ? x + 1 : x; if (x == e && *e) break; }
}
static void trace_dump(void) {
    for (int i = 0; i < ee_count_n; i++) LOG("count: fn %08x entered %u times\n", ee_count_addr[i], ee_count_hits[i]);
    if (getenv("EE_COUNT")) return;     /* counting only: skip the 4M-line trace file */
    FILE *f = fopen("trace.txt", "w");
    if (!f) return;
    u32 n = ee_trace_pos < (1u << 22) ? ee_trace_pos : (1u << 22);
    for (u32 k = ee_trace_pos - n; k != ee_trace_pos; k++) fprintf(f, "%08x\n", ee_trace_buf[k & 0x3fffff]);
    fclose(f);
    LOG("trace: %u entries written to trace.txt\n", n);
}
#else
static void trace_dump(void) {}
#endif

/* entry hooks: tools/recomp_ee.py emits ee_hook(addr) at the start of the
 * functions in its HOOKS set, so the runtime can look at or adjust arguments.
 * No game hooks yet (widescreen will go here); a hooked function logs its
 * arguments, which is the debug use: add the address to HOOKS and regenerate. */
volatile int g_ws_active = -1;
int ws_active(void) { return g_ws_active < 0 ? g_cfg.widescreen : g_ws_active; }

/* debug: tools/recomp_ee.py --bbtrace ADDR logs every basic block of a function */
void ee_bb(u32 a) {
    static long long from = -1, to; if (from < 0) { from = getenv("BB_FROM") ? atoll(getenv("BB_FROM")) : 0; to = getenv("BB_TO") ? atoll(getenv("BB_TO")) : (1ll << 62); }
    if ((long long)g_frame < from || (long long)g_frame > to) return;   /* BB_FROM / BB_TO=<frame>: trace only that range */
    LOG("bb %08x v0 %08x v1 %08x a0 %08x a1 %08x s0 %08x s1 %08x sp %08x\n", a, GW(2), GW(3), GW(4), GW(5), GW(16), GW(17), GW(29));
}

/* MH2_UNLOCK=1 (testing): fn 125040 reads global\famePoints.txt, the fame each
 * road trip city needs, into its object at +732..+760 (8 words). The hook
 * remembers the object (again at every call); check_frame_limit zeroes those
 * words while they still hold fame values (0/100/200/400, famePoints.txt), so
 * a freed and reused object is left alone. MH2_UNLOCK unset or 0: off. */
static u32 fame_obj;
static int unlock_on(void) { static int on = -1; if (on < 0) { const char *e = getenv("MH2_UNLOCK"); on = e && *e && *e != '0'; } return on; }
int ee_hook(u32 a) {
    if (a == 0x00125040u) {
        if (unlock_on()) { fame_obj = GW(4); LOG("unlock: fame table at %08x\n", fame_obj); }
        return 0;
    }
    LOG("hook %08x: a0 %08x a1 %08x a2 %08x a3 %08x ra %08x\n", a, GW(4), GW(5), GW(6), GW(7), GW(31));
    return 0;
}

static void on_exit_report(void) { trace_dump(); fflush(stdout); fflush(stderr); }

void check_frame_limit(void) {
    if (fame_obj) {
        int fame = 1;
        for (int k = 0; k < 8; k++) { u32 w = mr32(fame_obj + 732 + 4 * k); if (w != 0 && w != 100 && w != 200 && w != 400) fame = 0; }
        /* not fame values (still loading, or the memory was reused): leave it */
        if (fame) for (int k = 0; k < 8; k++) mw32(fame_obj + 732 + 4 * k, 0);
    }
    if (max_frames && g_frame >= max_frames) {
        extern void kernel_dump(void);
        LOG("frame limit reached\n"); kernel_dump(); ipu_stats();
        if (getenv("EE_DUMP")) { FILE *f = fopen(getenv("EE_DUMP"), "wb"); if (f) { fwrite(ee_ram, 1, 32 << 20, f); fclose(f); } }
        gst_sync(); gst_stats();
        { void gsgl_stats(void), vif_stats(void), vu_stats(void); gsgl_stats(); vif_stats(); vu_stats(); spu2_stats(); }
        { extern u64 n_poll_skips, n_poll_skipped_cycles;
          LOG("hw: %llu poll loops skipped ahead, %.1f s of emulated time\n", (unsigned long long)n_poll_skips, n_poll_skipped_cycles / 294912000.0); }
        exit(0);
    }
}

int main(int argc, char **argv) {
    const char *disc = ".";
    int gs_scale = 2, scale_given = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--log-sys") && i + 1 < argc) g_log_sys = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--log-io")) g_log_io = 1;
        else if (!strcmp(argv[i], "--log-ipu")) { extern int g_log_ipu; g_log_ipu = 1; }
        else if (!strcmp(argv[i], "--no-movies")) { extern int g_no_movies; g_no_movies = 1; }
        else if (!strcmp(argv[i], "--log-iop")) { extern int g_log_iop; g_log_iop = 1; }
        else if (!strcmp(argv[i], "--turbo")) { g_turbo = 1; extern int g_turbo_cli; g_turbo_cli = 1; }
        else if (!strcmp(argv[i], "--no-audio")) g_no_audio = 1;
        else if (!strcmp(argv[i], "--log-mc")) { extern int g_log_mc; g_log_mc = 1; }
        else if (!strcmp(argv[i], "--log-frames")) { extern int g_log_frames; g_log_frames = 1; }
        else if (!strcmp(argv[i], "--log-spu") && i + 1 < argc) g_log_spu = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--log-pad")) { extern int g_log_pad; g_log_pad = 1; }
        else if (!strcmp(argv[i], "--no-window")) show_window = 0;
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) { gs_scale = atoi(argv[++i]); scale_given = 1; }
        else if (!strcmp(argv[i], "--image") && i + 1 < argc) {
            /* made absolute now: the disc folder below becomes the current directory */
            static char full[MAX_PATH]; extern const char *g_disc_image;
            g_disc_image = GetFullPathNameA(argv[++i], MAX_PATH, full, NULL) ? full : argv[i];
        }
        else if (!strcmp(argv[i], "--log-vif") && i + 1 < argc) { extern int g_log_vif; g_log_vif = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--log-gs") && i + 1 < argc) { extern int g_log_gs; g_log_gs = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) max_frames = strtoull(argv[++i], 0, 10);
        else disc = argv[i];
    }
    setvbuf(stderr, NULL, _IONBF, 0);
    atexit(on_exit_report);
    { void prof_init(void); prof_init(); }
    watch_setup();
    if (!SetCurrentDirectoryA(disc)) { LOG("cannot enter disc folder %s\n", disc); return 1; }
    config_load("mh2.ini");
    if (g_cfg.scale && !scale_given) gs_scale = g_cfg.scale;
    if (!g_cfg.frame_limit) g_turbo = 1;
    hw_init();
    { void ipu_init(void); ipu_init(); }
    { void gs_init(int, int); gs_init(show_window, gs_scale); }
    gst_init();
    spu2_init();
    load_elf(elf_name);
    sif_init();
    kernel_init();
    ee.fcr31 = 0x01000001;
    GD(29) = 0x01f00000;
    LOG("boot: entry %08x, %u functions\n", ee_entry, ee_table_count);
    ee_call(ee_entry);
    LOG("boot: entry returned\n");
    return 0;
}
