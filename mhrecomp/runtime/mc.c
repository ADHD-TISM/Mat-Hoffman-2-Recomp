/* Memory card: the IOP side of MCSERV (sid 0x80000400), high-level, over a
 * folder of host files. The game's own EE libmc (SCE 2200, old v1 protocol)
 * runs unchanged; MCMAN/MCSERV/SIO2MAN are not run.
 *
 * Card 1 (port 0) is memcard\mc0\ in the current folder (the disc folder for
 * build\mh2.exe, game\ for the launcher's build): every card directory and
 * file is a host directory and file of the same name. Port 1 reads as empty.
 *
 * Request layouts, from the game's libmc (0x1e5cd0..0x1e7338):
 *   path calls (1044 bytes): [0] port [4] slot [8] flags/mode [12] max entries
 *     [16] EE pointer (GetDir table, SetFileInfo info) [20..] path
 *   handle calls (48 bytes): [0] fd [12] size [16] seek offset [20] origin /
 *     write head count [24] EE buffer [28] EE result area [32..47] write head
 *   GetInfo (48): [4] port [8] slot [12] want format [16] want free [20] want
 *     type [28] EE result area: +0 type, +4 free clusters, +0x90 format
 *   Read's result area: +0 head count +4 tail count (both 0 here: the data is
 *     written straight into the EE buffer)
 * Return value: 4 bytes at the start of the reply.
 * fn: 1 GetInfo 2 Open 3 Close 4 Seek 5 Read 6 Write 10 Flush 12 ChDir
 *     13 GetDir 14 SetFileInfo 15 Delete 16 Format 17 Unformat, 254 version. */
#include "rt.h"
#include <windows.h>
#include <direct.h>
#include <io.h>
#include <sys/stat.h>
#include <time.h>

#define MC_ROOT "memcard"
#define MC_CURDIR_BUF 0x00301d40u       /* the game's libmc copies ChDir's old directory from here */
enum { OK = 0, CHANGED = -1, NOFORMAT = -2, FULL = -3, NOENTRY = -4, DENIED = -5, NOTEMPTY = -6, UPLIMIT = -7 };

int g_log_mc;
static FILE *fds[32];
static char cwd[2][1024] = { "/", "/" };
static int seen[2];

static inline u32 rd(const u8 *b, u32 o) { u32 v; memcpy(&v, b + o, 4); return v; }
static inline void wr(u8 *b, u32 o, u32 v) { memcpy(b + o, &v, 4); }

/* card path (absolute or relative to the card's current directory) -> host path */
static void host_path(int port, const char *p, char *out, size_t n) {
    char full[2048];
    if (p[0] == '/' || p[0] == '\\') snprintf(full, sizeof full, "%s", p);
    else snprintf(full, sizeof full, "%s/%s", cwd[port & 1], p);
    /* resolve . and .. */
    char parts[64][64]; int np = 0;
    for (char *s = full; *s; ) {
        while (*s == '/' || *s == '\\') s++;
        if (!*s) break;
        char *e = s; while (*e && *e != '/' && *e != '\\') e++;
        size_t l = (size_t)(e - s);
        if (l == 1 && s[0] == '.') {}
        else if (l == 2 && s[0] == '.' && s[1] == '.') { if (np) np--; }
        else if (np < 64) { if (l > 63) l = 63; memcpy(parts[np], s, l); parts[np][l] = 0; np++; }
        s = e;
    }
    int k = snprintf(out, n, MC_ROOT "\\mc%d", port & 1);
    for (int i = 0; i < np && k >= 0 && (size_t)k < n; i++) k += snprintf(out + k, n - k, "\\%s", parts[i]);   /* stop once truncated */
}

static void ensure_root(int port) {
    char p[64]; _mkdir(MC_ROOT); snprintf(p, sizeof p, MC_ROOT "\\mc%d", port & 1); _mkdir(p);
}

static int wild(const char *pat, const char *s) {
    if (!*pat) return !*s;
    if (*pat == '*') return wild(pat + 1, s) || (*s && wild(pat, s + 1));
    if (*s && (*pat == '?' || toupper((u8)*pat) == toupper((u8)*s))) return wild(pat + 1, s + 1);
    return 0;
}

