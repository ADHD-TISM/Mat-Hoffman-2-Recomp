/* GS local memory (4 MB): the swizzled pixel layouts of every storage format.
 *
 * Memory is pages of 8 KB, a page is 32 blocks of 256 bytes, a block is 4
 * columns. Page size in pixels depends on the format (CT32 64x32, CT16
 * 64x64, T8 128x64, T4 128x128). Base pointers are in blocks (64 words),
 * buffer widths in units of 64 pixels. The 8- and 4-bit column layouts are
 * the CT32 column pattern with every other pair of rows rotated by 4. */
#include "gs.h"

u8 gs_vram[GS_VRAM_SIZE] __attribute__((aligned(64)));

static const u8 blk32[4][8] = {
    { 0,  1,  4,  5, 16, 17, 20, 21}, { 2,  3,  6,  7, 18, 19, 22, 23},
    { 8,  9, 12, 13, 24, 25, 28, 29}, {10, 11, 14, 15, 26, 27, 30, 31}};
static const u8 blk16[8][4] = {
    { 0,  2,  8, 10}, { 1,  3,  9, 11}, { 4,  6, 12, 14}, { 5,  7, 13, 15},
    {16, 18, 24, 26}, {17, 19, 25, 27}, {20, 22, 28, 30}, {21, 23, 29, 31}};
static const u8 blk16s[8][4] = {
    { 0,  2, 16, 18}, { 1,  3, 17, 19}, { 8, 10, 24, 26}, { 9, 11, 25, 27},
    { 4,  6, 20, 22}, { 5,  7, 21, 23}, {12, 14, 28, 30}, {13, 15, 29, 31}};
static const u8 col32[2][8] = {{0, 1, 4, 5, 8, 9, 12, 13}, {2, 3, 6, 7, 10, 11, 14, 15}};
/* halfword index inside a 16x2 column: pixels 8 apart share a word */
static const u8 col16[2][16] = {
    {0, 2, 8, 10, 16, 18, 24, 26, 1, 3, 9, 11, 17, 19, 25, 27},
    {4, 6, 12, 14, 20, 22, 28, 30, 5, 7, 13, 15, 21, 23, 29, 31}};

static inline u32 bw1(u32 bw) { return bw ? bw : 1; }

/* word index (0..1M) for 32-bit formats; z = 1 for the Z32/Z24 block order */
u32 gs_addr32(u32 bp, u32 bw, u32 x, u32 y, int z) {
    u32 page = (y >> 5) * bw1(bw) + (x >> 6);
    u32 b = blk32[(y >> 3) & 3][(x >> 3) & 7] ^ (z ? 24 : 0);
    u32 blk = bp + page * 32 + b;
    return (blk * 64 + ((y >> 1) & 3) * 16 + col32[y & 1][x & 7]) & 0xfffff;
}
/* halfword index; s = 16S layout, z = Z16 block order */
u32 gs_addr16(u32 bp, u32 bw, u32 x, u32 y, int s, int z) {
    u32 page = (y >> 6) * bw1(bw) + (x >> 6);
    u32 b = (s ? blk16s : blk16)[(y >> 3) & 7][(x >> 4) & 3] ^ (z ? 24 : 0);
    u32 blk = bp + page * 32 + b;
    return (blk * 128 + ((y >> 1) & 3) * 32 + col16[y & 1][x & 15]) & 0x1fffff;
}
/* byte index */
u32 gs_addr8(u32 bp, u32 bw, u32 x, u32 y) {
    u32 ppr = bw1(bw) >> 1; if (!ppr) ppr = 1;
    u32 page = (y >> 6) * ppr + (x >> 7);
    u32 blk = bp + page * 32 + blk32[(y >> 4) & 3][(x >> 4) & 7];
    u32 col = (y >> 2) & 3, r = y & 3;
    u32 w = col32[r & 1][(x & 7) ^ ((((r >> 1) ^ col) & 1) << 2)];
    return (blk * 256 + col * 64 + w * 4 + (r >> 1) + ((x >> 3) & 1) * 2) & 0x3fffff;
}
/* nibble index */
u32 gs_addr4(u32 bp, u32 bw, u32 x, u32 y) {
    u32 ppr = bw1(bw) >> 1; if (!ppr) ppr = 1;
    u32 page = (y >> 7) * ppr + (x >> 7);
    u32 blk = bp + page * 32 + blk16[(y >> 4) & 7][(x >> 5) & 3];
    u32 col = (y >> 2) & 3, r = y & 3;
    u32 w = col32[r & 1][(x & 7) ^ ((((r >> 1) ^ col) & 1) << 2)];
    return (blk * 512 + col * 128 + w * 8 + (r >> 1) + ((x >> 3) & 3) * 2) & 0x7fffff;
}

