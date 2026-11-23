/* VU0 in macro mode (COP2 instructions issued by the EE). Floats follow the
 * PS2 rules (no inf/nan, clamp to +-max), the same helpers as the FPU. */
#include "rt.h"

void vu_micro_run(int vu, u32 pc);   /* vu.c */

#define VF(n) ee.vf[n]
static inline float fx(float x) { return fpu_fix(x); }

static int warned[256];
static void unimpl(u32 w, int key) { if (!warned[key & 255]++) LOG("vu0: unimplemented macro %08x\n", w); }

static inline int dest_has(int dest, int c) { return (dest >> (3 - c)) & 1; }

static void wr(int fd, int dest, const float *v) {
    if (fd == 0) return;
    for (int c = 0; c < 4; c++) if (dest_has(dest, c)) ee.vf[fd].f[c] = fx(v[c]);
}
static void wracc(int dest, const float *v) {
    for (int c = 0; c < 4; c++) if (dest_has(dest, c)) ee.vacc.f[c] = fx(v[c]);
}

static void vf_get(int n, float *o) {
    if (n == 0) { o[0] = o[1] = o[2] = 0; o[3] = 1; return; }
    for (int c = 0; c < 4; c++) o[c] = fpu_fix(ee.vf[n].f[c]);
}

void vu0_macro(u32 w) {
    int dest = (w >> 21) & 15, ft = (w >> 16) & 31, fs = (w >> 11) & 31, fd = (w >> 6) & 31, fn = w & 63, bc = w & 3;
    float S[4], T[4], R[4], A[4];
    vf_get(fs, S); vf_get(ft, T);
    for (int c = 0; c < 4; c++) A[c] = ee.vacc.f[c];
    float q = ee.vq.f, I = ee.vi_reg.f;
    int c;
    if (fn < 0x3c) {
        float tb = T[bc];
        switch (fn) {
        case 0x00: case 0x01: case 0x02: case 0x03: for (c = 0; c < 4; c++) R[c] = S[c] + tb; wr(fd, dest, R); return;
        case 0x04: case 0x05: case 0x06: case 0x07: for (c = 0; c < 4; c++) R[c] = S[c] - tb; wr(fd, dest, R); return;
        case 0x08: case 0x09: case 0x0a: case 0x0b: for (c = 0; c < 4; c++) R[c] = A[c] + fx(S[c] * tb); wr(fd, dest, R); return;
        case 0x0c: case 0x0d: case 0x0e: case 0x0f: for (c = 0; c < 4; c++) R[c] = A[c] - fx(S[c] * tb); wr(fd, dest, R); return;
        case 0x10: case 0x11: case 0x12: case 0x13: for (c = 0; c < 4; c++) R[c] = S[c] > tb ? S[c] : tb; wr(fd, dest, R); return;
        case 0x14: case 0x15: case 0x16: case 0x17: for (c = 0; c < 4; c++) R[c] = S[c] < tb ? S[c] : tb; wr(fd, dest, R); return;
        case 0x18: case 0x19: case 0x1a: case 0x1b: for (c = 0; c < 4; c++) R[c] = S[c] * tb; wr(fd, dest, R); return;
        case 0x1c: for (c = 0; c < 4; c++) R[c] = S[c] * q; wr(fd, dest, R); return;
        case 0x1d: for (c = 0; c < 4; c++) R[c] = S[c] > I ? S[c] : I; wr(fd, dest, R); return;
        case 0x1e: for (c = 0; c < 4; c++) R[c] = S[c] * I; wr(fd, dest, R); return;
        case 0x1f: for (c = 0; c < 4; c++) R[c] = S[c] < I ? S[c] : I; wr(fd, dest, R); return;
        case 0x20: for (c = 0; c < 4; c++) R[c] = S[c] + q; wr(fd, dest, R); return;
        case 0x21: for (c = 0; c < 4; c++) R[c] = A[c] + fx(S[c] * q); wr(fd, dest, R); return;
        case 0x22: for (c = 0; c < 4; c++) R[c] = S[c] + I; wr(fd, dest, R); return;
        case 0x23: for (c = 0; c < 4; c++) R[c] = A[c] + fx(S[c] * I); wr(fd, dest, R); return;
        case 0x24: for (c = 0; c < 4; c++) R[c] = S[c] - q; wr(fd, dest, R); return;
        case 0x25: for (c = 0; c < 4; c++) R[c] = A[c] - fx(S[c] * q); wr(fd, dest, R); return;
        case 0x26: for (c = 0; c < 4; c++) R[c] = S[c] - I; wr(fd, dest, R); return;
        case 0x27: for (c = 0; c < 4; c++) R[c] = A[c] - fx(S[c] * I); wr(fd, dest, R); return;
        case 0x28: for (c = 0; c < 4; c++) R[c] = S[c] + T[c]; wr(fd, dest, R); return;
        case 0x29: for (c = 0; c < 4; c++) R[c] = A[c] + fx(S[c] * T[c]); wr(fd, dest, R); return;
        case 0x2a: for (c = 0; c < 4; c++) R[c] = S[c] * T[c]; wr(fd, dest, R); return;
        case 0x2b: for (c = 0; c < 4; c++) R[c] = S[c] > T[c] ? S[c] : T[c]; wr(fd, dest, R); return;
        case 0x2c: for (c = 0; c < 4; c++) R[c] = S[c] - T[c]; wr(fd, dest, R); return;
        case 0x2d: for (c = 0; c < 4; c++) R[c] = A[c] - fx(S[c] * T[c]); wr(fd, dest, R); return;
        case 0x2e:   /* VOPMSUB: fd.xyz = acc - fs.yzx * ft.zxy */
            R[0] = A[0] - fx(S[1] * T[2]); R[1] = A[1] - fx(S[2] * T[0]); R[2] = A[2] - fx(S[0] * T[1]); R[3] = 0;
            wr(fd, 14, R); return;
        case 0x2f: for (c = 0; c < 4; c++) R[c] = S[c] < T[c] ? S[c] : T[c]; wr(fd, dest, R); return;
        case 0x30: if (fd) ee.vi[fd] = (u16)(ee.vi[fs] + ee.vi[ft]); return;   /* id=fd, is=fs, it=ft */
        case 0x31: if (fd) ee.vi[fd] = (u16)(ee.vi[fs] - ee.vi[ft]); return;
        case 0x32: { int imm = (w >> 6) & 31; if (imm & 16) imm -= 32; if (ft) ee.vi[ft] = (u16)(ee.vi[fs] + imm); return; }
        case 0x34: if (fd) ee.vi[fd] = ee.vi[fs] & ee.vi[ft]; return;
        case 0x35: if (fd) ee.vi[fd] = ee.vi[fs] | ee.vi[ft]; return;
        case 0x38: vu_micro_run(0, ((w >> 6) & 0x7fff) * 8); return;                    /* VCALLMS */
        case 0x39: vu_micro_run(0, (ee.vi[27] & 0xffff) * 8); return;                    /* VCALLMSR */
        }
        unimpl(w, fn); return;
    }
    int op = ((w >> 4) & 0x7c) | bc;
    float tb = T[bc];
    switch (op) {
    case 0x00: case 0x01: case 0x02: case 0x03: for (c = 0; c < 4; c++) R[c] = S[c] + tb; wracc(dest, R); return;
    case 0x04: case 0x05: case 0x06: case 0x07: for (c = 0; c < 4; c++) R[c] = S[c] - tb; wracc(dest, R); return;
    case 0x08: case 0x09: case 0x0a: case 0x0b: for (c = 0; c < 4; c++) R[c] = A[c] + fx(S[c] * tb); wracc(dest, R); return;
    case 0x0c: case 0x0d: case 0x0e: case 0x0f: for (c = 0; c < 4; c++) R[c] = A[c] - fx(S[c] * tb); wracc(dest, R); return;
    case 0x10: case 0x11: case 0x12: case 0x13: {   /* VITOF0/4/12/15 */
        static const float sc[4] = { 1.0f, 1.0f / 16, 1.0f / 4096, 1.0f / 32768 };
        if (ft) for (c = 0; c < 4; c++) if (dest_has(dest, c)) ee.vf[ft].f[c] = (float)ee.vf[fs].s[c] * sc[op & 3];
        return; }
    case 0x14: case 0x15: case 0x16: case 0x17: {   /* VFTOI0/4/12/15 */
        static const float sc[4] = { 1.0f, 16.0f, 4096.0f, 32768.0f };
        if (ft) for (c = 0; c < 4; c++) if (dest_has(dest, c)) {
            float v = S[c] * sc[op & 3];
            ee.vf[ft].s[c] = v >= 2147483647.0f ? 0x7fffffff : v <= -2147483648.0f ? (s32)0x80000000 : (s32)v;
        }
        return; }
    case 0x18: case 0x19: case 0x1a: case 0x1b: for (c = 0; c < 4; c++) R[c] = S[c] * tb; wracc(dest, R); return;
    case 0x1c: for (c = 0; c < 4; c++) R[c] = S[c] * q; wracc(dest, R); return;
    case 0x1d: if (ft) for (c = 0; c < 4; c++) if (dest_has(dest, c)) ee.vf[ft].f[c] = fabsf(S[c]); return;
    case 0x1e: for (c = 0; c < 4; c++) R[c] = S[c] * I; wracc(dest, R); return;
    case 0x1f: {   /* VCLIPw.xyz fs, ft */
        float wv = fabsf(T[3]); u32 f = 0;
        if (S[0] > wv) f |= 1; if (S[0] < -wv) f |= 2;
        if (S[1] > wv) f |= 4; if (S[1] < -wv) f |= 8;
        if (S[2] > wv) f |= 16; if (S[2] < -wv) f |= 32;
        ee.vi[18] = ((ee.vi[18] << 6) | f) & 0xffffff;
        return; }
    case 0x20: for (c = 0; c < 4; c++) R[c] = S[c] + q; wracc(dest, R); return;
    case 0x21: for (c = 0; c < 4; c++) R[c] = A[c] + fx(S[c] * q); wracc(dest, R); return;
    case 0x22: for (c = 0; c < 4; c++) R[c] = S[c] + I; wracc(dest, R); return;
    case 0x23: for (c = 0; c < 4; c++) R[c] = A[c] + fx(S[c] * I); wracc(dest, R); return;
    case 0x24: for (c = 0; c < 4; c++) R[c] = S[c] - q; wracc(dest, R); return;
    case 0x25: for (c = 0; c < 4; c++) R[c] = A[c] - fx(S[c] * q); wracc(dest, R); return;
    case 0x26: for (c = 0; c < 4; c++) R[c] = S[c] - I; wracc(dest, R); return;
    case 0x27: for (c = 0; c < 4; c++) R[c] = A[c] - fx(S[c] * I); wracc(dest, R); return;
    case 0x28: for (c = 0; c < 4; c++) R[c] = S[c] + T[c]; wracc(dest, R); return;
    case 0x29: for (c = 0; c < 4; c++) R[c] = A[c] + fx(S[c] * T[c]); wracc(dest, R); return;
    case 0x2a: for (c = 0; c < 4; c++) R[c] = S[c] * T[c]; wracc(dest, R); return;
    case 0x2c: for (c = 0; c < 4; c++) R[c] = S[c] - T[c]; wracc(dest, R); return;
    case 0x2d: for (c = 0; c < 4; c++) R[c] = A[c] - fx(S[c] * T[c]); wracc(dest, R); return;
    case 0x2e:   /* VOPMULA: acc.xyz = fs.yzx * ft.zxy */
        R[0] = S[1] * T[2]; R[1] = S[2] * T[0]; R[2] = S[0] * T[1]; R[3] = 0; wracc(14, R); return;
    case 0x2f: return;   /* VNOP */
    case 0x30: if (ft) for (c = 0; c < 4; c++) if (dest_has(dest, c)) ee.vf[ft].u[c] = ee.vf[fs].u[c]; return;
    case 0x31: if (ft) { vf128 s = ee.vf[fs]; if (fs == 0) { s.f[0] = s.f[1] = s.f[2] = 0; s.f[3] = 1; }
        u32 r4[4] = { s.u[1], s.u[2], s.u[3], s.u[0] };
        for (c = 0; c < 4; c++) if (dest_has(dest, c)) ee.vf[ft].u[c] = r4[c]; } return;
    case 0x34: { u32 a = (ee.vi[fs] & 0xff) * 16; if (ft) for (c = 0; c < 4; c++) if (dest_has(dest, c)) memcpy(&ee.vf[ft].u[c], vu0_mem + a + 4 * c, 4);
        if (fs) ee.vi[fs] = (u16)(ee.vi[fs] + 1); return; }
    case 0x35: { u32 a = (ee.vi[ft] & 0xff) * 16; for (c = 0; c < 4; c++) if (dest_has(dest, c)) memcpy(vu0_mem + a + 4 * c, &ee.vf[fs].u[c], 4);
        if (ft) ee.vi[ft] = (u16)(ee.vi[ft] + 1); return; }
    case 0x36: { if (fs) ee.vi[fs] = (u16)(ee.vi[fs] - 1); u32 a = (ee.vi[fs] & 0xff) * 16;
        if (ft) for (c = 0; c < 4; c++) if (dest_has(dest, c)) memcpy(&ee.vf[ft].u[c], vu0_mem + a + 4 * c, 4); return; }
    case 0x37: { if (ft) ee.vi[ft] = (u16)(ee.vi[ft] - 1); u32 a = (ee.vi[ft] & 0xff) * 16;
        for (c = 0; c < 4; c++) if (dest_has(dest, c)) memcpy(vu0_mem + a + 4 * c, &ee.vf[fs].u[c], 4); return; }
    case 0x38: { int fsf = (w >> 21) & 3, ftf = (w >> 23) & 3; float d = T[ftf];
        ee.vq.f = d == 0.0f ? (((ee.vf[fs].u[fsf] ^ ee.vf[ft].u[ftf]) & 0x80000000u) ? -3.402823466e38f : 3.402823466e38f) : fx(S[fsf] / d); return; }
    case 0x39: { int ftf = (w >> 23) & 3; ee.vq.f = fx(sqrtf(fabsf(T[ftf]))); return; }
    case 0x3a: { int fsf = (w >> 21) & 3, ftf = (w >> 23) & 3; float d = sqrtf(fabsf(T[ftf]));
        ee.vq.f = d == 0.0f ? 3.402823466e38f : fx(S[fsf] / d); return; }
    case 0x3b: return;   /* VWAITQ */
    case 0x3c: { int fsf = (w >> 21) & 3; if (ft) ee.vi[ft] = (u16)ee.vf[fs].u[fsf]; return; }
    case 0x3d: if (ft) for (c = 0; c < 4; c++) if (dest_has(dest, c)) ee.vf[ft].s[c] = (s16)ee.vi[fs]; return;
    case 0x3e: { u32 a = (ee.vi[fs] & 0xff) * 16; for (c = 0; c < 4; c++) if (dest_has(dest, c)) { u32 v; memcpy(&v, vu0_mem + a + 4 * c, 4); if (ft) ee.vi[ft] = (u16)v; } return; }
    case 0x3f: { u32 a = (ee.vi[fs] & 0xff) * 16; for (c = 0; c < 4; c++) if (dest_has(dest, c)) { u32 v = ee.vi[ft]; memcpy(vu0_mem + a + 4 * c, &v, 4); } return; }
    case 0x40: case 0x41: { u32 r = ee.vi[20];   /* VRNEXT / VRGET */
        if (op == 0x40) { int x = ((r >> 4) ^ (r >> 22)) & 1; r = ((r << 1) | x) & 0x7fffff; ee.vi[20] = r; }
        if (ft) for (c = 0; c < 4; c++) if (dest_has(dest, c)) ee.vf[ft].u[c] = 0x3f800000u | (r & 0x7fffff);
        return; }
    case 0x42: { int fsf = (w >> 21) & 3; ee.vi[20] = 0x3f800000u | (ee.vf[fs].u[fsf] & 0x7fffff); return; }
    case 0x43: { int fsf = (w >> 21) & 3; ee.vi[20] = (ee.vi[20] ^ ee.vf[fs].u[fsf]) & 0x7fffff; return; }
    }
    unimpl(w, 0x80 | op);
}

