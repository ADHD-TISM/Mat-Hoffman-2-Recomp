/* GS front end: GIF tag parsing for the three paths, the GS register file,
 * vertex queue and primitive assembly, host<->local transfers and CLUT
 * loads. Drawing goes to the OpenGL back end (gs_gl.c) as batches of
 * triangles/lines with a gs_draw_state; local memory (gs_vram) lives here
 * and the back end keeps it coherent with its render targets. */
#include "rt.h"
#include "gs.h"
#include <stdlib.h>
#include <math.h>
#include <time.h>

int g_log_gs;
static u64 n_gif_qw, n_prims, n_xfer_px;

/* ---- registers ----------------------------------------------------------- */
static u64 R[0x80];
enum {
    PRIM = 0x00, RGBAQ = 0x01, ST = 0x02, UV = 0x03, XYZF2 = 0x04, XYZ2 = 0x05, TEX0_1 = 0x06, TEX0_2 = 0x07,
    CLAMP_1 = 0x08, CLAMP_2 = 0x09, FOG = 0x0a, XYZF3 = 0x0c, XYZ3 = 0x0d, TEX1_1 = 0x14, TEX1_2 = 0x15,
    TEX2_1 = 0x16, TEX2_2 = 0x17, XYOFFSET_1 = 0x18, XYOFFSET_2 = 0x19, PRMODECONT = 0x1a, PRMODE = 0x1b,
    TEXCLUT = 0x1c, SCANMSK = 0x22, MIPTBP1_1 = 0x34, MIPTBP1_2 = 0x35, MIPTBP2_1 = 0x36, MIPTBP2_2 = 0x37, TEXA = 0x3b, FOGCOL = 0x3d, TEXFLUSH = 0x3f, SCISSOR_1 = 0x40,
    SCISSOR_2 = 0x41, ALPHA_1 = 0x42, ALPHA_2 = 0x43, DIMX = 0x44, DTHE = 0x45, COLCLAMP = 0x46,
    TEST_1 = 0x47, TEST_2 = 0x48, PABE = 0x49, FBA_1 = 0x4a, FBA_2 = 0x4b, FRAME_1 = 0x4c, FRAME_2 = 0x4d,
    ZBUF_1 = 0x4e, ZBUF_2 = 0x4f, BITBLTBUF = 0x50, TRXPOS = 0x51, TRXREG = 0x52, TRXDIR = 0x53,
    HWREG = 0x54, SIGNAL = 0x60, FINISH = 0x61, LABEL = 0x62,
};
#define F(v, lo, n) ((u32)(((v) >> (lo)) & ((1ull << (n)) - 1)))

static float cur_q = 1.0f;

/* ---- CLUT buffer: 512 halfwords; CT32 entries keep their high half at +256 */
static u16 clut[512];
static u32 clut_hash, cbp0, cbp1;
/* bumped by every register write that is not vertex data and by CLUT changes:
   draw_prim reuses its decoded state until then */
static u32 st_gen = 1;
static u32 clut_ver = 1;      /* bumped whenever clut[] changes */

