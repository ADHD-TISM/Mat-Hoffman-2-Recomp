/* In-game menu: Esc (keyboard) or Back+Start (gamepad) pauses the game and
 * shows settings and Exit over the picture.
 *
 * The menu runs on the EE thread at a vblank (menu_vsync, from kernel.c): the
 * game does not run while it is open. Each pass presents the last frame again
 * with the overlay (gs_present_again); the GS stage (or the EE thread without
 * the graphics thread) draws it from menu_render(), a bitmap drawn with GDI.
 * Settings apply at once; the ones changed here are written to mh2.ini when
 * the menu closes (config_save). Keys come from the window (menu_on_key, in
 * the window procedure on this thread), pad buttons are polled. */
#include "rt.h"
#include "config.h"
#include <windows.h>
#include <stdlib.h>

/* other files */
void gsgl_pump(void);
int gsgl_has_focus(void);
int gsgl_window_shown(void);
int gsgl_is_fullscreen(void);
void gsgl_set_fullscreen(int on);
void gsgl_recheck_vsync(void);
void gsgl_request_scale(int s);
int gsgl_scale(void);
void gs_present_again(void);
int pad_menu_buttons(void);
void pad_rumble_stop(void);
int config_save(const char *const *keys, const char *const *vals, int n);
int ws_active(void);
extern int g_pace_restart;
extern int g_spu_volume;
int g_turbo_cli;                   /* --turbo (scripted runs): no menu; frame_limit = 0 keeps it */

volatile int g_menu_open;          /* spu2.c: no underrun counting while paused */
volatile int g_menu_visible;       /* gs_gl.c: draw the overlay */
static volatile int open_request;

/* ---- items ---------------------------------------------------------------------- */
enum { IT_RESUME, IT_HDR_DISPLAY, IT_WIDE, IT_ASPECT, IT_FILTER, IT_FULL, IT_VSYNC, IT_SCALE,
       IT_HDR_SOUND, IT_VOLUME, IT_HDR_CONTROLS, IT_PAD, IT_RUMBLE, IT_PAD2, IT_EXIT, IT_N };
static const char *const label[IT_N] = {
    "Resume", "Display", "Widescreen", "Picture shape", "Filter", "Fullscreen", "Vsync", "3D resolution",
    "Sound", "Volume", "Controls", "Gamepad", "Vibration", "Player 2 pad", "Exit game",
};
static int is_header(int i) { return i == IT_HDR_DISPLAY || i == IT_HDR_SOUND || i == IT_HDR_CONTROLS; }
static const char *const ini_key[IT_N] = {
    [IT_WIDE] = "widescreen", [IT_ASPECT] = "aspect", [IT_FILTER] = "filter", [IT_FULL] = "fullscreen",
    [IT_VSYNC] = "vsync", [IT_SCALE] = "scale", [IT_VOLUME] = "volume", [IT_PAD] = "gamepad",
    [IT_RUMBLE] = "rumble", [IT_PAD2] = "pad2",
};

static int sel = IT_RESUME, exit_armed, dirty[IT_N];

static void value_text(int i, char *out, size_t n) {
    out[0] = 0;
    switch (i) {
    case IT_WIDE: snprintf(out, n, "%s%s", g_cfg.widescreen ? "On (16:9)" : "Off (4:3)",
                           g_cfg.widescreen != ws_active() ? ", next race" : ""); break;
    case IT_ASPECT: snprintf(out, n, "%s", g_cfg.stretch ? "Stretch to window" : "Keep shape"); break;
    case IT_FILTER: snprintf(out, n, "%s", g_cfg.smooth ? "Smooth" : "Sharp pixels"); break;
    case IT_FULL: snprintf(out, n, "%s", gsgl_is_fullscreen() ? "On" : "Off"); break;
    case IT_VSYNC: snprintf(out, n, "%s", g_cfg.vsync ? "On" : "Off"); break;
    case IT_SCALE: snprintf(out, n, "%dx%s", gsgl_scale(), gsgl_scale() == 1 ? " (PS2)" : ""); break;
    case IT_VOLUME: snprintf(out, n, "%d%%", g_spu_volume); break;
    case IT_PAD: snprintf(out, n, "%s", g_cfg.gamepad ? "On" : "Off"); break;
    case IT_RUMBLE: snprintf(out, n, "%s", g_cfg.rumble ? "On" : "Off"); break;
    case IT_PAD2: snprintf(out, n, "%s", g_cfg.pad2 == 0 ? "Off" : g_cfg.pad2 == 2 ? "First gamepad" : "Second gamepad"); break;
    case IT_EXIT: snprintf(out, n, "%s", exit_armed ? "Press again to exit" : ""); break;
    }
}

