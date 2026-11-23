/* Pad: the IOP side of padman (sid 0x80000100/0x80000101), high-level.
 *
 * The game runs its own EE libpad ("xpad" flavour, libpad 2200). padPortOpen
 * hands the IOP a 256-byte area: two 128-byte frames that padman DMAs into
 * every vblank, the EE picking the one with the larger frame counter (+0x58).
 * Layout as read by the game's libpad (fn 1d92a8 getDmaStr, 1d9358 padRead,
 * 1d93d8 padGetState, 1d9578 padInfoAct, 1d9698 padInfoComb, 1d97b8
 * padInfoMode):
 *   +0x00 data[32]   (0 = ok, mode id, buttons active low, sticks, pressure)
 *   +0x30 act info, 4 bytes per actuator; +0x40 comb info, 4 bytes each
 *   +0x50 u16 modeTable[4]   +0x58 frame   +0x60 length
 *   +0x64 modeConfig  +0x65 modeCurId  +0x68 nrOfModes  +0x69 modeCurOffs
 *   +0x6a nrOfActuators  +0x6b numActComb  +0x6e lock
 *   +0x70 state (6 stable, 0 disconnected)  +0x71 reqState (0 done, 2 busy)
 *   +0x72 currentTask (1 = info ready)
 *
 * Port 0 slot 0 (player 1) is a DualShock 2 fed from the keyboard (window
 * focused) and the first XInput controller. Port 1 slot 0 (player 2) is the
 * second XInput controller (mh2.ini pad2 = gamepad2, the default), or the
 * first one (pad2 = gamepad1: then player 1 is the keyboard alone), or off
 * (pad2 = off). Player 2 reads as plugged in only while that controller is
 * connected. Multitap slots 1-3 read as disconnected.
 *
 * PAD_SCRIPT (player 1) and PAD2_SCRIPT (player 2, which then reads as
 * plugged in) = "frame:buttons[,frame:buttons...]" press buttons from a frame
 * on (until the next entry), for headless runs. Buttons are names joined by
 * '+': start select up down left right x square circle triangle l1 r1 l2 r2
 * l3 r3, or '-' for none. Example: PAD_SCRIPT=3700:start,3706:-,3900:x,3906:-
 */
#include "rt.h"
#include "config.h"
#include "iop.h"
#include <windows.h>
#include <stdlib.h>

/* button bits of the 16-bit word (data[2] low byte, data[3] high byte), active low */
enum {
    B_SELECT = 1 << 0, B_L3 = 1 << 1, B_R3 = 1 << 2, B_START = 1 << 3,
    B_UP = 1 << 4, B_RIGHT = 1 << 5, B_DOWN = 1 << 6, B_LEFT = 1 << 7,
    B_L2 = 1 << 8, B_R2 = 1 << 9, B_L1 = 1 << 10, B_R1 = 1 << 11,
    B_TRIANGLE = 1 << 12, B_CIRCLE = 1 << 13, B_CROSS = 1 << 14, B_SQUARE = 1 << 15,
};

int g_log_pad;
static u32 area[2][4];          /* EE address per port/slot, 0 = not open */
static u32 frame_no[2][4];
static int pressure_mode[2], analog[2] = { 1, 1 }, mode_lock[2];
/* vibration: padSetActDirect (EE fn 1d99a8) SIF-DMAs a 32-byte block
   {counter, 1, 6, data[6]} into one half (counter & 1) of a 64-byte IOP buffer
   whose address padman returns from open (+20); padSetActAlign (cmd 8) maps
   data bytes to actuators 0 (small motor, on/off) and 1 (large motor, 0-255). */
static u32 act_buf[2];                  /* IOP address per port (slot 0), 0 = none */
static u8 act_align[2][6] = { { 0, 1, 0xff, 0xff, 0xff, 0xff }, { 0, 1, 0xff, 0xff, 0xff, 0xff } };

/* ---- host input ------------------------------------------------------------ */
typedef struct { WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; } xi_gamepad;
typedef struct { DWORD packet; xi_gamepad g; } xi_state;
static DWORD (WINAPI *xi_get)(DWORD, xi_state *);
typedef struct { WORD left, right; } xi_vibration;     /* left = low-frequency (large) motor */
static DWORD (WINAPI *xi_set)(DWORD, xi_vibration *);
static int xi_tried;
static void xi_load(void);