static void clut_load(u64 tex0) {
    u32 psm = F(tex0, 20, 6), cbp = F(tex0, 37, 14), cpsm = F(tex0, 51, 4), csm = F(tex0, 55, 1);
    u32 csa = F(tex0, 56, 5), cld = F(tex0, 61, 3);
    if (psm != PSMT8 && psm != PSMT4 && psm != PSMT8H && psm != PSMT4HL && psm != PSMT4HH) return;
    switch (cld) {
    case 0: return;
    case 1: break;
    case 2: cbp0 = cbp; break;
    case 3: cbp1 = cbp; break;
    case 4: if (cbp == cbp0) return; cbp0 = cbp; break;
    case 5: if (cbp == cbp1) return; cbp1 = cbp; break;
    default: return;
    }
    int n = (psm == PSMT8 || psm == PSMT8H) ? 256 : 16;
    int c16 = (cpsm == PSMCT16 || cpsm == PSMCT16S);
    gsgl_sync_vram(cbp, cbp + 31);
    if (csm == 0) {   /* CSM1: the entries read for this CLUT source, cached until its VRAM
         * page changes (gsgl_vram_gen: host writes and target write-backs bump it). The
         * 16x16 / 8x2 CLUT lies inside cbp..cbp+31; a CSM2 row may not, so it is read each time. */
        extern u32 gsgl_vram_gen(u32 b0, u32 b1);
        /* applied_ver / applied_csa: clut[] was not changed since this entry was
           applied there, so applying it again changes nothing */
        static struct { u32 cbp, cpsm, n, gen, used, applied_ver, applied_csa, applied_c16; u64 stamp; u32 v[256]; } cc[32];
        static u64 clock;
        u32 gen = gsgl_vram_gen(cbp, cbp + 31);
        int k, hit = -1, lru = 0;
        for (k = 0; k < 32; k++) {
            if (cc[k].used && cc[k].cbp == cbp && cc[k].cpsm == cpsm && cc[k].n == (u32)n) { hit = k; break; }
            if (!cc[k].used || cc[k].stamp < cc[lru].stamp) lru = k;
        }
        if (hit < 0 || cc[hit].gen != gen) {
            k = hit >= 0 ? hit : lru;
            cc[k].used = 1; cc[k].cbp = cbp; cc[k].cpsm = cpsm; cc[k].n = (u32)n; cc[k].gen = gen; cc[k].applied_ver = 0;
            /* 16x16 (8x2 for 4-bit) pixels; T8 entries have bits 3 and 4 swapped */
            int w = n == 256 ? 16 : 8;
            for (int i = 0; i < n; i++) {
                int p = n == 256 ? ((i & 0xe7) | ((i & 0x08) << 1) | ((i & 0x10) >> 1)) : i;
                cc[k].v[i] = gs_read_pixel(c16 ? cpsm : PSMCT32, cbp, 1, p % w, p / w);
            }
        }
        cc[k].stamp = ++clock;
        if (cc[k].applied_ver == clut_ver && cc[k].applied_csa == csa && cc[k].applied_c16 == (u32)c16) return;
        const u32 *src = cc[k].v; int changed = 0;
        for (int i = 0; i < n; i++) {
            if (c16) {
                u16 *d = &clut[(csa * 16 + i) & 511];
                if (*d != (u16)src[i]) { *d = (u16)src[i]; changed = 1; }
            } else {
                u32 o = (csa & 15) * 16 + i;
                u16 lo = (u16)src[i], hi = (u16)(src[i] >> 16);
                if (clut[o & 255] != lo || clut[256 + (o & 255)] != hi) { clut[o & 255] = lo; clut[256 + (o & 255)] = hi; changed = 1; }
            }
        }
        if (changed) {
            u32 h = 2166136261u;
            for (int i = 0; i < 512; i++) h = (h ^ clut[i]) * 16777619u;
            clut_hash = h; st_gen++; clut_ver++;
        }
        cc[k].applied_ver = clut_ver; cc[k].applied_csa = csa; cc[k].applied_c16 = (u32)c16;
        return;
    }
    {
        /* CSM2: a line of 256 entries at TEXCLUT (CBW, COU, COV), CT16 only */
        u64 tc = R[TEXCLUT];
        u32 cbw = F(tc, 0, 6), cou = F(tc, 6, 6) * 16, cov = F(tc, 12, 10);
        for (int i = 0; i < n; i++) clut[(csa * 16 + i) & 511] = (u16)gs_read_pixel(cpsm, cbp, cbw, cou + i, cov);
    }
    u32 h = 2166136261u;
    for (int i = 0; i < 512; i++) h = (h ^ clut[i]) * 16777619u;
    clut_hash = h; st_gen++; clut_ver++;
}

/* ---- texture decode for the back end ------------------------------------- */
static inline u32 exp16(u32 c, const gs_draw_state *s) {
    u32 r = (c & 31) << 3, g = ((c >> 5) & 31) << 3, b = ((c >> 10) & 31) << 3;
    u32 a = (c & 0x8000) ? s->ta1 : ((s->aem && !(c & 0x7fff)) ? 0 : s->ta0);
    return r | (g << 8) | (b << 16) | (a << 24);
}
static inline u32 exp24(u32 c, const gs_draw_state *s) {
    c &= 0xffffff;
    return c | ((s->aem && !c) ? 0 : s->ta0 << 24);
}
static inline u32 clut_entry(u32 i, const gs_draw_state *s, int four) {
    if (s->cpsm == PSMCT16 || s->cpsm == PSMCT16S) return exp16(clut[((four ? s->csa * 16 : 0) + i) & 511], s);
    u32 o = ((four ? (s->csa & 15) * 16 : 0) + i) & 255;
    return clut[o] | ((u32)clut[256 + o] << 16);
}

/* Decode a (1<<tw) x (1<<th) texture into RGBA8. */
void gs_decode_texture(const gs_draw_state *s, u32 *out) {
    u32 w = 1u << s->tw, h = 1u << s->th, bp = s->tbp, bw = s->tbw, psm = s->tpsm;
    u32 lut[256]; int pal = 0;
    if (psm == PSMT8 || psm == PSMT8H) { for (int i = 0; i < 256; i++) lut[i] = clut_entry(i, s, 0); pal = 1; }
    if (psm == PSMT4 || psm == PSMT4HL || psm == PSMT4HH) { for (int i = 0; i < 16; i++) lut[i] = clut_entry(i, s, 1); pal = 1; }
    if (psm == PSMCT32 || psm == PSMZ32) {                 /* whole rows, no per-pixel call */
        for (u32 y = 0; y < h; y++) gs_read_span32(psm, bp, bw, y, w, out + y * w);
        return;
    }
    if (psm == PSMCT24 || psm == PSMZ24) {
        for (u32 y = 0; y < h; y++) {
            gs_read_span32(psm, bp, bw, y, w, out + y * w);
            for (u32 x = 0; x < w; x++) out[y * w + x] = exp24(out[y * w + x], s);
        }
        return;
    }
    for (u32 y = 0; y < h; y++) {
        u32 *row = out + y * w;
        gs_read_row(psm, bp, bw, 0, y, w, row);         /* raw values, then expanded in place */
        if (pal) for (u32 x = 0; x < w; x++) row[x] = lut[row[x] & 255];
        else for (u32 x = 0; x < w; x++) row[x] = exp16(row[x], s);
    }
}
u32 gs_clut_hash(void) { return clut_hash; }