/* the ini value of a setting, as the launcher writes it */
static void ini_value(int i, char *out, size_t n) {
    switch (i) {
    case IT_ASPECT: snprintf(out, n, "%s", g_cfg.stretch ? "stretch" : "4:3"); break;
    case IT_FILTER: snprintf(out, n, "%s", g_cfg.smooth ? "smooth" : "nearest"); break;
    case IT_FULL: snprintf(out, n, "%d", gsgl_is_fullscreen()); break;
    case IT_SCALE: snprintf(out, n, "%d", gsgl_scale()); break;
    case IT_VOLUME: snprintf(out, n, "%d", g_spu_volume); break;
    case IT_PAD2: snprintf(out, n, "%s", g_cfg.pad2 == 0 ? "off" : g_cfg.pad2 == 2 ? "gamepad1" : "gamepad2"); break;
    default: snprintf(out, n, "%d", i == IT_WIDE ? g_cfg.widescreen : i == IT_VSYNC ? g_cfg.vsync :
                      i == IT_PAD ? g_cfg.gamepad : g_cfg.rumble); break;
    }
}

/* left / right on item i (dir -1 / +1) */
static void change(int i, int dir) {
    switch (i) {
    case IT_WIDE: g_cfg.widescreen = !g_cfg.widescreen; break;
    case IT_ASPECT: g_cfg.stretch = !g_cfg.stretch; break;
    case IT_FILTER: g_cfg.smooth = !g_cfg.smooth; break;
    case IT_FULL: gsgl_set_fullscreen(!gsgl_is_fullscreen()); break;
    case IT_VSYNC: g_cfg.vsync = !g_cfg.vsync; gsgl_recheck_vsync(); break;
    case IT_SCALE: { int s = gsgl_scale() + dir; if (s < 1) s = 4; if (s > 4) s = 1; gsgl_request_scale(s); break; }
    case IT_VOLUME: { int v = g_spu_volume + dir * 10; if (v < 0) v = 0; if (v > 100) v = 100;
                      if (v == g_spu_volume) return; g_spu_volume = v; break; }
    case IT_PAD: g_cfg.gamepad = !g_cfg.gamepad; break;
    case IT_RUMBLE: g_cfg.rumble = !g_cfg.rumble; break;
    case IT_PAD2: g_cfg.pad2 = (g_cfg.pad2 + (dir > 0 ? 1 : 2)) % 3; break;
    default: return;
    }
    dirty[i] = 1;
}

/* ---- the picture: GDI into a 32-bit bitmap ------------------------------------------- */
static SRWLOCK lock = SRWLOCK_INIT;
static volatile LONG version = 1;
static void touched(void) { InterlockedIncrement(&version); }
int menu_version(void) { return (int)version; }

#define MW 720
#define ROW 44
static int menu_h(void) { return 96 + IT_N * ROW + 64; }
void menu_size(int *w, int *h) { *w = MW; *h = menu_h(); }

