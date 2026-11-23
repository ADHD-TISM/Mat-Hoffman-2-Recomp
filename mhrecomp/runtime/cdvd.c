/* Sony CD/DVD file server (cdvdfsv.irx) at RPC level, for games that use the
 * stock libcdvd on the EE instead of a module of their own.
 *
 * Servers (sid): 0x80000592 init, 0x80000593 S commands, 0x80000595 N
 * commands (read, seek, ...), 0x80000597 search file, 0x8000059a disk ready.
 * sceCdRead data goes straight into EE memory, as the real server does with
 * SIF DMA; the read ends at once (no seek or read time is modelled).
 *
 * The disc is the image named by g_disc_image: a plain .iso (2048-byte
 * sectors) or a raw .bin (2352-byte MODE2 sectors, data at offset 24). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "ee.h"
#include "rt.h"
#include "iop.h"

extern const char *g_disc_image;
extern int g_log_sys;

static FILE *img;
static int sec_size = 2048, sec_off = 0;

static void img_open(void) {
    if (img) return;
    img = fopen(g_disc_image, "rb");
    if (!img) { LOG("cdvd: cannot open disc image %s\n", g_disc_image); exit(1); }
    _fseeki64(img, 0, SEEK_END);
    long long n = _ftelli64(img);
    if (n % 2352 == 0 && n % 2048 != 0) { sec_size = 2352; sec_off = 24; }
}

/* read `count` sectors at `lsn` into host memory; missing sectors read as 0 */
void cdvd_read_host(u32 lsn, u32 count, u8 *dst) {
    img_open();
    if (sec_size == 2048) {
        _fseeki64(img, (long long)lsn * 2048, SEEK_SET);
        size_t got = fread(dst, 1, (size_t)count * 2048, img);
        if (got < (size_t)count * 2048) memset(dst + got, 0, (size_t)count * 2048 - got);
        return;
    }
    static u8 raw[2352];
    for (u32 k = 0; k < count; k++) {
        _fseeki64(img, (long long)(lsn + k) * 2352, SEEK_SET);
        if (fread(raw, 1, 2352, img) != 2352) memset(raw, 0, sizeof raw);
        memcpy(dst + k * 2048, raw + 24, 2048);
    }
}

static void ee_put(u32 ea, const u8 *src, u32 n) {
    while (n) {
        u32 room = 0x1000 - (ea & 0xfff), c = n < room ? n : room;
        u8 *p = ee_ptr(ea);
        if (p) memcpy(p, src, c);
        ea += c; src += c; n -= c;
    }
}

/* ISO9660 lookup of "\DIR\NAME.EXT;1" (any case, either slash). 1 if found. */
int cdvd_find(const char *path, u32 *lsn, u32 *size) {
    static u8 sec[2048];
    cdvd_read_host(16, 1, sec);
    if (memcmp(sec + 1, "CD001", 5)) return 0;
    u32 dl, ds; memcpy(&dl, sec + 158, 4); memcpy(&ds, sec + 166, 4);
    const char *p = path;
    if (!strncmp(p, "cdrom0:", 7)) p += 7;
    for (;;) {
        while (*p == '\\' || *p == '/') p++;
        if (!*p) return 0;
        char part[64]; int n = 0;
        while (*p && *p != '\\' && *p != '/' && n < 63) part[n++] = (char)toupper((unsigned char)*p++);
        part[n] = 0;
        int last = !*p;
        if (last && !strchr(part, ';') && n < 61) { strcat(part, ";1"); n += 2; }
        int found = 0;
        for (u32 s = 0; s < (ds + 2047) / 2048 && !found; s++) {
            cdvd_read_host(dl + s, 1, sec);
            for (u32 o = 0; o < 2048 && sec[o]; o += sec[o]) {
                u8 *r = sec + o; int nl = r[32];
                char nm[64]; if (nl > 63) nl = 63;
                for (int k = 0; k < nl; k++) nm[k] = (char)toupper(r[33 + k]);
                nm[nl] = 0;
                int isdir = r[25] & 2;
                if (!last && isdir) { char *sc = strchr(nm, ';'); if (sc) *sc = 0; }
                if (strcmp(nm, part) || (!last && !isdir)) continue;
                memcpy(&dl, r + 2, 4); memcpy(&ds, r + 10, 4);
                found = 1; break;
            }
        }
        if (!found) return 0;
        if (last) { *lsn = dl; *size = ds; return 1; }
    }
}