/* ---- host <-> local transfers ------------------------------------------- */
static struct {
    int active;
    u32 psm, bp, bw, sx, sy, w, h, x, y;
    u64 bits; int nbits;
} xfer;

static void xfer_done(void) {
    gsgl_vram_written(xfer.psm, xfer.bp, xfer.bw, xfer.sx, xfer.sy, xfer.w, xfer.h);
    xfer.active = 0;
}

static void xfer_put(u32 v) {
    gs_write_pixel(xfer.psm, xfer.bp, xfer.bw, (xfer.sx + xfer.x) & 2047, (xfer.sy + xfer.y) & 2047, v);
    n_xfer_px++;
    if (++xfer.x >= xfer.w) { xfer.x = 0; if (++xfer.y >= xfer.h) xfer_done(); }
}

static void xfer_data(const u8 *p, u32 bytes) {
    if (!xfer.active) { if (g_log_gs) LOG("gs: image data with no transfer (%u bytes)\n", bytes); return; }
    int bpp = gs_psm_xfer_bpp(xfer.psm);
    if (bpp == 32) {
        u32 npx = bytes / 4, i = 0;
        if (gs_psm_span32(xfer.psm)) {                       /* runs up to the end of each row */
            while (i < npx && xfer.active) {
                u32 run = xfer.w - xfer.x; if (run > npx - i) run = npx - i;
                u32 tmp[2048]; if (run > 2048) run = 2048;
                memcpy(tmp, p + 4 * i, run * 4);
                gs_write_span32(xfer.psm, xfer.bp, xfer.bw, (xfer.sx + xfer.x) & 2047, (xfer.sy + xfer.y) & 2047, run, tmp);
                n_xfer_px += run; xfer.x += run; i += run;
                if (xfer.x >= xfer.w) { xfer.x = 0; if (++xfer.y >= xfer.h) xfer_done(); }
            }
            return;
        }
        for (; i < npx && xfer.active; i++) { u32 v; memcpy(&v, p + 4 * i, 4); xfer_put(v); }
        return;
    }
    /* 24/16/8/4 bits: unpack up to the end of the row, then write the row at once */
    u32 tmp[2048], n = 0, mask = (u32)((1ull << bpp) - 1);
    for (u32 i = 0; i < bytes && xfer.active; i++) {
        xfer.bits |= (u64)p[i] << xfer.nbits; xfer.nbits += 8;
        while (xfer.nbits >= bpp && xfer.active) {
            tmp[n++] = (u32)xfer.bits & mask;
            xfer.bits >>= bpp; xfer.nbits -= bpp;
            if (n == 2048 || xfer.x + n >= xfer.w) {
                gs_write_row(xfer.psm, xfer.bp, xfer.bw, (xfer.sx + xfer.x) & 2047, (xfer.sy + xfer.y) & 2047, n, tmp);
                n_xfer_px += n; xfer.x += n; n = 0;
                if (xfer.x >= xfer.w) { xfer.x = 0; if (++xfer.y >= xfer.h) xfer_done(); }
            }
        }
    }
    if (n) {
        gs_write_row(xfer.psm, xfer.bp, xfer.bw, (xfer.sx + xfer.x) & 2047, (xfer.sy + xfer.y) & 2047, n, tmp);
        n_xfer_px += n; xfer.x += n;
    }
}

static void local_to_local(void) {
    u64 b = R[BITBLTBUF], p = R[TRXPOS], r = R[TRXREG];
    u32 sbp = F(b, 0, 14), sbw = F(b, 16, 6), spsm = F(b, 24, 6), dbp = F(b, 32, 14), dbw = F(b, 48, 6), dpsm = F(b, 56, 6);
    u32 ssx = F(p, 0, 11), ssy = F(p, 16, 11), dsx = F(p, 32, 11), dsy = F(p, 48, 11);
    u32 w = F(r, 0, 12), h = F(r, 32, 12);
    u32 b0, b1; gs_rect_blocks(spsm, sbp, sbw, ssx, ssy, w, h, &b0, &b1);
    gsgl_sync_vram(b0, b1);
    gs_rect_blocks(dpsm, dbp, dbw, dsx, dsy, w, h, &b0, &b1);
    gsgl_sync_vram(b0, b1);
    u32 *tmp = malloc((size_t)w * h * 4 + 4);
    for (u32 y = 0; y < h; y++) gs_read_row(spsm, sbp, sbw, ssx, (ssy + y) & 2047, w, tmp + y * w);
    for (u32 y = 0; y < h; y++) gs_write_row(dpsm, dbp, dbw, dsx, (dsy + y) & 2047, w, tmp + y * w);
    free(tmp);
    gsgl_vram_written(dpsm, dbp, dbw, dsx, dsy, w, h);
    if (g_log_gs) LOG("gs: local->local %ux%u %x(%x) -> %x(%x)\n", w, h, sbp, spsm, dbp, dpsm);
}

