/* GS: front end (gs.c: GIF, registers, primitives, transfers), local memory
 * layouts (gs_mem.c) and the OpenGL back end (gs_gl.c). */
#pragma once
#include <stdint.h>
#include <string.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef int32_t s32;

#define GS_VRAM_SIZE (4u << 20)
extern u8 gs_vram[GS_VRAM_SIZE];

enum {
    PSMCT32 = 0x00, PSMCT24 = 0x01, PSMCT16 = 0x02, PSMCT16S = 0x0a,
    PSMT8 = 0x13, PSMT4 = 0x14, PSMT8H = 0x1b, PSMT4HL = 0x24, PSMT4HH = 0x2c,
    PSMZ32 = 0x30, PSMZ24 = 0x31, PSMZ16 = 0x32, PSMZ16S = 0x3a,
};

u32 gs_addr32(u32 bp, u32 bw, u32 x, u32 y, int z);
u32 gs_addr16(u32 bp, u32 bw, u32 x, u32 y, int s, int z);
u32 gs_addr8(u32 bp, u32 bw, u32 x, u32 y);
u32 gs_addr4(u32 bp, u32 bw, u32 x, u32 y);
u32 gs_read_pixel(u32 psm, u32 bp, u32 bw, u32 x, u32 y);
void gs_write_pixel(u32 psm, u32 bp, u32 bw, u32 x, u32 y, u32 v);
void gs_write_row(u32 psm, u32 bp, u32 bw, u32 x0, u32 y, u32 n, const u32 *v);   /* gs_mem.c: n raw pixels of one row */
void gs_read_row(u32 psm, u32 bp, u32 bw, u32 x0, u32 y, u32 n, u32 *out);
void gs_write_row32(u32 psm, u32 bp, u32 bw, u32 y, u32 w, const u32 *v);
int gs_psm_span32(u32 psm);
void gs_write_span32(u32 psm, u32 bp, u32 bw, u32 x0, u32 y, u32 n, const u32 *v);
void gs_read_span32(u32 psm, u32 bp, u32 bw, u32 y, u32 n, u32 *out);
int gs_psm_bpp(u32 psm);
int gs_psm_xfer_bpp(u32 psm);
void gs_page_dims(u32 psm, u32 *pw, u32 *ph);
void gs_rect_blocks(u32 psm, u32 bp, u32 bw, u32 x, u32 y, u32 w, u32 h, u32 *first, u32 *last);

/* ---- what the back end needs to draw a batch ---------------------------- */
typedef struct {
    float x, y, z;          /* GS pixels (offset removed), z as 0..1 */
    u8 r, g, b, a;
    float s, t, q;          /* STQ, or UV in texels with q = 1 */
    float fog;              /* 0..1 */
} gs_vertex;

typedef struct {            /* everything a batch shares; memcmp'd to merge */
    u32 prim;               /* 0 points, 1 lines, 2 triangles */
    u32 fbp, fbw, fpsm, fbmsk;   /* FRAME (fbp in blocks) */
    u32 zbp, zpsm, zmsk;    /* ZBUF (zbp in blocks) */
    u32 zte, ztst, ate, atst, aref, afail, date, datm;
    u32 abe, alpha_a, alpha_b, alpha_c, alpha_d, alpha_fix, pabe, fba;
    u32 tme, fst, tbp, tbw, tpsm, tw, th, tcc, tfx;
    u32 cbp, cpsm, csm, csa, clut_hash;
    u32 ta0, ta1, aem;
    u32 wms, wmt, minu, maxu, minv, maxv;
    u32 mmag, mmin;
    u32 mxl, lcm, mipl, mipk;   /* TEX1: max mip level, LOD method, L, K (K as signed 7.4 fixed) */
    u32 miptbp[6], miptbw[6];   /* MIPTBP1/2: levels 1..6 (only up to mxl are filled) */
    u32 fge, fogcol;
    u32 scx0, scx1, scy0, scy1;
    u32 colclamp, dthe;
} gs_draw_state;

/* gs_gl.c */
int  gsgl_init(int show_window, int scale);
void gsgl_draw(const gs_draw_state *st, const gs_vertex *v, u32 n, int same);   /* n vertices, prim from st; same: st is what the previous call passed */
void gsgl_flush(void);
/* local memory was written by the host (or a local->local copy): blocks [b0, b1] */
void gsgl_vram_written(u32 psm, u32 bp, u32 bw, u32 x, u32 y, u32 w, u32 h);
/* something is about to read local memory blocks [b0, b1]: bring GPU drawing back */
void gsgl_sync_vram(u32 b0, u32 b1);
/* show fbp/fbw/psm rectangle (dbx, dby, w, h); fbw == 0 shows black */
void gsgl_present_rect(u32 fbp, u32 fbw, u32 psm, u32 dbx, u32 dby, u32 w, u32 h);
/* gs.c, for the back end */
void gs_decode_texture(const gs_draw_state *s, u32 *out);
void gs_init(int show_window, int scale);
extern int g_log_gs;
void gsgl_pump(void);
