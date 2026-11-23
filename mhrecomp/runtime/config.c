/* mh2.ini reader (see config.h). Format: `name = value` lines, # or ; comments. */
#include "rt.h"
#include "config.h"
int g_gs_thread_cfg = 1;   /* gsthread.c: 0 = graphics on the EE thread */
#include <ctype.h>
#include <stdlib.h>
#include <windows.h>
#include <io.h>

bmx2_cfg g_cfg = {
    .scale = 0, .window_w = 0, .window_h = 0, .fullscreen = 0, .stretch = 0, .smooth = 1,
    .frame_limit = 1, .vsync = 1, .gamepad = 1, .pad2 = 1, .rumble = 1,
    .keys = {
        [CFG_UP] = { VK_UP }, [CFG_DOWN] = { VK_DOWN }, [CFG_LEFT] = { VK_LEFT }, [CFG_RIGHT] = { VK_RIGHT },
        [CFG_START] = { VK_RETURN }, [CFG_SELECT] = { VK_BACK, VK_RSHIFT },
        [CFG_CROSS] = { 'K', VK_SPACE }, [CFG_CIRCLE] = { 'L' }, [CFG_SQUARE] = { 'J' }, [CFG_TRIANGLE] = { 'I' },
        [CFG_L1] = { 'Q' }, [CFG_R1] = { 'E' }, [CFG_L2] = { '1' }, [CFG_R2] = { '3' },
        [CFG_L3] = { 'Z' }, [CFG_R3] = { 'C' },
        [CFG_LS_UP] = { 'W' }, [CFG_LS_DOWN] = { 'S' }, [CFG_LS_LEFT] = { 'A' }, [CFG_LS_RIGHT] = { 'D' },
        [CFG_RS_UP] = { VK_NUMPAD8 }, [CFG_RS_DOWN] = { VK_NUMPAD2 }, [CFG_RS_LEFT] = { VK_NUMPAD4 }, [CFG_RS_RIGHT] = { VK_NUMPAD6 },
    },
};

static const char *const key_setting[CFG_NKEYS] = {
    "key_up", "key_down", "key_left", "key_right", "key_start", "key_select", "key_cross", "key_circle",
    "key_square", "key_triangle", "key_l1", "key_r1", "key_l2", "key_r2", "key_l3", "key_r3",
    "key_lstick_up", "key_lstick_down", "key_lstick_left", "key_lstick_right",
    "key_rstick_up", "key_rstick_down", "key_rstick_left", "key_rstick_right",
};

static const struct { const char *name; int vk; } key_names[] = {
    { "up", VK_UP }, { "down", VK_DOWN }, { "left", VK_LEFT }, { "right", VK_RIGHT },
    { "enter", VK_RETURN }, { "backspace", VK_BACK }, { "tab", VK_TAB }, { "space", VK_SPACE },
    { "lshift", VK_LSHIFT }, { "rshift", VK_RSHIFT }, { "lctrl", VK_LCONTROL }, { "rctrl", VK_RCONTROL },
    { "lalt", VK_LMENU }, { "ralt", VK_RMENU }, { "comma", VK_OEM_COMMA }, { "period", VK_OEM_PERIOD },
    { "slash", VK_OEM_2 }, { "semicolon", VK_OEM_1 }, { "none", 0 },
};

/* one key name -> virtual-key code; -1 if unknown */
static int parse_key(const char *s) {
    if (s[0] && !s[1] && isalnum((unsigned char)s[0])) return toupper((unsigned char)s[0]);
    if (!_strnicmp(s, "num", 3) && s[3] >= '0' && s[3] <= '9' && !s[4]) return VK_NUMPAD0 + s[3] - '0';
    for (unsigned k = 0; k < sizeof key_names / sizeof key_names[0]; k++)
        if (!_stricmp(s, key_names[k].name)) return key_names[k].vk;
    return -1;
}

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

static int as_bool(const char *v) { return !_stricmp(v, "1") || !_stricmp(v, "on") || !_stricmp(v, "true") || !_stricmp(v, "yes"); }

static void set(const char *k, char *v, int line) {
    if (!_stricmp(k, "scale")) { int n = atoi(v); if (n >= 1 && n <= 4) g_cfg.scale = n; }
    else if (!_stricmp(k, "window_size")) {
        int w, h;
        if (_stricmp(v, "auto") && sscanf(v, "%dx%d", &w, &h) == 2 && w >= 320 && h >= 240) { g_cfg.window_w = w; g_cfg.window_h = h; }
    }
    else if (!_stricmp(k, "fullscreen")) g_cfg.fullscreen = as_bool(v);
    else if (!_stricmp(k, "aspect")) g_cfg.stretch = !_stricmp(v, "stretch");
    else if (!_stricmp(k, "filter")) g_cfg.smooth = _stricmp(v, "nearest") != 0;
    else if (!_stricmp(k, "frame_limit")) g_cfg.frame_limit = as_bool(v);
    else if (!_stricmp(k, "vsync")) g_cfg.vsync = as_bool(v);
    else if (!_stricmp(k, "volume")) { char e[32]; int n = atoi(v); if (n < 0) n = 0; if (n > 100) n = 100;   /* spu2.c reads SPU_VOLUME */
                                       if (!getenv("SPU_VOLUME")) { snprintf(e, sizeof e, "SPU_VOLUME=%d", n); _putenv(e); } }
    else if (!_stricmp(k, "gs_thread")) g_gs_thread_cfg = as_bool(v);
    else if (!_stricmp(k, "gamepad")) g_cfg.gamepad = as_bool(v);
    else if (!_stricmp(k, "widescreen")) g_cfg.widescreen = as_bool(v);
    else if (!_stricmp(k, "skip_intro")) g_cfg.skip_intro = as_bool(v);
    else if (!_stricmp(k, "rumble")) g_cfg.rumble = as_bool(v);
    else if (!_stricmp(k, "pad2")) g_cfg.pad2 = !_stricmp(v, "off") || !_stricmp(v, "0") ? 0 : !_stricmp(v, "gamepad1") ? 2 : 1;
    /* debug switches the runtime reads from the environment (once, lazily) */
    else if (!_stricmp(k, "async_tex_sync")) { if (as_bool(v)) _putenv("GS_ASYNC_TEX_SYNC=1"); }
    else if (!_stricmp(k, "exact_gs_sync")) {}   /* old key: exact is the default now */
    else if (!_stricmp(k, "vu_mode")) {
        if (!_stricmp(v, "interpreter")) _putenv("VU_INTERP=1");
        else if (!_stricmp(v, "check")) _putenv("VU_CHECK=1");
    }
    else {
        for (int b = 0; b < CFG_NKEYS; b++) {
            if (_stricmp(k, key_setting[b])) continue;
            int vk[2] = { 0, 0 }, n = 0;
            for (char *p = strtok(v, ","); p && n < 2; p = strtok(NULL, ",")) {
                int c = parse_key(trim(p));
                if (c < 0) { LOG("mh2.ini line %d: unknown key \"%s\" for %s\n", line, trim(p), k); return; }
                vk[n++] = c;
            }
            g_cfg.keys[b][0] = vk[0]; g_cfg.keys[b][1] = vk[1];
            return;
        }
    }
}