static u64 fu_l2h_count;   /* local->host transfers, for the GS_FEATURES report */
static void trxdir_write(u32 dir) {
    if (xfer.active) { if (g_log_gs) LOG("gs: transfer restarted with %u,%u left\n", xfer.x, xfer.y); xfer_done(); }
    u64 b = R[BITBLTBUF], p = R[TRXPOS], r = R[TRXREG];
    if (dir == 0) {
        xfer.psm = F(b, 56, 6); xfer.bp = F(b, 32, 14); xfer.bw = F(b, 48, 6);
        xfer.sx = F(p, 32, 11); xfer.sy = F(p, 48, 11);
        xfer.w = F(r, 0, 12); xfer.h = F(r, 32, 12);
        xfer.x = xfer.y = 0; xfer.bits = 0; xfer.nbits = 0;
        xfer.active = xfer.w && xfer.h;
        if (xfer.active) {
            u32 b0, b1; gs_rect_blocks(xfer.psm, xfer.bp, xfer.bw, xfer.sx, xfer.sy, xfer.w, xfer.h, &b0, &b1);
            gsgl_sync_vram(b0, b1);
        }
        if (g_log_gs > 1) LOG("gs: host->local %ux%u at %u,%u bp %x bw %u psm %x\n", xfer.w, xfer.h, xfer.sx, xfer.sy, xfer.bp, xfer.bw, xfer.psm);
    } else if (dir == 2) {
        local_to_local();
    } else if (dir == 1) {
        fu_l2h_count++;
        static int once; if (!once++) LOG("gs: local->host transfer (not supported)\n");
    }
}

/* ---- vertex queue and primitives ----------------------------------------- */
typedef struct { u32 x, y, z; u8 r, g, b, a; float s, t, q; u32 u, v, f; } qvert;
static qvert vq[3];
static int nvq;

static u32 cur_prim(void) { return F(R[PRIM], 0, 3); }
static u32 attrs(void) { return (R[PRMODECONT] & 1) ? (u32)R[PRIM] : (u32)R[PRMODE]; }

/* GS feature use, logged at exit with --log-gs or GS_FEATURES=1 (which "not done" parts matter) */
static int fu_on;   /* GS_FEATURES or GS logging: count features per primitive */
static u64 fu_n, fu_date, fu_dthe, fu_noclamp, fu_ad, fu_fixhi, fu_pabe, fu_fba, fu_abe, fu_ad_d, fu_zread;
static void fu_report(void) {
    LOG("gs: features in %llu prims: blend %llu, Ad factor %llu, FIX>0x80 %llu, Ad as A/B/D %llu, DATE %llu, dither %llu, "
        "COLCLAMP off %llu, PABE %llu, FBA %llu, local->host %llu, Z texture %llu\n",
        fu_n, fu_abe, fu_ad, fu_fixhi, fu_ad_d, fu_date, fu_dthe, fu_noclamp, fu_pabe, fu_fba, fu_l2h_count, fu_zread);
}
static void fu_count(const gs_draw_state *st) {
    fu_n++;
    fu_date += st->date; fu_dthe += st->dthe;
    if (st->abe) {
        fu_abe++;
        fu_noclamp += !st->colclamp;
        static int want = -1; if (want < 0) want = getenv("GS_FEATURES") != 0;
        if (!st->colclamp && want) {   /* which blends run with wrap, and when */
            static u64 seen[256 * 2]; u32 k = st->alpha_a | st->alpha_b << 2 | st->alpha_c << 4 | st->alpha_d << 6;
            u32 key = k * 2 + (st->alpha_fix > 0x40);
            if (!seen[key]++) LOG("gs: COLCLAMP off with blend A%u B%u C%u D%u fix %02x tme %u fpsm %x fbp %x fbw %u fbmsk %08x prim %u scissor %u-%u,%u-%u ztst %u zmsk %u at frame %llu\n",
                st->alpha_a, st->alpha_b, st->alpha_c, st->alpha_d, st->alpha_fix, st->tme, st->fpsm, st->fbp, st->fbw, st->fbmsk,
                st->prim, st->scx0, st->scx1, st->scy0, st->scy1, st->ztst, st->zmsk, (unsigned long long)g_frame);
        }
        fu_ad += st->alpha_c == 1; fu_fixhi += st->alpha_c == 2 && st->alpha_fix > 0x80;
        fu_ad_d += st->alpha_a == 1 || st->alpha_b == 1 || st->alpha_d == 1;
        fu_pabe += st->pabe;
    }
    fu_fba += st->fba;
    fu_zread += st->tme && (st->tpsm & 0x30) == 0x30;
}