static inline u32 *vw(u32 i) { return (u32 *)gs_vram + i; }
static inline u16 *vh(u32 i) { return (u16 *)gs_vram + i; }

/* Read a pixel in its storage format, returned raw (CT16 as 16 bits, T8 as index...). */
u32 gs_read_pixel(u32 psm, u32 bp, u32 bw, u32 x, u32 y) {
    switch (psm) {
    case PSMCT32: case PSMZ32: return *vw(gs_addr32(bp, bw, x, y, psm == PSMZ32));
    case PSMCT24: case PSMZ24: return *vw(gs_addr32(bp, bw, x, y, psm == PSMZ24)) & 0xffffff;
    case PSMCT16: case PSMZ16: return *vh(gs_addr16(bp, bw, x, y, 0, psm == PSMZ16));
    case PSMCT16S: case PSMZ16S: return *vh(gs_addr16(bp, bw, x, y, 1, psm == PSMZ16S));
    case PSMT8: return gs_vram[gs_addr8(bp, bw, x, y)];
    case PSMT4: { u32 n = gs_addr4(bp, bw, x, y); return (gs_vram[n >> 1] >> ((n & 1) * 4)) & 15; }
    case PSMT8H: return *vw(gs_addr32(bp, bw, x, y, 0)) >> 24;
    case PSMT4HL: return (*vw(gs_addr32(bp, bw, x, y, 0)) >> 24) & 15;
    case PSMT4HH: return *vw(gs_addr32(bp, bw, x, y, 0)) >> 28;
    }
    return 0;
}

void gs_write_pixel(u32 psm, u32 bp, u32 bw, u32 x, u32 y, u32 v) {
    switch (psm) {
    case PSMCT32: case PSMZ32: *vw(gs_addr32(bp, bw, x, y, psm == PSMZ32)) = v; return;
    case PSMCT24: case PSMZ24: { u32 *p = vw(gs_addr32(bp, bw, x, y, psm == PSMZ24)); *p = (*p & 0xff000000u) | (v & 0xffffff); return; }
    case PSMCT16: case PSMZ16: *vh(gs_addr16(bp, bw, x, y, 0, psm == PSMZ16)) = (u16)v; return;
    case PSMCT16S: case PSMZ16S: *vh(gs_addr16(bp, bw, x, y, 1, psm == PSMZ16S)) = (u16)v; return;
    case PSMT8: gs_vram[gs_addr8(bp, bw, x, y)] = (u8)v; return;
    case PSMT4: { u32 n = gs_addr4(bp, bw, x, y); u8 *p = &gs_vram[n >> 1];
        *p = (n & 1) ? (u8)((*p & 0x0f) | (v << 4)) : (u8)((*p & 0xf0) | (v & 15)); return; }
    case PSMT8H: { u32 *p = vw(gs_addr32(bp, bw, x, y, 0)); *p = (*p & 0x00ffffffu) | (v << 24); return; }
    case PSMT4HL: { u32 *p = vw(gs_addr32(bp, bw, x, y, 0)); *p = (*p & 0xf0ffffffu) | ((v & 15) << 24); return; }
    case PSMT4HH: { u32 *p = vw(gs_addr32(bp, bw, x, y, 0)); *p = (*p & 0x0fffffffu) | ((v & 15) << 28); return; }
    }
}