static char loaded_path[MAX_PATH];   /* config_save writes back here; empty = no file was read */

void config_load(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    snprintf(loaded_path, sizeof loaded_path, "%s", path);
    char buf[512];
    int line = 0;
    while (fgets(buf, sizeof buf, f)) {
        line++;
        char *s = trim(buf);
        if (!*s || *s == '#' || *s == ';') continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        set(trim(s), trim(eq + 1), line);
    }
    fclose(f);
    LOG("config: read %s\n", path);
}

/* The in-game menu (menu.c) changed these settings: write them into mh2.ini.
 * The whole file is read; each `key = value` line of a changed key is replaced
 * in place (comments, order, line endings and the other settings stay), a key
 * the file lacks is added at the end. The new text goes to mh2.ini.new, which
 * then replaces the file; on any error the file is left as it was. Only if a
 * mh2.ini was read at start-up: development runs without one keep it so. */
int config_save(const char *const *keys, const char *const *vals, int nkeys) {
    if (nkeys <= 0) return 0;
    if (!loaded_path[0]) { LOG("config: no mh2.ini was read, menu changes are not saved\n"); return 0; }
    FILE *f = fopen(loaded_path, "rb");
    if (!f) { LOG("config: cannot read %s, menu changes are not saved\n", loaded_path); return 0; }
    fseek(f, 0, SEEK_END); long size = ftell(f); fseek(f, 0, SEEK_SET);
    char *text = size >= 0 && size < (16 << 20) ? malloc((size_t)size + 1) : NULL;
    size_t got = text ? fread(text, 1, (size_t)size, f) : 0;
    fclose(f);
    if (!text || got != (size_t)size) { free(text); LOG("config: cannot read %s, menu changes are not saved\n", loaded_path); return 0; }
    text[size] = 0;
    if (memchr(text, 0, (size_t)size)) {    /* UTF-16 or binary: the line walk below would cut it short */
        free(text); LOG("config: %s is not plain text, menu changes are not saved\n", loaded_path); return 0;
    }
    char tmp[MAX_PATH + 8]; snprintf(tmp, sizeof tmp, "%s.new", loaded_path);
    FILE *o = fopen(tmp, "wb");
    if (!o) { free(text); LOG("config: cannot write %s\n", tmp); return 0; }
    int done[64] = { 0 };
    const char *eol = strstr(text, "\r\n") ? "\r\n" : "\n";
    for (char *line = text; *line; ) {
        char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) + 1 : strlen(line);
        /* the key of this line: text before '=' with blanks trimmed, if not a comment */
        const char *p = line; while (*p == ' ' || *p == '\t') p++;
        int hit = -1;
        if (*p != '#' && *p != ';') {
            const char *eq = memchr(p, '=', len - (size_t)(p - line));
            if (eq) {
                const char *ke = eq; while (ke > p && (ke[-1] == ' ' || ke[-1] == '\t')) ke--;
                for (int i = 0; i < nkeys && i < 64; i++)
                    if (strlen(keys[i]) == (size_t)(ke - p) && !_strnicmp(p, keys[i], (size_t)(ke - p))) hit = i;
            }
        }
        if (hit >= 0) { fprintf(o, "%s = %s%s", keys[hit], vals[hit], end ? eol : ""); done[hit] = 1; }
        else fwrite(line, 1, len, o);
        line += len;
    }
    int last_nl = size == 0 || text[size - 1] == '\n';
    for (int i = 0; i < nkeys && i < 64; i++)
        if (!done[i]) { fprintf(o, "%s%s = %s%s", last_nl ? "" : eol, keys[i], vals[i], eol); last_nl = 1; }
    free(text);
    int ok = fflush(o) == 0 && !ferror(o);
    if (ok) ok = FlushFileBuffers((HANDLE)_get_osfhandle(_fileno(o))) != 0;   /* on disk before it replaces the old file */
    ok = fclose(o) == 0 && ok;
    if (ok) ok = MoveFileExA(tmp, loaded_path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok) DeleteFileA(tmp);
    LOG(ok ? "config: menu settings written to %s\n" : "config: could not write %s, it is unchanged\n", loaded_path);
    return ok;
}