static void build_state(gs_draw_state *st, int cls, u32 a) {
    memset(st, 0, sizeof *st);
    int c = (a >> 9) & 1;
    u64 frame = R[FRAME_1 + c], zbuf = R[ZBUF_1 + c], test = R[TEST_1 + c], alpha = R[ALPHA_1 + c];
    u64 tex0 = R[TEX0_1 + c], tex1 = R[TEX1_1 + c], clamp = R[CLAMP_1 + c], sc = R[SCISSOR_1 + c];
    st->prim = cls;
    st->fbp = F(frame, 0, 9) * 32; st->fbw = F(frame, 16, 6); st->fpsm = F(frame, 24, 6); st->fbmsk = (u32)(frame >> 32);
    st->ate = F(test, 0, 1); st->atst = F(test, 1, 3); st->aref = F(test, 4, 8); st->afail = F(test, 12, 2);
    st->date = F(test, 14, 1); st->datm = F(test, 15, 1); st->zte = F(test, 16, 1); st->ztst = F(test, 17, 2);
    if (!st->ate) { st->atst = 1; st->aref = 0; st->afail = 0; }
    if ((st->zte && st->ztst != 1) || !F(zbuf, 32, 1)) {
        st->zbp = F(zbuf, 0, 9) * 32; st->zpsm = F(zbuf, 24, 4) | 0x30; st->zmsk = F(zbuf, 32, 1);
    } else st->zmsk = 1;
    if (!st->zte) st->ztst = 1;
    st->abe = (a >> 6) & 1;
    if (cls == 1 && ((a >> 7) & 1)) st->abe = 1;   /* AA1 lines blend */
    if (st->abe) {
        st->alpha_a = F(alpha, 0, 2); st->alpha_b = F(alpha, 2, 2); st->alpha_c = F(alpha, 4, 2);
        st->alpha_d = F(alpha, 6, 2); st->alpha_fix = F(alpha, 32, 8); st->pabe = R[PABE] & 1;
    }
    st->fba = R[FBA_1 + c] & 1;
    st->tme = (a >> 4) & 1;
    if (st->tme) {
        st->fst = (a >> 8) & 1;
        st->tbp = F(tex0, 0, 14); st->tbw = F(tex0, 14, 6); st->tpsm = F(tex0, 20, 6);
        st->tw = F(tex0, 26, 4); st->th = F(tex0, 30, 4);
        if (st->tw > 10) st->tw = 10;
        if (st->th > 10) st->th = 10;
        st->tcc = F(tex0, 34, 1); st->tfx = F(tex0, 35, 2);
        int pal = gs_psm_bpp(st->tpsm) <= 8 || st->tpsm == PSMT8H || st->tpsm == PSMT4HL || st->tpsm == PSMT4HH;
        if (pal) {
            st->cbp = F(tex0, 37, 14); st->cpsm = F(tex0, 51, 4); st->csm = F(tex0, 55, 1); st->csa = F(tex0, 56, 5);
            st->clut_hash = clut_hash;
        }
        u64 texa = R[TEXA];
        int needs_texa = st->tpsm == PSMCT24 || st->tpsm == PSMCT16 || st->tpsm == PSMCT16S ||
                         (pal && (st->cpsm == PSMCT16 || st->cpsm == PSMCT16S));
        if (needs_texa) { st->ta0 = F(texa, 0, 8); st->aem = F(texa, 15, 1); st->ta1 = F(texa, 32, 8); }
        st->wms = F(clamp, 0, 2); st->wmt = F(clamp, 2, 2);
        st->minu = F(clamp, 4, 10); st->maxu = F(clamp, 14, 10); st->minv = F(clamp, 24, 10); st->maxv = F(clamp, 34, 10);
        st->mmag = F(tex1, 5, 1);
        st->mmin = F(tex1, 6, 3);
        st->lcm = F(tex1, 0, 1); st->mxl = F(tex1, 2, 3); st->mipl = F(tex1, 19, 2); st->mipk = F(tex1, 32, 12);
        if (st->mxl > 6) st->mxl = 6;
        if (st->mxl && st->mmin >= 2) {          /* mipmapping: MIPTBP1 (levels 1-3) and MIPTBP2 (4-6) */
            u64 m1 = R[MIPTBP1_1 + c], m2 = R[MIPTBP2_1 + c];
            for (u32 k = 0; k < st->mxl; k++) {
                u64 m = k < 3 ? m1 : m2; u32 sh = (k % 3) * 20;
                st->miptbp[k] = F(m, sh, 14); st->miptbw[k] = F(m, sh + 14, 6);
            }
        }
    }
    st->fge = (a >> 5) & 1;
    if (st->fge) st->fogcol = (u32)R[FOGCOL] & 0xffffff;
    st->scx0 = F(sc, 0, 11); st->scx1 = F(sc, 16, 11); st->scy0 = F(sc, 32, 11); st->scy1 = F(sc, 48, 11);
    st->colclamp = R[COLCLAMP] & 1;
    st->dthe = R[DTHE] & 1;
    if (fu_on) fu_count(st);
}

static void to_vertex(gs_vertex *o, const qvert *q, const qvert *col, u32 a, const gs_draw_state *st) {
    int c = (a >> 9) & 1;
    u64 off = R[XYOFFSET_1 + c];
    /* In interlaced field mode (SMODE2 INT+FFMD) the game moves its drawing half a line
     * (XYOFFSET y +8) on every other field, because a TV shows that field half a line
     * lower. Each field is shown progressively here, so that offset made the whole
     * picture bob. Drop the sub-line part of the offset in field mode. */
    u32 offy = F(off, 32, 16);
    if ((gs_priv[2] & 3) == 3) offy &= ~15u;
    o->x = ((float)q->x - (float)F(off, 0, 16)) / 16.0f;
    o->y = ((float)q->y - (float)offy) / 16.0f;
    u32 z = q->z;
    if (st->zpsm == PSMZ24 && z > 0xffffff) z = 0xffffff;
    if ((st->zpsm == PSMZ16 || st->zpsm == PSMZ16S) && z > 0xffff) z = 0xffff;
    o->z = (float)((double)z / 4294967296.0);
    o->r = col->r; o->g = col->g; o->b = col->b; o->a = col->a;
    if (st->fst) { o->s = q->u / 16.0f; o->t = q->v / 16.0f; o->q = 1.0f; }
    else { o->s = q->s; o->t = q->t; o->q = q->q; }
    o->fog = q->f / 255.0f;
}