/* one 64-byte sceMcTblGetDir entry, built in a host buffer */
static void put_dt(u8 *a, time_t t) {
    struct tm *g = localtime(&t);
    u8 d[8] = { 0, (u8)g->tm_sec, (u8)g->tm_min, (u8)g->tm_hour, (u8)g->tm_mday, (u8)(g->tm_mon + 1), 0, 0 };
    u16 y = (u16)(g->tm_year + 1900); memcpy(d + 6, &y, 2);
    memcpy(a, d, 8);
}
static void put_entry(u8 *a, const char *name, const struct _stat64 *st, int dir) {
    memset(a, 0, 64);
    put_dt(a, st ? st->st_ctime : time(NULL));
    put_dt(a + 8, st ? st->st_mtime : time(NULL));
    u32 size = dir ? 0 : (u32)(st ? st->st_size : 0); memcpy(a + 16, &size, 4);
    u16 attr = dir ? 0x8427 : 0x8497; memcpy(a + 20, &attr, 2);   /* exists | readable/writable/executable | dir or file */
    for (int i = 0; i < 31 && name[i]; i++) a[32 + i] = (u8)name[i];
}

/* up to maxent entries of a listing into out (maxent * 64 bytes); returns the count */
static int list_dir(int port, const char *path, int maxent, u8 *out) {
    char dirpart[1024], pat[256];
    const char *sl = strrchr(path, '/');
    if (sl) { size_t l = (size_t)(sl - path); memcpy(dirpart, path, l); dirpart[l] = 0; if (!l) strcpy(dirpart, "/"); snprintf(pat, sizeof pat, "%s", sl + 1); }
    else { strcpy(dirpart, cwd[port & 1]); snprintf(pat, sizeof pat, "%s", path); }
    char hd[2048]; host_path(port, dirpart, hd, sizeof hd);
    int n = 0, root = !strcmp(dirpart, "/") || !dirpart[0];
    if (!strpbrk(pat, "*?")) {                    /* a name: the entry itself */
        char hp[2048]; struct _stat64 st;
        snprintf(hp, sizeof hp, "%s\\%s", hd, pat);
        if (_stat64(hp, &st)) return 0;
        if (maxent > 0) put_entry(out, pat, &st, (st.st_mode & _S_IFDIR) != 0);
        return 1;
    }
    if (!root && maxent > n && wild(pat, ".")) put_entry(out + 64 * n++, ".", NULL, 1);
    if (!root && maxent > n && wild(pat, "..")) put_entry(out + 64 * n++, "..", NULL, 1);
    char q[2100]; snprintf(q, sizeof q, "%s\\*", hd);
    struct _finddata64i32_t f; intptr_t h = _findfirst64i32(q, &f);
    if (h == -1) return n;
    do {
        if (!strcmp(f.name, ".") || !strcmp(f.name, "..") || !wild(pat, f.name)) continue;
        if (n >= maxent) break;
        char hp[2048]; struct _stat64 st; snprintf(hp, sizeof hp, "%s\\%s", hd, f.name);
        if (_stat64(hp, &st)) continue;
        put_entry(out + 64 * n++, f.name, &st, (st.st_mode & _S_IFDIR) != 0);
    } while (_findnext64i32(h, &f) == 0);
    _findclose(h);
    return n;
}

/* GetDir. flags != 0 is sceMcGetDir's "next" call: it continues the previous
 * listing of the same path instead of starting again (the old code listed from
 * the start each time, so a game reading a long directory in steps got the
 * first entries again and again). */
static int get_dir(int port, const char *path, int maxent, u32 table, int cont) {
    static char last[1024]; static int done;
    if (!cont || strcmp(last, path)) { snprintf(last, sizeof last, "%s", path); done = 0; }
    if (maxent <= 0) return 0;
    int want = done + maxent;
    u8 *buf = malloc((size_t)want * 64);
    if (!buf) return 0;
    int got = list_dir(port, path, want, buf);
    int n = got > done ? got - done : 0;
    for (int i = 0; i < n * 64; i++) mw8(table + (u32)i, buf[(size_t)done * 64 + (size_t)i]);
    free(buf);
    done += n;
    return n;
}

/* free 1 KB clusters as on a formatted 8 MB card: 8000 when empty, less each
 * file's clusters and each directory's entry clusters (512-byte entries) */
#define MC_CLUSTERS 8000
static int used_clusters(const char *hd, int root) {
    char q[2100]; snprintf(q, sizeof q, "%s\\*", hd);
    struct _finddata64i32_t f; intptr_t h = _findfirst64i32(q, &f);
    int n = 0, used = 0;
    if (h != -1) {
        do {
            if (!strcmp(f.name, ".") || !strcmp(f.name, "..")) continue;
            n++;
            if (f.attrib & _A_SUBDIR) { char sub[2100]; snprintf(sub, sizeof sub, "%s\\%s", hd, f.name); used += used_clusters(sub, 0); }
            else used += (int)((f.size + 1023) / 1024);
        } while (_findnext64i32(h, &f) == 0);
        _findclose(h);
    }
    used += (n + 2 + 1) / 2;                      /* this directory's entries, with . and .. */
    if (root) used -= 1;                          /* the empty root's cluster is part of the 8000 */
    return used;
}
static int free_clusters(int port) {
    char hd[64]; snprintf(hd, sizeof hd, MC_ROOT "\\mc%d", port & 1);
    int f = MC_CLUSTERS - used_clusters(hd, 1);
    return f < 0 ? 0 : f;
}