/* one row of PSMCT32 / PSMCT24 pixels, x = 0..w-1 */
void gs_write_row32(u32 psm, u32 bp, u32 bw, u32 y, u32 w, const u32 *v) {
    if (psm == PSMCT24) {
        for (u32 x = 0; x < w; x++) { u32 *p = vw(gs_addr32(bp, bw, x, y, 0)); *p = (*p & 0xff000000u) | (v[x] & 0xffffff); }
    } else {
        for (u32 x = 0; x < w; x++) *vw(gs_addr32(bp, bw, x, y, 0)) = v[x];
    }
}

/* n pixels of a 32-bit-word format (CT32/CT24/Z32/Z24) from x0 on one row; x wraps at 2048.
 * Host -> local transfers and texture decodes use these instead of one call per pixel
 * (a 512x256 movie picture each was ~3-4 ms of the graphics thread). */
int gs_psm_span32(u32 psm) { return psm == PSMCT32 || psm == PSMCT24 || psm == PSMZ32 || psm == PSMZ24; }
void gs_write_span32(u32 psm, u32 bp, u32 bw, u32 x0, u32 y, u32 n, const u32 *v) {
    int z = psm == PSMZ32 || psm == PSMZ24;
    if (psm == PSMCT24 || psm == PSMZ24) {
        for (u32 i = 0; i < n; i++) { u32 *p = vw(gs_addr32(bp, bw, (x0 + i) & 2047, y, z)); *p = (*p & 0xff000000u) | (v[i] & 0xffffff); }
    } else {
        for (u32 i = 0; i < n; i++) *vw(gs_addr32(bp, bw, (x0 + i) & 2047, y, z)) = v[i];
    }
}
void gs_read_span32(u32 psm, u32 bp, u32 bw, u32 y, u32 n, u32 *out) {
    int z = psm == PSMZ32 || psm == PSMZ24;
    u32 m = (psm == PSMCT24 || psm == PSMZ24) ? 0xffffffu : 0xffffffffu;
    for (u32 x = 0; x < n; x++) out[x] = *vw(gs_addr32(bp, bw, x, y, z)) & m;
}

/* Rows of any format: the format switch once per row instead of once per pixel
 * (T8 / T4 / CT16 texture uploads and decodes were most of the GS stage in a
 * level: ~23 a frame in Mat Hoffman 2). x wraps at 2048. Values are raw, as
 * gs_read_pixel returns them and gs_write_pixel takes them. */