/* rows / values copied under the lock: the renderer may run on the GS stage */
static struct { int sel; char val[IT_N][48]; } snap;
static void snapshot(void) {
    char val[IT_N][48] = { { 0 } };          /* zeroed past each string: compared whole below */
    for (int i = 0; i < IT_N; i++) value_text(i, val[i], sizeof val[i]);
    AcquireSRWLockExclusive(&lock);
    int same = snap.sel == sel && !memcmp(snap.val, val, sizeof val);
    snap.sel = sel; memcpy(snap.val, val, sizeof val);
    ReleaseSRWLockExclusive(&lock);
    if (!same) touched();   /* the presenting thread draws and uploads it again */
}

/* pix: MW x menu_h() pixels, top row first, RGBA in memory order (alpha set) */
void menu_render(u32 *pix) {
    static HDC mdc; static HBITMAP bmp; static u32 *bits; static HFONT f_big, f_norm, f_bold, f_small;
    int w = MW, h = menu_h();
    if (!mdc) {
        mdc = CreateCompatibleDC(NULL);
        BITMAPINFO bi = { 0 };
        bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        bmp = CreateDIBSection(mdc, &bi, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
        SelectObject(mdc, bmp);
        f_big = CreateFontA(-40, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
        f_norm = CreateFontA(-26, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
        f_bold = CreateFontA(-22, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
        f_small = CreateFontA(-20, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
    }
    if (!bits) { memset(pix, 0, (size_t)w * h * 4); return; }
    AcquireSRWLockShared(&lock);
    int s = snap.sel; char val[IT_N][48]; memcpy(val, snap.val, sizeof val);
    ReleaseSRWLockShared(&lock);

    RECT all = { 0, 0, w, h };
    HBRUSH bg = CreateSolidBrush(RGB(18, 20, 26)), hi = CreateSolidBrush(RGB(40, 92, 170));
    FillRect(mdc, &all, bg);
    SetBkMode(mdc, TRANSPARENT);
    SelectObject(mdc, f_big); SetTextColor(mdc, RGB(255, 255, 255));
    RECT t = { 32, 24, w - 32, 80 };
    DrawTextA(mdc, "Paused", -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    for (int i = 0; i < IT_N; i++) {
        int y = 96 + i * ROW;
        RECT r = { 20, y, w - 20, y + ROW - 4 };
        if (is_header(i)) {
            SelectObject(mdc, f_bold); SetTextColor(mdc, RGB(120, 170, 235));
            RECT hr = { 32, y + 10, w - 32, y + ROW };
            DrawTextA(mdc, label[i], -1, &hr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            continue;
        }
        if (i == s) FillRect(mdc, &r, hi);
        SelectObject(mdc, f_norm);
        SetTextColor(mdc, i == IT_EXIT ? RGB(255, 140, 130) : RGB(235, 235, 235));
        RECT lr = { 48, y, w / 2 + 40, y + ROW - 4 };
        DrawTextA(mdc, label[i], -1, &lr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SetTextColor(mdc, RGB(255, 255, 255));
        RECT vr = { w / 2 - 20, y, w - 48, y + ROW - 4 };
        if (val[i][0]) {
            char v[64];
            if (i != s || i == IT_EXIT) snprintf(v, sizeof v, "%s", val[i]); else snprintf(v, sizeof v, "<  %s  >", val[i]);
            DrawTextA(mdc, v, -1, &vr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        }
    }
    SelectObject(mdc, f_small); SetTextColor(mdc, RGB(150, 155, 165));
    RECT fr = { 32, h - 56, w - 32, h - 16 };
    DrawTextA(mdc, "Up/Down: choose    Left/Right: change    Enter/A: select    Esc/B: resume", -1, &fr,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    DeleteObject(bg); DeleteObject(hi);
    GdiFlush();
    /* BGRX -> RGBA with the panel slightly see-through */
    for (int k = 0; k < w * h; k++) {
        u32 c = bits[k];
        pix[k] = ((c >> 16) & 0xff) | (c & 0xff00) | ((c & 0xff) << 16) | (232u << 24);
    }
}

/* ---- input --------------------------------------------------------------------------- */
enum { A_NONE, A_UP, A_DOWN, A_LEFT, A_RIGHT, A_OK, A_BACK };
static int keyq[16], nkeys;

/* from the window procedure: 1 = the menu took the key */
int menu_on_key(WPARAM vk, LPARAM l) {
    int repeat = (l >> 30) & 1;           /* auto-repeat of a held key */
    if (!g_menu_open) {
        if (vk == VK_ESCAPE) { if (!repeat) open_request = 1; return 1; }
        return 0;
    }
    if (repeat && (vk == VK_ESCAPE || vk == VK_RETURN || vk == VK_SPACE || vk == VK_BACK)) return 1;   /* arrows repeat */
    int a = vk == VK_UP ? A_UP : vk == VK_DOWN ? A_DOWN : vk == VK_LEFT ? A_LEFT : vk == VK_RIGHT ? A_RIGHT :
            vk == VK_RETURN || vk == VK_SPACE ? A_OK : vk == VK_ESCAPE || vk == VK_BACK ? A_BACK : A_NONE;
    if (a != A_NONE && nkeys < 16) keyq[nkeys++] = a;
    return 1;
}

/* pad: newly pressed buttons, held d-pad repeats */
#define XB_UP 0x0001
#define XB_DOWN 0x0002
#define XB_LEFT 0x0004
#define XB_RIGHT 0x0008
#define XB_START 0x0010
#define XB_BACK 0x0020
#define XB_A 0x1000
#define XB_B 0x2000
static int pad_prev;
static DWORD repeat_at;
static int pad_actions(int *out, int max) {
    int b = pad_menu_buttons(), n = 0, now = b & ~pad_prev;
    DWORD t = GetTickCount();
    int dirs = b & (XB_UP | XB_DOWN | XB_LEFT | XB_RIGHT);
    if (now & dirs) repeat_at = t + 350;
    else if (dirs && (int)(t - repeat_at) >= 0) { now |= dirs; repeat_at = t + 110; }
    pad_prev = b;
    if (now & XB_UP && n < max) out[n++] = A_UP;
    if (now & XB_DOWN && n < max) out[n++] = A_DOWN;
    if (now & XB_LEFT && n < max) out[n++] = A_LEFT;
    if (now & XB_RIGHT && n < max) out[n++] = A_RIGHT;
    if (now & XB_A && n < max) out[n++] = A_OK;
    if ((now & XB_B || ((b & (XB_BACK | XB_START)) == (XB_BACK | XB_START) && now & (XB_BACK | XB_START))) && n < max) out[n++] = A_BACK;
    return n;
}

static int held_any(void) {
    int b = pad_menu_buttons() & (XB_UP | XB_DOWN | XB_LEFT | XB_RIGHT | XB_START | XB_BACK | XB_A | XB_B);
    if (b) return 1;
    if (!gsgl_has_focus()) return 0;
    static const int vk[] = { VK_ESCAPE, VK_RETURN, VK_SPACE, VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_BACK };
    for (unsigned k = 0; k < sizeof vk / sizeof vk[0]; k++) if (GetAsyncKeyState(vk[k]) & 0x8000) return 1;
    return 0;
}

static void move(int d) {
    exit_armed = 0;
    do { sel = (sel + d + IT_N) % IT_N; } while (is_header(sel));
}

/* write the settings changed since the menu opened into mh2.ini (on close,
   on Exit game and when the window is closed with the menu open) */
int menu_save_pending(void) {
    const char *keys[IT_N], *vals[IT_N]; char buf[IT_N][32]; int nk = 0;
    for (int i = 0; i < IT_N; i++) {
        if (!dirty[i] || !ini_key[i]) continue;
        ini_value(i, buf[nk], sizeof buf[nk]);
        keys[nk] = ini_key[i]; vals[nk] = buf[nk]; nk++;
        dirty[i] = 0;
    }
    return nk ? config_save(keys, vals, nk) : 0;
}

static int ntest;              /* MENU_TEST, below */
static int test_next(void);

/* ---- the pause loop ---------------------------------------------------------------- */
static void run_menu(void) {
    g_menu_open = 1;
    pad_rumble_stop();
    sel = IT_RESUME; exit_armed = 0; nkeys = 0;
    memset(dirty, 0, sizeof dirty);
    snapshot();                             /* the panel is ready before it shows */
    g_menu_visible = 1;
    pad_prev = pad_menu_buttons();          /* the buttons that opened it are not presses */
    LOG("menu: opened at frame %llu\n", (unsigned long long)g_frame);
    int done = 0;
    while (!done) {
        gsgl_pump();
        int act[24], n = 0;
        for (int k = 0; k < nkeys && n < 24; k++) act[n++] = keyq[k];
        nkeys = 0;
        n += pad_actions(act + n, 24 - n);
        if (ntest > 0) { int t = test_next(); if (t != A_NONE && n < 24) act[n++] = t; }
        for (int k = 0; k < n && !done; k++) {
            switch (act[k]) {
            case A_UP: move(-1); break;
            case A_DOWN: move(1); break;
            case A_LEFT: case A_RIGHT: change(sel, act[k] == A_LEFT ? -1 : 1); break;
            case A_BACK: done = 1; break;
            case A_OK:
                if (sel == IT_RESUME) done = 1;
                else if (sel == IT_EXIT) {
                    if (exit_armed) { LOG("menu: exit\n"); menu_save_pending(); pad_rumble_stop(); exit(0); }
                    exit_armed = 1;
                } else change(sel, 1);
                break;
            }
        }
        snapshot();
        gs_present_again();
        Sleep(16);
    }
    g_menu_visible = 0;
    int saved = menu_save_pending();
    /* keys or buttons still held would reach the game: wait for them to go (1.5 s at most) */
    DWORD t0 = GetTickCount();
    while (held_any() && GetTickCount() - t0 < 1500) { gsgl_pump(); gs_present_again(); Sleep(16); }
    nkeys = 0;
    g_pace_restart = 1;                     /* the pause is not a slow frame */
    g_menu_open = 0;
    LOG("menu: closed%s\n", saved ? ", settings saved" : "");
}

/* MENU_TEST=<frame>:<key>,<key>,... (debug): open the menu at that frame and
   feed it one key every 4 passes: up down left right ok back */
static int test_keys[64], ntest = -1, test_pos; static u64 test_at;
static void test_load(void) {
    ntest = 0;
    const char *e = getenv("MENU_TEST");
    if (!e) return;
    test_at = strtoull(e, 0, 10);
    const char *p = strchr(e, ':');
    while (p && *p && ntest < 64) {
        p++;
        int a = !strncmp(p, "up", 2) ? A_UP : !strncmp(p, "down", 4) ? A_DOWN : !strncmp(p, "left", 4) ? A_LEFT :
                !strncmp(p, "right", 5) ? A_RIGHT : !strncmp(p, "ok", 2) ? A_OK : !strncmp(p, "back", 4) ? A_BACK : A_NONE;
        if (a != A_NONE) test_keys[ntest++] = a;
        p = strchr(p, ',');
    }
    LOG("menu: test at frame %llu with %d keys\n", (unsigned long long)test_at, ntest);
}
/* run_menu asks for the next scripted key (A_NONE: none this pass) */
static int test_next(void) {
    static int pass;
    if (test_pos >= ntest || ++pass % 4) return A_NONE;
    return test_keys[test_pos++];
}

/* every vblank, on the EE thread (kernel.c) */
void menu_vsync(void) {
    if (ntest < 0) test_load();
    if (ntest > 0 && g_frame == test_at) open_request = 1;
    if (g_turbo_cli || !gsgl_window_shown()) return;
    int b = pad_menu_buttons();
    static int combo_prev;
    int combo = (b & (XB_BACK | XB_START)) == (XB_BACK | XB_START);
    if (combo && !combo_prev) open_request = 1;
    combo_prev = combo;
    if (!open_request) return;
    open_request = 0;
    run_menu();
    combo_prev = (pad_menu_buttons() & (XB_BACK | XB_START)) == (XB_BACK | XB_START);
}