/* XInputGetState on an empty slot is slow (it looks for the device each call):
   a slot that was empty is tried again only once a second */
static u16 rumble_now[4];               /* last motor values sent per XInput slot: large << 8 | small */
#define RUMBLE_UNKNOWN 0xffff           /* not delivered: the game never makes it (small is 0/1) */
static int xi_present[4];               /* the slot's last reads found a pad */
static int xi_read(DWORD idx, xi_state *x) {
    static u64 retry_at[4]; static int misses[4];
    if (!xi_tried) xi_load();
    if (!xi_get || idx > 3 || g_frame < retry_at[idx]) return 0;
    if (xi_get(idx, x) == 0) { misses[idx] = 0; xi_present[idx] = 1; return 1; }
    /* a connected pad that misses one read (a wireless blip) is retried next frame;
       only after 3 misses in a row is the slot treated as empty */
    if (++misses[idx] >= 3) { retry_at[idx] = g_frame + 60; rumble_now[idx] = RUMBLE_UNKNOWN; xi_present[idx] = 0; }   /* resend vibration on replug */
    return 0;
}

static void xi_load(void) {
    xi_tried = 1;
    const char *dlls[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" };
    for (int i = 0; i < 3 && !xi_get; i++) {
        HMODULE m = LoadLibraryA(dlls[i]);
        if (m) {
            xi_get = (DWORD (WINAPI *)(DWORD, xi_state *))(void *)GetProcAddress(m, "XInputGetState");
            xi_set = (DWORD (WINAPI *)(DWORD, xi_vibration *))(void *)GetProcAddress(m, "XInputSetState");
        }
    }
}

/* the XInput slot that is this player's pad, or -1 */
static int player_slot(int port) {
    if (!g_cfg.gamepad) return -1;
    if (port == 0) return g_cfg.pad2 == 2 ? -1 : 0;
    return g_cfg.pad2 == 0 ? -1 : g_cfg.pad2 == 2 ? 0 : 1;
}

static u64 rumble_retry[4];             /* a send failed (no pad): next try at this frame */
static void rumble_send(int idx, u16 v) {
    if (idx < 0 || idx > 3 || rumble_now[idx] == v || g_frame < rumble_retry[idx]) return;
    if (xi_set && !xi_present[idx]) return;   /* no pad there: sent when one is read again */
    if (g_log_pad) LOG("pad: rumble slot %d large %u small %u at frame %llu\n", idx, v >> 8, (v & 0xff) != 0, (unsigned long long)g_frame);
    if (!xi_set) { rumble_now[idx] = v; return; }
    xi_vibration vib = { (WORD)((v >> 8) * 257), (WORD)((v & 0xff) ? 0xffff : 0) };
    /* only a delivered value is remembered; a failed one (no pad, a wireless
       blip) leaves the slot unknown, so the next value, a stop included, is
       retried once a second until it gets through */
    if (xi_set((DWORD)idx, &vib) == 0) rumble_now[idx] = v;
    else { rumble_now[idx] = RUMBLE_UNKNOWN; rumble_retry[idx] = g_frame + 60; }
}
static void rumble_stop_all(void) {
    for (int i = 0; i < 4; i++) if (rumble_now[i]) { rumble_retry[i] = 0; rumble_send(i, 0); }
}

/* menu.c: the raw XInput buttons of the first two pads (the menu reads them even
   with gamepad = 0 while it is open, so a pad can always leave it), and a stop
   for the motors while the game is paused */
int pad_menu_buttons(void) {
    extern volatile int g_menu_open;
    if (!xi_tried) xi_load();
    if (!xi_get) return 0;
    int slot[2] = { -1, -1 };
    if (g_cfg.gamepad) { slot[0] = player_slot(0); slot[1] = player_slot(1); }
    else if (g_menu_open) slot[0] = 0;
    /* only slots pad_vsync found a pad in (it keeps the miss count); while the
       game is paused g_frame stands still, so an empty slot is tried once a
       second by the clock instead */
    static DWORD retry_at[4]; static unsigned char waiting[4]; DWORD t = GetTickCount(); int b = 0;
    for (int k = 0; k < 2; k++) {
        int s = slot[k];
        if (s < 0 || (k && s == slot[0])) continue;
        /* paused: a slot whose read failed waits a second (the difference is only taken
           while waiting, 1 s from the failure, so the 49.7-day tick wrap cannot block it) */
        if (g_menu_open ? waiting[s] && (int)(t - retry_at[s]) < 0 : !xi_present[s]) continue;
        xi_state x;
        if (xi_get((DWORD)s, &x) == 0) { b |= x.g.buttons; waiting[s] = 0; }
        else { waiting[s] = 1; retry_at[s] = t + 1000; }
    }
    return b;
}
void pad_rumble_stop(void) { rumble_stop_all(); }

/* newest motor values the game DMA'd for a port: large << 8 | small (0 = off) */
static u16 rumble_read(int port) {
    u32 a = act_buf[port];
    if (!a) return 0;
    u32 c0 = ir32(a), c1 = ir32(a + 32);
    if (g_log_pad) {
        static u32 seen[2];
        if (c0 + c1 != seen[port]) { seen[port] = c0 + c1; LOG("pad: act block for player %d: counters %u %u\n", port + 1, c0, c1); }
    }
    u32 half = (s32)(c1 - c0) > 0 ? a + 32 : a;   /* the larger counter is the newer block */
    u8 d[6]; for (int k = 0; k < 6; k++) d[k] = ir8(half + 12 + k);
    u8 small = act_align[port][0] < 6 ? d[act_align[port][0]] : 0;
    u8 large = act_align[port][1] < 6 ? d[act_align[port][1]] : 0;
    return (u16)(large << 8 | (small ? 1 : 0));
}

typedef struct { u16 buttons; u8 lx, ly, rx, ry; } pad_in;

static const struct { const char *name; u16 bit; } bnames[] = {
    { "select", B_SELECT }, { "l3", B_L3 }, { "r3", B_R3 }, { "start", B_START },
    { "up", B_UP }, { "right", B_RIGHT }, { "down", B_DOWN }, { "left", B_LEFT },
    { "l2", B_L2 }, { "r2", B_R2 }, { "l1", B_L1 }, { "r1", B_R1 },
    { "triangle", B_TRIANGLE }, { "circle", B_CIRCLE }, { "x", B_CROSS }, { "cross", B_CROSS },
    { "square", B_SQUARE },
};

typedef struct { u64 frame; u16 buttons; } script_ev;
static script_ev scripts[2][4096];
static int nscripts[2] = { -1, -1 };

static void script_load(int port) {
    script_ev *script = scripts[port];
    int nscript = 0;
    const char *s = getenv(port ? "PAD2_SCRIPT" : "PAD_SCRIPT");
    while (s && *s && nscript < 4096) {
        char *e; u64 f = strtoull(s, &e, 10);
        if (*e != ':') break;
        s = e + 1;
        u16 b = 0;
        while (*s && *s != ',') {
            const char *t = s; while (*s && *s != '+' && *s != ',') s++;
            size_t n = (size_t)(s - t);
            for (unsigned k = 0; k < sizeof bnames / sizeof bnames[0]; k++)
                if (strlen(bnames[k].name) == n && !_strnicmp(t, bnames[k].name, n)) b |= bnames[k].bit;
            if (*s == '+') s++;
        }
        script[nscript].frame = f; script[nscript].buttons = b; nscript++;
        if (*s == ',') s++;
    }
    nscripts[port] = nscript;
    if (nscript) LOG("pad: %d scripted inputs for player %d\n", nscript, port + 1);
}

static u8 stick(int neg, int pos) { return neg && !pos ? 0x00 : pos && !neg ? 0xff : 0x80; }
static u8 axis(SHORT v, int flip) {
    int a = flip ? -(int)v : v;            /* -32768..32767 -> 0..255, 128 centre */
    a = (a + 32768) >> 8; if (a > 255) a = 255; if (a < 0) a = 0;
    return (u8)a;
}

static void gamepad_in(pad_in *p, const xi_state *x);

/* skip_intro: the boot movies end when Start is pressed, so while one of them
   plays (its .PSS was opened by name and the IPU decoded it within 10 frames)
   player 1's Start is pulsed, 4 frames down, 4 up. Any other movie (Cool Stuff,
   rider bios) ends intro mode, as does INTRO.PSS, the last boot movie, ending:
   30 frames with no decode after it decoded at least once. Only decodes since
   the open count, so the previous movie's last decode cannot end it early. */
static int intro_movie = -1;   /* -1 boot, 1 intro movie open, 0 intro over */
static int intro_last;         /* the open movie is INTRO.PSS */
static u64 intro_opened;       /* frame the open movie was opened */
void pad_movie_opened(const char *nm) {
    if (g_log_pad) LOG("pad: movie %s opened at frame %llu\n", nm, (unsigned long long)g_frame);
    if (intro_movie == 0) return;
    const char *b = strrchr(nm, '\\'); b = b ? b + 1 : nm;
    if (!_strnicmp(b, "ACCLAIM.PSS", 11) || !_strnicmp(b, "ZINTRO.PSS", 10) || !_strnicmp(b, "INTRO.PSS", 9)) {
        intro_movie = 1; intro_last = !_strnicmp(b, "INTRO.PSS", 9); intro_opened = g_frame;
    } else intro_movie = 0;
}
static int intro_press(void) {
    extern u64 g_ipu_last_frame;
    if (intro_movie != 1) return 0;
    int decoded = g_ipu_last_frame > intro_opened;             /* this movie has decoded a picture */
    if (decoded && g_frame - g_ipu_last_frame < 10) return g_cfg.skip_intro && (g_frame & 4);
    if (decoded && intro_last && g_frame - g_ipu_last_frame > 30) intro_movie = 0;
    return 0;
}

/* one player's input; returns whether that player's pad is plugged in */
static int read_host(int port, pad_in *p) {
    p->buttons = 0; p->lx = p->ly = p->rx = p->ry = 0x80;
    if (nscripts[port] < 0) script_load(port);
    for (int i = 0; i < nscripts[port]; i++) if (scripts[port][i].frame <= g_frame) p->buttons = scripts[port][i].buttons;
    xi_state x;
    if (port == 1) {
        int connected = nscripts[1] > 0;
        if (g_cfg.pad2 && g_cfg.gamepad && xi_read(g_cfg.pad2 == 2 ? 0 : 1, &x)) { gamepad_in(p, &x); connected = 1; }
        return connected;
    }

    extern int gsgl_has_focus(void);
    if (gsgl_has_focus()) {
#define K(vk) (GetAsyncKeyState(vk) & 0x8000)
        static const u16 bit[16] = { B_UP, B_DOWN, B_LEFT, B_RIGHT, B_START, B_SELECT, B_CROSS, B_CIRCLE,
                                     B_SQUARE, B_TRIANGLE, B_L1, B_R1, B_L2, B_R2, B_L3, B_R3 };
#define KB(b) ((g_cfg.keys[b][0] && K(g_cfg.keys[b][0])) || (g_cfg.keys[b][1] && K(g_cfg.keys[b][1])))
        for (int k = 0; k < 16; k++) if (KB(k)) p->buttons |= bit[k];
        int l = KB(CFG_LS_LEFT), r = KB(CFG_LS_RIGHT), u = KB(CFG_LS_UP), d = KB(CFG_LS_DOWN);
        if (l || r) p->lx = stick(l, r);
        if (u || d) p->ly = stick(u, d);
        l = KB(CFG_RS_LEFT); r = KB(CFG_RS_RIGHT); u = KB(CFG_RS_UP); d = KB(CFG_RS_DOWN);
        if (l || r) p->rx = stick(l, r);
        if (u || d) p->ry = stick(u, d);
#undef KB
#undef K
    }

    if (g_cfg.gamepad && g_cfg.pad2 != 2 && xi_read(0, &x)) gamepad_in(p, &x);
    if (intro_press()) p->buttons |= B_START;
    return 1;
}

static void gamepad_in(pad_in *p, const xi_state *xp) {
    const xi_state x = *xp;
    {
        WORD w = x.g.buttons;
        static const struct { WORD xb; u16 bit; } map[] = {
            { 0x0001, B_UP }, { 0x0002, B_DOWN }, { 0x0004, B_LEFT }, { 0x0008, B_RIGHT },
            { 0x0010, B_START }, { 0x0020, B_SELECT }, { 0x0040, B_L3 }, { 0x0080, B_R3 },
            { 0x0100, B_L1 }, { 0x0200, B_R1 },
            { 0x1000, B_CROSS }, { 0x2000, B_CIRCLE }, { 0x4000, B_SQUARE }, { 0x8000, B_TRIANGLE },
        };
        for (unsigned k = 0; k < sizeof map / sizeof map[0]; k++) if (w & map[k].xb) p->buttons |= map[k].bit;
        if (x.g.lt > 30) p->buttons |= B_L2;
        if (x.g.rt > 30) p->buttons |= B_R2;
        /* dead zone so a resting stick does not override the keyboard */
        if (abs(x.g.lx) > 7849) p->lx = axis(x.g.lx, 0);
        if (abs(x.g.ly) > 7849) p->ly = axis(x.g.ly, 1);
        if (abs(x.g.rx) > 8689) p->rx = axis(x.g.rx, 0);
        if (abs(x.g.ry) > 8689) p->ry = axis(x.g.ry, 1);
    }
}

/* ---- the DMA'd frame ------------------------------------------------------- */
static void put8(u32 a, u8 v) { mw8(a, v); }

static void write_frame(int port, int slot, const pad_in *in, int plugged) {
    u32 base = area[port][slot];
    if (!base) return;
    u32 n = ++frame_no[port][slot];
    u32 f = base + 128 * (n & 1);
    for (u32 k = 0; k < 128; k += 4) mw32(f + k, 0);
    mw32(f + 0x58, n);
    put8(f + 0x72, 1);                 /* currentTask: info ready */
    put8(f + 0x71, 0);                 /* reqState: complete */
    if (slot != 0 || !plugged) {       /* nothing plugged in */
        put8(f + 0x70, 0);
        return;
    }
    u8 id = !analog[port] ? 0x41 : pressure_mode[port] ? 0x79 : 0x73;
    put8(f + 0x70, 6);                 /* stable */
    put8(f + 0x64, 2);                 /* modeConfig: mode info read */
    put8(f + 0x65, id);
    put8(f + 0x66, 3);                 /* model: DualShock 2 */
    put8(f + 0x67, 1);
    mw16(f + 0x50, 4); mw16(f + 0x52, 7);     /* modeTable: digital, dualshock */
    put8(f + 0x68, 2);
    put8(f + 0x69, analog[port] ? 1 : 0);
    put8(f + 0x6a, 2);                 /* two motors */
    put8(f + 0x30, 0); put8(f + 0x31, 1); put8(f + 0x32, 0); put8(f + 0x33, 1);       /* small: on/off */
    put8(f + 0x34, 0); put8(f + 0x35, 1); put8(f + 0x36, 1); put8(f + 0x37, 0x14);    /* large: 8-bit level */
    put8(f + 0x6b, 1);
    put8(f + 0x40, 0); put8(f + 0x41, 1); put8(f + 0x42, 0); put8(f + 0x43, 0);
    put8(f + 0x6e, (u8)mode_lock[port]);
    /* button mask (padInfoPressMode, EE fn 1d9ca0): a DualShock 2 always has the 18
       pressure-capable buttons; the game's pad setup (fn 112d98 step 70) gives up without */
    put8(f + 0x79, 0xff); put8(f + 0x7a, 0xff); put8(f + 0x7b, 0x03);

    u16 b = (u16)~in->buttons;
    u8 d[32]; memset(d, 0, sizeof d);
    d[0] = 0; d[1] = id; d[2] = (u8)b; d[3] = (u8)(b >> 8);
    u32 len = 4;
    if (id != 0x41) {
        d[4] = in->rx; d[5] = in->ry; d[6] = in->lx; d[7] = in->ly;
        len = 8;
    }
    if (id == 0x79) {
        static const u16 order[12] = { B_RIGHT, B_LEFT, B_UP, B_DOWN, B_TRIANGLE, B_CIRCLE,
                                       B_CROSS, B_SQUARE, B_L1, B_R1, B_L2, B_R2 };
        for (int k = 0; k < 12; k++) d[8 + k] = (in->buttons & order[k]) ? 0xff : 0;
        len = 20;
    }
    for (int k = 0; k < 32; k++) put8(f + k, d[k]);
    mw32(f + 0x60, len);
}

void pad_vsync(void) {
    static u16 last[2]; static int was[2] = { -1, -1 };
    for (int p = 0; p < 2; p++) {
        pad_in in;
        int plugged = read_host(p, &in);
        if (g_log_pad && in.buttons != last[p]) LOG("pad: player %d frame %llu buttons %04x\n", p + 1, (unsigned long long)g_frame, in.buttons);
        if (plugged != was[p] && (p || g_log_pad)) LOG("pad: player %d pad %s at frame %llu\n", p + 1, plugged ? "plugged in" : "unplugged", (unsigned long long)g_frame);
        last[p] = in.buttons; was[p] = plugged;
        for (int s = 0; s < 4; s++) write_frame(p, s, &in, plugged);
    }
    {   /* vibration (mh2.ini rumble = 0 turns it off) */
        static int hooked; static u64 test_at;
        if (!hooked) {
            hooked = 1; atexit(rumble_stop_all);
            const char *e = getenv("PAD_RUMBLE_TEST");   /* debug: frame to call the game's vibrate fn */
            if (e) test_at = strtoull(e, 0, 10);
        }
        if (test_at && g_frame == test_at) {   /* fn 16bb80(player 0, large motor, 255); debug only: safe because 16bb80 makes no blocking syscall */
            ee_state save = ee;
            GD(4) = 0; GD(5) = 1; GD(6) = 255; GD(29) -= 0x1000;
            LOG("pad: rumble test call at frame %llu\n", (unsigned long long)g_frame);
            ee_call(0x0016bb80);
            ee = save;
        }
        u16 v[4] = { 0, 0, 0, 0 };
        for (int p = 0; p < 2; p++) {
            int idx = player_slot(p);
            if (idx >= 0 && g_cfg.rumble && was[p] == 1) v[idx] |= rumble_read(p);
        }
        for (int i = 0; i < 2; i++) rumble_send(i, v[i]);
    }
}

/* padman RPC: buf is the 128-byte request/reply in IOP RAM (sif.c server buffer).
 * Command numbers and reply offsets as the game's libpad sends and reads them
 * (fn 1d9008 open, 1d98f0 setMainMode, 1d9a60 setActAlign, 1d9bf0 setButtonInfo,
 * 1d9e80 getPortMax, 1d9ee8 getSlotMax, 1d91f0 close, 1d9f50 version):
 *   1 open(port, slot, -, area)   +12 result, +20 IOP address of the 64-byte act buffer
 *   6 setMainMode(mode, lock), 8 setActAlign(data[6] at +12): result at +20;
 *   10 setButtonInfo(mask): result at +16 (1 = accepted; the game then waits on the
 *       frame's reqState)
 *   12 getPortMax, 13 getSlotMax(port), 14 close, 16 init, 18 version: result at +12
 * The button mask (padInfoPressMode) is read from the frame (+0x79), not by RPC. */
void pad_rpc(u8 *buf) {
    u32 w[8]; memcpy(w, buf, 32);
    u32 cmd = w[0], port = w[1] & 1, slot = w[2] & 3, ret = 1, ret20 = 1;
    u32 iop = (u32)(buf - iop_ram);
    switch (cmd) {
    case 1:                                                              /* open */
        area[port][slot] = w[4]; frame_no[port][slot] = 0;
        if (slot == 0) {
            act_buf[port] = iop + 0x800 + 64 * port;                     /* free space in the server buffer */
            for (u32 k = 0; k < 64; k += 4) iw32(act_buf[port] + k, 0);
            ret20 = act_buf[port];
        } else ret20 = 0;
        break;
    case 6:                                                              /* setMainMode(port, slot, mode, lock) */
        if (slot == 0) { analog[port] = w[3] != 0; mode_lock[port] = w[4] == 3; if (!analog[port]) pressure_mode[port] = 0; }
        else ret20 = 0;
        break;
    case 8:                                                              /* setActAlign(port, slot, data[6]) */
        if (slot == 0) memcpy(act_align[port], buf + 12, 6); else ret20 = 0;
        break;
    case 10:                                                             /* setButtonInfo(port, slot, mask) */
        if (slot == 0) pressure_mode[port] = (w[3] & 0x3ffff) != 0; else ret20 = 0;
        break;
    case 12: ret = 2; break;                                             /* getPortMax */
    case 13: ret = 1; break;                                             /* getSlotMax */
    case 14:                                                             /* close */
        area[port][slot] = 0;
        if (slot == 0) act_buf[port] = 0;
        break;
    case 16: ret = 1; break;                                             /* init */
    case 18: memset(buf, 0, 128); ret = 0x400; break;                    /* module version */
    default: break;                                                      /* 11 vref, 20 end */
    }
    if (g_log_pad || (cmd != 6 && cmd != 8 && cmd != 10))
        LOG("pad: cmd %u port %u slot %u args %08x %08x -> %#x / %#x\n", cmd, port, slot, w[3], w[4], ret, ret20);
    memcpy(buf + 12, &ret, 4);
    if (cmd == 1 || cmd == 6 || cmd == 8) memcpy(buf + 20, &ret20, 4);
    if (cmd == 10) memcpy(buf + 16, &ret20, 4);
}