/* a 64-byte table entry's date (8 bytes: 0, sec, min, hour, day, month, year16) -> FILETIME */
static int get_dt(u32 a, FILETIME *ft) {
    u8 d[8]; for (int i = 0; i < 8; i++) d[i] = mr8(a + i);
    SYSTEMTIME t = { 0 }; u16 y; memcpy(&y, d + 6, 2);
    t.wYear = y; t.wMonth = d[5]; t.wDay = d[4]; t.wHour = d[3]; t.wMinute = d[2]; t.wSecond = d[1];
    if (!t.wYear || !t.wMonth || !t.wDay) return 0;
    FILETIME local; if (!SystemTimeToFileTime(&t, &local)) return 0;
    return LocalFileTimeToFileTime(&local, ft);   /* put_dt writes local time */
}
static void set_times(const char *hp, u32 info, int create, int modify) {
    FILETIME c, m; int hc = create && get_dt(info, &c), hm = modify && get_dt(info + 8, &m);
    if (!hc && !hm) return;
    HANDLE h = CreateFileA(hp, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    SetFileTime(h, hc ? &c : NULL, NULL, hm ? &m : NULL);
    CloseHandle(h);
}

static int mc_open(int port, const char *path, u32 mode) {
    if (port & 1) return NOENTRY;
    char hp[2048]; host_path(port, path, hp, sizeof hp);
    if (mode & 0x40) {                            /* sceMcFileCreateDir */
        struct _stat64 st;
        if (!_stat64(hp, &st)) return NOENTRY;    /* already there: -4, as mcman answers */
        return _mkdir(hp) ? DENIED : OK;
    }
    int k; for (k = 0; k < 32 && fds[k]; k++) {}
    if (k == 32) return UPLIMIT;
    struct _stat64 st; int exists = !_stat64(hp, &st);
    if (exists && (st.st_mode & _S_IFDIR)) return DENIED;
    const char *m;
    if (!exists && !(mode & 0x200)) return NOENTRY;
    if ((mode & 3) == 1) m = "rb";
    else if (mode & 0x400 || !exists) m = (mode & 3) == 3 ? "w+b" : "wb";
    else m = "r+b";
    FILE *f = fopen(hp, m);
    if (!f) return exists ? DENIED : NOENTRY;
    fds[k] = f;
    return k;
}

static void log_call(u32 fn, const u8 *b, int r) {
    if (!g_log_mc) return;
    if (fn == 2 || (fn >= 12 && fn <= 15)) LOG("mc: fn %u port %u mode %x '%s' -> %d\n", fn, rd(b, 0), rd(b, 8), (const char *)b + 20, r);
    else LOG("mc: fn %u %08x %08x %08x %08x -> %d\n", fn, rd(b, 0), rd(b, 12), rd(b, 16), rd(b, 20), r);
}

void mc_rpc(u32 fn, u8 *b) {
    int r = 0;
    switch (fn) {
    case 254: wr(b, 0, 0); wr(b, 4, 0x20a); wr(b, 8, 0x20e); return;           /* versions: mcserv 2.10, mcman 2.14 */
    case 1: {                                                                  /* GetInfo */
        int port = (int)rd(b, 4) & 1; u32 res = rd(b, 28);
        if (port == 0) {
            ensure_root(0);
            if (rd(b, 20)) mw32(res, 2);                                       /* PS2 card */
            if (rd(b, 16)) { int fc = free_clusters(0); mw32(res + 4, (u32)fc); if (g_log_mc) LOG("mc: card 1 has %d free clusters\n", fc); }   /* free 1 KB clusters */
            if (rd(b, 12)) mw32(res + 0x90, 1);                                /* formatted */
            r = seen[0] ? OK : CHANGED; seen[0] = 1;
        } else {
            if (rd(b, 20)) mw32(res, 0);
            if (rd(b, 16)) mw32(res + 4, 0);
            if (rd(b, 12)) mw32(res + 0x90, 0);
            r = -10;                                                           /* no card */
        }
        break; }
    case 2: r = mc_open((int)rd(b, 0), (const char *)b + 20, rd(b, 8)); break;
    case 3: { u32 k = rd(b, 0); if (k < 32 && fds[k]) { fclose(fds[k]); fds[k] = 0; } r = 0; break; }
    case 4: { u32 k = rd(b, 0); if (k >= 32 || !fds[k]) { r = -4; break; }     /* Seek */
        int org = (int)rd(b, 20);
        _fseeki64(fds[k], (s32)rd(b, 16), org == 1 ? SEEK_CUR : org == 2 ? SEEK_END : SEEK_SET);
        r = (int)_ftelli64(fds[k]); break; }
    case 5: { u32 k = rd(b, 0), n = rd(b, 12), dst = rd(b, 24), res = rd(b, 28);   /* Read */
        if (res) { mw32(res, 0); mw32(res + 4, 0); }
        if (k >= 32 || !fds[k]) { r = -4; break; }
        u8 *tmp = malloc(n ? n : 1); size_t got = fread(tmp, 1, n, fds[k]);
        for (size_t i = 0; i < got; i++) mw8(dst + (u32)i, tmp[i]);
        free(tmp); r = (int)got; break; }
    case 6: { u32 k = rd(b, 0), head = rd(b, 20), body = rd(b, 12), src = rd(b, 24);   /* Write */
        if (k >= 32 || !fds[k]) { r = -4; break; }
        if (head > 16) head = 16;
        size_t put = fwrite(b + 32, 1, head, fds[k]);
        if (body && src) {
            u8 *tmp = malloc(body); for (u32 i = 0; i < body; i++) tmp[i] = mr8(src + i);
            put += fwrite(tmp, 1, body, fds[k]); free(tmp);
        }
        r = (int)put; break; }
    case 10: { u32 k = rd(b, 0); if (k < 32 && fds[k]) fflush(fds[k]); r = 0; break; }   /* Flush */
    case 12: {                                                                 /* ChDir(port, slot, new, old*) */
        int port = (int)rd(b, 0) & 1; const char *p = (const char *)b + 20;
        if (rd(b, 16)) { const char *c = cwd[port]; u32 i = 0; for (; c[i] && i < 1023; i++) mw8(MC_CURDIR_BUF + i, (u8)c[i]); mw8(MC_CURDIR_BUF + i, 0); }
        if (!p[0]) { r = 0; break; }
        char hp[2048]; struct _stat64 st; host_path(port, p, hp, sizeof hp);
        if (_stat64(hp, &st) || !(st.st_mode & _S_IFDIR)) { r = NOENTRY; break; }
        if (p[0] == '/') snprintf(cwd[port], sizeof cwd[port], "%s", p);
        else { char t[1024]; snprintf(t, sizeof t, "%s/%s", strcmp(cwd[port], "/") ? cwd[port] : "", p); snprintf(cwd[port], sizeof cwd[port], "%s", t); }
        r = 0; break; }
    case 13: r = (rd(b, 0) & 1) ? NOENTRY : get_dir(0, (const char *)b + 20, (int)rd(b, 12), rd(b, 16), rd(b, 8) != 0); break;   /* GetDir; [8] flags: 0 first, 1 next */
    case 14: {   /* SetFileInfo: flags 1 created, 2 modified (host file times), 4 attributes (not kept), 0x10 name */
        int port = (int)rd(b, 0) & 1; u32 flags = rd(b, 8), info = rd(b, 16);
        if ((flags & 3) && info) { char hp[2048]; host_path(port, (const char *)b + 20, hp, sizeof hp); set_times(hp, info, flags & 1, flags & 2); }
        if (flags & 0x10 && info) {
            char nn[33]; for (int i = 0; i < 32; i++) nn[i] = (char)mr8(info + 32 + i); nn[32] = 0;
            if (!nn[0] || strpbrk(nn, "/\\:") || !strcmp(nn, ".") || !strcmp(nn, "..")) { r = NOENTRY; break; }   /* the new name must stay a plain file name */
            char from[2048], to[2048]; host_path(port, (const char *)b + 20, from, sizeof from);
            snprintf(to, sizeof to, "%s", from); char *s = strrchr(to, '\\'); if (s) snprintf(s + 1, sizeof to - (size_t)(s + 1 - to), "%s", nn);
            r = rename(from, to) ? NOENTRY : 0;
        } else r = 0;
        break; }
    case 15: {                                                                 /* Delete */
        char hp[2048]; struct _stat64 st; host_path((int)rd(b, 0), (const char *)b + 20, hp, sizeof hp);
        if (_stat64(hp, &st)) r = NOENTRY;
        else if (st.st_mode & _S_IFDIR) r = _rmdir(hp) ? NOTEMPTY : 0;
        else r = remove(hp) ? DENIED : 0;
        break; }
    case 16: case 17: r = 0; break;                                            /* Format / Unformat: nothing to do */
    default: LOG("mc: unknown fn %u\n", fn); r = -1; break;
    }
    log_call(fn, b, r);
    wr(b, 0, (u32)r);
}