/* the draw state for class cls (1 lines, 2 the rest) under attributes a:
   decoded again only after st_gen moved (a register or the CLUT changed).
   *same: it is the state the previous draw passed, so gsgl_draw may skip its compare */
static const gs_draw_state *get_state(int cls, u32 a, int *same) {
    static gs_draw_state cache[3]; static u32 gen[3], attr[3], id[3], next_id, last_id;
    if (gen[cls] != st_gen || attr[cls] != a) {
        build_state(&cache[cls], cls, a);
        gen[cls] = st_gen; attr[cls] = a; id[cls] = ++next_id;
    } else if (fu_on) fu_count(&cache[cls]);
    *same = id[cls] == last_id;
    last_id = id[cls];
    return &cache[cls];
}

static void draw_prim(int type) {
    u32 a = attrs();
    gs_vertex v[6];
    int iip = (a >> 3) & 1, same;
    n_prims++;
    switch (type) {
    case 0: {   /* point -> 1x1 quad */
        const gs_draw_state *st = get_state(2, a, &same);
        gs_vertex p; to_vertex(&p, &vq[0], &vq[0], a, st);
        for (int k = 0; k < 6; k++) v[k] = p;
        v[1].x += 1; v[2].y += 1; v[3].x += 1; v[4].x += 1; v[4].y += 1; v[5].y += 1;
        gsgl_draw(st, v, 6, same);
        break; }
    case 1: case 2: {
        const gs_draw_state *st = get_state(1, a, &same);
        to_vertex(&v[0], &vq[0], iip ? &vq[0] : &vq[1], a, st);
        to_vertex(&v[1], &vq[1], &vq[1], a, st);
        gsgl_draw(st, v, 2, same);
        break; }
    case 3: case 4: case 5: {
        const gs_draw_state *st = get_state(2, a, &same);
        for (int k = 0; k < 3; k++) to_vertex(&v[k], &vq[k], iip ? &vq[k] : &vq[2], a, st);
        gsgl_draw(st, v, 3, same);
        break; }
    case 6: {   /* sprite: corners from two vertices, colour/Z/fog from the second */
        const gs_draw_state *st = get_state(2, a, &same);
        gs_vertex p0, p1;
        to_vertex(&p0, &vq[0], &vq[1], a, st);
        to_vertex(&p1, &vq[1], &vq[1], a, st);
        if (!st->fst) {   /* no perspective on a sprite: divide now */
            p0.s /= p0.q; p0.t /= p0.q; p1.s /= p1.q; p1.t /= p1.q; p0.q = p1.q = 1.0f;
        }
        p0.z = p1.z; p0.fog = p1.fog;
        gs_vertex tr = p0, bl = p0;
        tr.x = p1.x; tr.s = p1.s;
        bl.y = p1.y; bl.t = p1.t;
        v[0] = p0; v[1] = tr; v[2] = bl; v[3] = tr; v[4] = p1; v[5] = bl;
        gsgl_draw(st, v, 6, same);
        break; }
    default:
        break;
    }
}

static void vertex_kick(u32 x, u32 y, u32 z, u32 f, int kick) {
    qvert *q = &vq[nvq];
    q->x = x; q->y = y; q->z = z; q->f = f;
    u64 c = R[RGBAQ];
    q->r = (u8)c; q->g = (u8)(c >> 8); q->b = (u8)(c >> 16); q->a = (u8)(c >> 24);
    fr qf; qf.u = (u32)(c >> 32); q->q = qf.f;
    fr s, t; s.u = (u32)R[ST]; t.u = (u32)(R[ST] >> 32); q->s = s.f; q->t = t.f;
    q->u = F(R[UV], 0, 14); q->v = F(R[UV], 16, 14);
    nvq++;
    int type = cur_prim();
    static const int need[8] = { 1, 2, 2, 3, 3, 3, 2, 1 };
    if (nvq < need[type]) return;
    if (kick && type != 7) draw_prim(type);
    switch (type) {
    case 2: vq[0] = vq[1]; nvq = 1; break;                  /* line strip */
    case 4: vq[0] = vq[1]; vq[1] = vq[2]; nvq = 2; break;   /* triangle strip */
    case 5: vq[1] = vq[2]; nvq = 2; break;                  /* triangle fan */
    default: nvq = 0;
    }
}