static u32 rd(const u8 *b, int i) { u32 v; memcpy(&v, b + 4 * i, 4); return v; }
static void wr(u8 *b, int i, u32 v) { memcpy(b + 4 * i, &v, 4); }
static u8 bcd(int v) { return (u8)((v / 10) << 4 | (v % 10)); }

/* 1 if the sid belongs to this server; the reply is written into buf */
int cdvd_rpc(u32 sid, u32 fn, u8 *buf, u32 send_size, u32 recv_size) {
    static int seen[4][64];
    switch (sid) {
    case 0x80000592:                    /* sceCdInit: result, cdvdfsv version, cdvdman version */
        memset(buf, 0, 16); wr(buf, 1, 0x00020214); wr(buf, 2, 0x00020214);
        return 1;
    case 0x8000059a:                    /* sceCdDiskReady: 2 = ready */
    case 0x8000059c:
        wr(buf, 0, 2);
        return 1;
    case 0x80000597: {                  /* sceCdSearchFile: { sceCdlFILE file; char name[256]; void *dest; } */
        char name[257]; memcpy(name, buf + 0x24, 256); name[256] = 0;
        u32 dest = send_size >= 0x128 ? rd(buf, 0x124 / 4) : 0;
        u32 lsn = 0, size = 0;
        int ok = cdvd_find(name, &lsn, &size);
        if (g_log_sys) LOG("cdvd: search '%s' -> %s lsn %u size %u\n", name, ok ? "found" : "NOT FOUND", lsn, size);
        if (ok) {
            u8 f[0x24]; memset(f, 0, sizeof f);
            wr(f, 0, lsn); wr(f, 1, size);
            const char *base = strrchr(name, '\\'); base = base ? base + 1 : name;
            strncpy((char *)f + 8, base, 15);
            if (dest) ee_put(dest, f, sizeof f);
            memcpy(buf, f, sizeof f);
        }
        memset(buf, 0, 4 > recv_size ? recv_size : 4);
        if (recv_size >= 4) wr(buf, 0, (u32)ok);
        return 1; }
    case 0x80000595: {                  /* N commands */
        u32 a0 = rd(buf, 0), a1 = rd(buf, 1), a2 = rd(buf, 2), a4 = rd(buf, 4), a5 = rd(buf, 5);
        switch (fn) {
        case 1: case 2: case 3: {       /* read: lsn, sectors, EE buffer, mode, EE head/tail struct, EE read-position word */
            static u8 *tmp; static u32 cap;
            if (a1 > 0x4000) { LOG("cdvd: read of %u sectors refused\n", a1); if (recv_size >= 4) wr(buf, 0, 0); return 1; }
            u32 bytes = a1 * 2048;
            if (bytes > cap) { tmp = realloc(tmp, bytes); cap = bytes; if (!tmp) { LOG("cdvd: out of memory\n"); exit(1); } }
            cdvd_read_host(a0, a1, tmp);
            ee_put(a2, tmp, bytes);
            if (a4) { u8 z[0x90]; memset(z, 0, sizeof z); ee_put(a4, z, sizeof z); }   /* no unaligned head or tail left to copy */
            if (a5) ee_put(a5, (const u8 *)&bytes, 4);
            if (g_log_sys) LOG("cdvd: read lsn %u x%u -> %08x\n", a0, a1, a2);
            if (a0 == 0) { void ee_backtrace(const char *why); ee_backtrace("cdvd: read at lsn 0"); }
            if (recv_size >= 4) wr(buf, 0, 0);
            return 1; }
        case 9: {                       /* sceCdSt*: lsn, sectors, EE buffer, subcommand, mode */
            /* Streaming read-ahead, answered at once from the image. Subcommands as the EE
               library sends them: 1 start(lsn), 2 read(sectors, buf), 3 stop, 4 seek(lsn),
               5 init, 6 stat, 7 pause, 8 resume, 9 seek and flush. A read answers the
               sectors delivered in the low 16 bits and the error in the high 16. */
            static u32 st_lsn; static int st_on; static int logged[16];
            u32 sub = rd(buf, 3), ret = 1;
            static u8 *tmp; static u32 cap;
            switch (sub) {
            case 1: st_lsn = a0; st_on = 1; break;
            case 2: {
                u32 n = a1 > 0x400 ? 0x400 : a1;
                if (!st_on) n = 0;
                if (n * 2048 > cap) { cap = n * 2048; tmp = realloc(tmp, cap); if (!tmp) { LOG("cdvd: out of memory\n"); exit(1); } }
                if (n) { cdvd_read_host(st_lsn, n, tmp); ee_put(a2, tmp, n * 2048); st_lsn += n; }
                ret = n;
                break; }
            case 3: st_on = 0; break;
            case 4: case 9: st_lsn = a0; break;
            case 6: ret = st_on ? 64 : 0; break;        /* sectors waiting in the stream buffer */
            default: break;                             /* 5 init, 7 pause, 8 resume */
            }
            if (g_log_sys || !logged[sub & 15]++) LOG("cdvd: stream cmd %u lsn %u sectors %u buf %08x -> %u\n", sub, a0, a1, a2, ret);
            if (recv_size >= 4) wr(buf, 0, ret);
            return 1; }
        case 14: if (recv_size >= 4) wr(buf, 0, 2); return 1;    /* disk ready */
        case 5: case 6: case 7: case 8:                          /* seek, standby, stop, pause */
            if (recv_size >= 4) wr(buf, 0, 0);
            return 1;
        default:
            if (!seen[0][fn & 63]++) LOG("cdvd: N command %u not handled (send %u recv %u)\n", fn, send_size, recv_size);
            if (recv_size && recv_size <= 0x1000) memset(buf, 0, recv_size);
            return 1;
        }
    }
    case 0x80000593: {                  /* S commands: word 0 of the reply is the result */
        if (recv_size && recv_size <= 0x1000) memset(buf, 0, recv_size);
        switch (fn) {
        case 0x01: {                    /* sceCdReadClock: result, then stat, sec, min, hour, pad, day, month, year (BCD) */
            wr(buf, 0, 1);
            u8 *c = buf + 4;
            c[0] = 0; c[1] = bcd(0); c[2] = bcd(0); c[3] = bcd(12); c[4] = 0; c[5] = bcd(1); c[6] = bcd(1); c[7] = bcd(2);
            return 1; }
        case 0x03: wr(buf, 0, 0x14); return 1;        /* sceCdGetDiskType: PS2 DVD */
        case 0x04: wr(buf, 0, 0); return 1;           /* sceCdGetError */
        case 0x05: wr(buf, 0, 1); wr(buf, 1, 0); return 1;   /* sceCdTrayReq: tray did not change */
        case 0x0c: wr(buf, 0, 0x0a); return 1;        /* sceCdStatus: paused */
        case 0x16: wr(buf, 0, 1); return 1;           /* sceCdBreak */
        case 0x22: wr(buf, 0, 1); return 1;           /* sceCdMmode */
        default:
            if (!seen[1][fn & 63]++) LOG("cdvd: S command %#x not handled (send %u recv %u)\n", fn, send_size, recv_size);
            wr(buf, 0, 1);
            return 1;
        }
    }
    }
    return 0;
}

/* Debug aid: likely return addresses on the guest stack (words that follow a
 * jal/jalr in the code), innermost first. */
void ee_backtrace(const char *why) {
    u32 sp = GW(29);
    LOG("%s: ra %08x, stack:", why, GW(31));
    for (u32 a = sp, n = 0; a < sp + 0x1000 && n < 24; a += 4) {
        u32 w = mr32(a);
        if (w < 0x100008 || w >= 0x440000 || (w & 3)) continue;
        u32 i = mr32(w - 8);
        if ((i >> 26) == 3 || (i & 0xfc00003f) == 9) { LOG(" %08x", w); n++; }
    }
    LOG("\n");
}