void vu0_qmfc2(int rt, int vf) { if (rt) { if (vf == 0) { ee.r[rt].d[0] = 0; ee.r[rt].d[1] = 0x3f80000000000000ull; } else memcpy(&ee.r[rt], &ee.vf[vf], 16); } }
void vu0_qmtc2(int rt, int vf) { if (vf) { if (rt) memcpy(&ee.vf[vf], &ee.r[rt], 16); else memset(&ee.vf[vf], 0, 16); } }
void vu0_lqc2(u32 a, int vf) { r128 t; a &= ~15u; u8 *p = ee_ptr(a); if (p) memcpy(&t, p, 16); else io_read128(a, &t); if (vf) memcpy(&ee.vf[vf], &t, 16); }
void vu0_sqc2(u32 a, int vf) {
    r128 t; if (vf) memcpy(&t, &ee.vf[vf], 16); else { t.d[0] = 0; t.d[1] = 0x3f80000000000000ull; }
    a &= ~15u; u8 *p = ee_ptr(a); if (p) memcpy(p, &t, 16); else io_write128(a, &t);
}

void vu1_start(u32 pc);
void vu0_cfc2(int rt, int r) {
    u32 v;
    if (r < 16) v = ee.vi[r] & 0xffff;
    else if (r == 21) v = ee.vi_reg.u;
    else if (r == 22) v = ee.vq.u;
    else if (r == 29) v = 0;            /* VPU_STAT: both VUs idle */
    else v = ee.vi[r];
    if (rt) ee.r[rt].sd[0] = (s32)v;
}
void vu0_ctc2(int rt, int r) {
    u32 v = rt ? (u32)ee.r[rt].d[0] : 0;
    if (r == 0) return;
    if (r < 16) { ee.vi[r] = v & 0xffff; return; }
    if (r == 21) { ee.vi_reg.u = v; return; }
    if (r == 22) { ee.vq.u = v; return; }
    if (r == 28) { return; }            /* FBRST */
    if (r == 31) { ee.vi[31] = v; if (g_gs_mt) gst_vu1_start((v & 0x7ff) * 8); else vu1_start((v & 0x7ff) * 8); return; }
    ee.vi[r] = v;
}
int vu0_bc2(void) { return 0; }