/* ---- register writes ----------------------------------------------------- */
static int gif_cur_path;   /* GIF path of the packet being parsed (gif_transfer), for early FINISH */
static void gs_write_reg(u32 r, u64 v) {
    r &= 0x7f;
    if (r != RGBAQ && r != ST && r != UV && r != XYZF2 && r != XYZF3 && r != XYZ2 && r != XYZ3 && r != FOG) st_gen++;
    switch (r) {
    case PRIM: R[PRIM] = v; nvq = 0; return;
    case XYZF2: case XYZF3:
        vertex_kick(F(v, 0, 16), F(v, 16, 16), F(v, 32, 24), F(v, 56, 8), r == XYZF2); return;
    case XYZ2: case XYZ3:
        vertex_kick(F(v, 0, 16), F(v, 16, 16), (u32)(v >> 32), F(R[FOG], 56, 8), r == XYZ2); return;
    case TEX0_1: case TEX0_2:
        if (g_log_gs > 2) LOG("gs: TEX0_%d = %016llx (tbp %llx)\n", r - TEX0_1 + 1, (unsigned long long)v, (unsigned long long)(v & 0x3fff));
        R[r] = v; clut_load(v); return;
    case TEX2_1: case TEX2_2: {
        u64 m = (0x3full << 20) | (0x7ffffffull << 37);   /* PSM, CBP, CPSM, CSM, CSA, CLD */
        u64 *t = &R[TEX0_1 + (r - TEX2_1)];
        *t = (*t & ~m) | (v & m);
        clut_load(*t);
        return; }
    case TRXDIR: R[r] = v; trxdir_write((u32)v & 3); return;
    case HWREG: { u8 b[8]; memcpy(b, &v, 8); xfer_data(b, 8); return; }
    case SIGNAL:
        if (g_gs_mt) { gst_post_csr(1); return; }    /* graphics thread: the EE applies it (gst_apply_csr) */
        gs_csr |= 1;
        if (!(gs_imr & 0x100)) intc_raise(0);
        return;
    case FINISH:
        gsgl_flush();
        if (g_gs_mt) {
            int gst_finish_reached(int path);
            if (gst_finish_reached(gif_cur_path)) gst_post_csr(2);   /* else the EE already set it (early FINISH) */
            return;
        }
        gs_csr |= 2;
        if (!(gs_imr & 0x200)) intc_raise(0);
        return;
    case LABEL: return;
    case TEXFLUSH: R[r] = v; return;
    default: R[r] = v; return;
    }
}

/* ---- GIF ----------------------------------------------------------------- */
typedef struct {
    u32 nloop, nreg, flg, loop, reg, eop;
    u64 regs;
    int active, half;    /* REGLIST: which 64-bit half is next */
} gif_path;
static gif_path gp[4];

static void packed_write(u32 reg, const u8 *q) {
    u64 lo, hi; memcpy(&lo, q, 8); memcpy(&hi, q + 8, 8);
    switch (reg) {
    case 0x0: gs_write_reg(PRIM, lo & 0x7ff); break;
    case 0x1: R[RGBAQ] = (lo & 0xff) | ((lo >> 24) & 0xff00) | ((hi & 0xff) << 16) | (((hi >> 32) & 0xff) << 24) |
                         ((u64)*(u32 *)&cur_q << 32);
              break;
    case 0x2: { fr qq; qq.u = (u32)hi; cur_q = qq.f; R[ST] = lo; break; }
    case 0x3: R[UV] = (lo & 0x3fff) | (((lo >> 32) & 0x3fff) << 16); break;
    case 0x4: {
        u32 x = (u32)lo & 0xffff, y = (u32)(lo >> 32) & 0xffff, z = (u32)(hi >> 4) & 0xffffff, f = (u32)(hi >> 36) & 0xff;
        vertex_kick(x, y, z, f, !((hi >> 47) & 1));
        break; }
    case 0x5: {
        u32 x = (u32)lo & 0xffff, y = (u32)(lo >> 32) & 0xffff, z = (u32)hi;
        vertex_kick(x, y, z, F(R[FOG], 56, 8), !((hi >> 47) & 1));
        break; }
    case 0xa: R[FOG] = ((hi >> 36) & 0xff) << 56; break;
    case 0xe: gs_write_reg((u32)hi & 0x7f, lo); break;
    case 0xf: break;
    default: gs_write_reg(reg, lo); break;
    }
}

void gif_transfer(const u8 *p, u32 nqw, int path) {
    {   /* on the VU stage of the split graphics threads: hand it to the GS stage (gsthread.c) */
        extern __thread int g_on_vu_stage; void gst2_gif(const u8 *p, u32 nqw, int path);
        if (g_on_vu_stage) { gst2_gif(p, nqw, path); return; }
    }
    gif_path *g = &gp[path & 3];
    gif_cur_path = path & 3;
    n_gif_qw += nqw;
    for (u32 i = 0; i < nqw; i++, p += 16) {
        if (!g->active) {
            u64 lo, hi; memcpy(&lo, p, 8); memcpy(&hi, p + 8, 8);
            g->nloop = F(lo, 0, 15); g->eop = F(lo, 15, 1); g->flg = F(lo, 58, 2);
            g->nreg = F(lo, 60, 4); if (!g->nreg) g->nreg = 16;
            g->regs = hi; g->loop = 0; g->reg = 0; g->half = 0;
            if (g->flg == 0 && F(lo, 46, 1)) gs_write_reg(PRIM, F(lo, 47, 11));
            if (g_log_gs > 2) LOG("gif%d: tag nloop %u flg %u nreg %u regs %016llx\n", path, g->nloop, g->flg, g->nreg, (unsigned long long)hi);
            g->active = g->nloop != 0;
            continue;
        }
        switch (g->flg) {
        case 0:   /* PACKED */
            packed_write((u32)(g->regs >> (4 * g->reg)) & 15, p);
            if (++g->reg >= g->nreg) { g->reg = 0; if (++g->loop >= g->nloop) g->active = 0; }
            break;
        case 1:   /* REGLIST: two registers per qword */
            for (int h = 0; h < 2 && g->active; h++) {
                u64 v; memcpy(&v, p + 8 * h, 8);
                u32 r = (u32)(g->regs >> (4 * g->reg)) & 15;
                if (r != 0xe && r != 0xf) gs_write_reg(r, v);
                if (++g->reg >= g->nreg) { g->reg = 0; if (++g->loop >= g->nloop) g->active = 0; }
            }
            break;
        default:  /* IMAGE */
            xfer_data(p, 16);
            if (++g->loop >= g->nloop) g->active = 0;
            break;
        }
    }
}