#define A32(z) (gs_addr32(bp, bw, (x0 + i) & 2047, y, (z)))
void gs_write_row(u32 psm, u32 bp, u32 bw, u32 x0, u32 y, u32 n, const u32 *v) {
    u32 i;
    switch (psm) {
    case PSMCT32: case PSMZ32: case PSMCT24: case PSMZ24: gs_write_span32(psm, bp, bw, x0, y, n, v); return;
    case PSMCT16: case PSMZ16: for (i = 0; i < n; i++) *vh(gs_addr16(bp, bw, (x0 + i) & 2047, y, 0, psm == PSMZ16)) = (u16)v[i]; return;
    case PSMCT16S: case PSMZ16S: for (i = 0; i < n; i++) *vh(gs_addr16(bp, bw, (x0 + i) & 2047, y, 1, psm == PSMZ16S)) = (u16)v[i]; return;
    case PSMT8: for (i = 0; i < n; i++) gs_vram[gs_addr8(bp, bw, (x0 + i) & 2047, y)] = (u8)v[i]; return;
    case PSMT4:
        for (i = 0; i < n; i++) {
            u32 a = gs_addr4(bp, bw, (x0 + i) & 2047, y); u8 *p = &gs_vram[a >> 1];
            *p = (a & 1) ? (u8)((*p & 0x0f) | (v[i] << 4)) : (u8)((*p & 0xf0) | (v[i] & 15));
        }
        return;
    case PSMT8H: for (i = 0; i < n; i++) { u32 *p = vw(A32(0)); *p = (*p & 0x00ffffffu) | (v[i] << 24); } return;
    case PSMT4HL: for (i = 0; i < n; i++) { u32 *p = vw(A32(0)); *p = (*p & 0xf0ffffffu) | ((v[i] & 15) << 24); } return;
    case PSMT4HH: for (i = 0; i < n; i++) { u32 *p = vw(A32(0)); *p = (*p & 0x0fffffffu) | ((v[i] & 15) << 28); } return;
    }
}
void gs_read_row(u32 psm, u32 bp, u32 bw, u32 x0, u32 y, u32 n, u32 *out) {
    u32 i;
    switch (psm) {
    case PSMCT32: case PSMZ32: for (i = 0; i < n; i++) out[i] = *vw(A32(psm == PSMZ32)); return;
    case PSMCT24: case PSMZ24: for (i = 0; i < n; i++) out[i] = *vw(A32(psm == PSMZ24)) & 0xffffff; return;
    case PSMCT16: case PSMZ16: for (i = 0; i < n; i++) out[i] = *vh(gs_addr16(bp, bw, (x0 + i) & 2047, y, 0, psm == PSMZ16)); return;
    case PSMCT16S: case PSMZ16S: for (i = 0; i < n; i++) out[i] = *vh(gs_addr16(bp, bw, (x0 + i) & 2047, y, 1, psm == PSMZ16S)); return;
    case PSMT8: for (i = 0; i < n; i++) out[i] = gs_vram[gs_addr8(bp, bw, (x0 + i) & 2047, y)]; return;
    case PSMT4: for (i = 0; i < n; i++) { u32 a = gs_addr4(bp, bw, (x0 + i) & 2047, y); out[i] = (gs_vram[a >> 1] >> ((a & 1) * 4)) & 15; } return;
    case PSMT8H: for (i = 0; i < n; i++) out[i] = *vw(A32(0)) >> 24; return;
    case PSMT4HL: for (i = 0; i < n; i++) out[i] = (*vw(A32(0)) >> 24) & 15; return;
    case PSMT4HH: for (i = 0; i < n; i++) out[i] = *vw(A32(0)) >> 28; return;
    }
    for (i = 0; i < n; i++) out[i] = 0;
}
#undef A32

int gs_psm_bpp(u32 psm) {
    switch (psm) {
    case PSMCT32: case PSMZ32: case PSMCT24: case PSMZ24: case PSMT8H: case PSMT4HL: case PSMT4HH: return 32;
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: return 16;
    case PSMT8: return 8;
    case PSMT4: return 4;
    }
    return 32;
}
/* transfer bits per pixel (what the host sends): 24-bit formats are packed */
int gs_psm_xfer_bpp(u32 psm) {
    switch (psm) {
    case PSMCT24: case PSMZ24: return 24;
    case PSMT8H: return 8;
    case PSMT4HL: case PSMT4HH: return 4;
    }
    return gs_psm_bpp(psm);
}

/* page size in pixels for a format */
void gs_page_dims(u32 psm, u32 *pw, u32 *ph) {
    switch (gs_psm_bpp(psm)) {
    case 32: *pw = 64; *ph = 32; return;
    case 16: *pw = 64; *ph = 64; return;
    case 8: *pw = 128; *ph = 64; return;
    default: *pw = 128; *ph = 128; return;
    }
}

/* Block range [first, last] touched by a w x h rectangle at (x, y). Pages
 * are rows of bw (bw/2 for 8/4-bit) pages, so a rectangle covers whole page
 * rows; this is conservative inside a row. */
void gs_rect_blocks(u32 psm, u32 bp, u32 bw, u32 x, u32 y, u32 w, u32 h, u32 *first, u32 *last) {
    u32 pw, ph; gs_page_dims(psm, &pw, &ph);
    u32 ppr = bw1(bw); if (gs_psm_bpp(psm) <= 8) { ppr >>= 1; if (!ppr) ppr = 1; }
    if (!w || !h) { *first = bp; *last = bp; return; }
    u32 p0 = (y / ph) * ppr + x / pw, p1 = ((y + h - 1) / ph) * ppr + (x + w - 1) / pw;
    *first = bp + p0 * 32; *last = bp + p1 * 32 + 31;
}