/* ---- display ------------------------------------------------------------- */
/* priv: the GS privileged registers as they were at this vblank (a copy when
 * the graphics thread presents, so the EE may flip DISPFB right after) */
void gs_present_from(const u64 *priv) {
    u64 pmode = priv[0], smode2 = priv[2];
    int circ = (pmode & 1) ? 0 : (pmode & 2) ? 1 : -1;
    static u64 last_dispfb = ~0ull, last_display = ~0ull, last_pmode = ~0ull;
    if (circ < 0) { gsgl_present_rect(0, 0, 0, 0, 0, 0, 0); return; }
    u64 dispfb = priv[7 + 2 * circ], display = priv[8 + 2 * circ];
    u32 fbp = F(dispfb, 0, 9) * 32, fbw = F(dispfb, 9, 6), psm = F(dispfb, 15, 5);
    u32 dbx = F(dispfb, 32, 11), dby = F(dispfb, 43, 11);
    u32 magh = F(display, 23, 4) + 1, magv = F(display, 27, 2) + 1;
    u32 w = (F(display, 32, 12) + 1) / magh, h = (F(display, 44, 11) + 1) / magv;
    if ((smode2 & 3) == 3) h /= 2;   /* interlaced field mode reads half the lines */
    /* FBP flips every frame with double buffering: only log other changes */
    if (g_log_gs && ((dispfb & ~0x1ffull) != (last_dispfb & ~0x1ffull) || display != last_display || pmode != last_pmode ||
                     (g_log_gs > 1 && dispfb != last_dispfb)))
        LOG("gs: display circuit %d fbp %x fbw %u psm %x at %u,%u size %ux%u (pmode %llx smode2 %llx)\n",
            circ + 1, fbp, fbw, psm, dbx, dby, w, h, (unsigned long long)pmode, (unsigned long long)smode2);
    last_dispfb = dispfb; last_display = display; last_pmode = pmode;
    gsgl_present_rect(fbp, fbw, psm, dbx, dby, w, h);
}

/* the in-game menu (menu.c): show the last frame again (the overlay is drawn on top) */
void gs_present_again(void) {
    if (g_gs_mt) gst_present(gs_priv); else gs_present_from(gs_priv);
}

/* EE side: GIF path 3 (DMA ch2, GIF FIFO) */
void gif_dma(const u8 *p, u32 nqw, int path) {
    if (g_gs_mt) gst_gif(p, nqw, path); else gif_transfer(p, nqw, path);
}

void gs_vsync(void) {
    if (g_gs_mt) gst_present(gs_priv); else gs_present_from(gs_priv);
    gsgl_pump();
    {   /* LOG_GS_FROM=<frame> LOG_GS_LEVEL=<n>: switch GS logging on late */
        static int init; static u64 from; static int lvl;
        if (!init) { init = 1; const char *e = getenv("LOG_GS_FROM"); if (e) { from = strtoull(e, 0, 10); lvl = getenv("LOG_GS_LEVEL") ? atoi(getenv("LOG_GS_LEVEL")) : 2; } }
        if (from && g_frame == from) { g_log_gs = lvl; LOG("gs: logging level %d from frame %llu\n", lvl, (unsigned long long)g_frame); }
    }
    if (g_log_gs && g_frame % 60 == 0) {
        static clock_t last; clock_t now = clock();
        double fps = last && now > last ? 60.0 * CLOCKS_PER_SEC / (double)(now - last) : 0;
        last = now;
        LOG("gs: frame %llu (%.1f fps host) gif %llu qw, %llu prims, %llu px uploaded\n", (unsigned long long)g_frame, fps,
            (unsigned long long)n_gif_qw, (unsigned long long)n_prims, (unsigned long long)n_xfer_px);
    }
}

void gs_init(int show_window, int scale) {
    R[PRMODECONT] = 1;
    if (g_log_gs || getenv("GS_FEATURES")) { fu_on = 1; atexit(fu_report); }
    if (!gsgl_init(show_window, scale)) { LOG("gs: OpenGL 3.3 initialisation failed\n"); exit(5); }
}
