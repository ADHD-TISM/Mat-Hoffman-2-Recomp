/* VU micro mode interpreter for VU0 and VU1.
 *
 * A microprogram runs to its E bit when MSCAL/MSCNT (VIF) or VCALLMS (EE)
 * starts it. Each 64-bit pair runs upper and lower together: the upper
 * result is computed first and committed after the lower op, so neither sees
 * the other's write and the upper wins if both write one register. Branches
 * have one delay slot, the E bit one more pair. FMAC hazards stall on
 * hardware, so results are visible at once here; Q and P do not stall, so
 * DIV/SQRT/RSQRT and the EFU ops land after their latency (in pairs) unless
 * WAITQ/WAITP. MAC/status/clip flags have no delay. XGKICK sends the GIF
 * packet at once on path 1. VU0 uses the EE's COP2 registers. */
#include "rt.h"
#include <math.h>
#include <emmintrin.h>
#include <stdlib.h>

void gif_transfer(const u8 *qw, u32 nqw, int path);
u32 vif1_top_value(void), vif1_itop_value(void), vif0_itop_value(void);

typedef struct {
    vf128 *vf; u32 *vi; vf128 *acc; fr *q, *i;
    u8 *mem; u32 qmask;            /* data memory and qw mask */
    u8 *code; u32 cmask;           /* micro memory and byte mask */
    u32 p; u32 status, mac;        /* status/mac also mirrored to vi[16]/vi[17] */
    u32 q_pend, p_pend; int q_wait, p_wait;
    u32 tpc;
    u64 runs, pairs;
    int rf1;                       /* 1: VU1, the register files are the fixed arrays below */
    /* Clipping flag pipeline: a CLIP result reaches the flag register 4 instructions
       later. Mat Hoffman 2's clipper does three CLIPs and then three FCGETs on
       successive instructions to read the three intermediate states. */
    u32 clip_new, cp_val[4]; int cp_cnt[4], cp_n;
} vu_t;

static vf128 vu1_vf[32], vu1_acc;
static u32 vu1_vi[32];
static fr vu1_q, vu1_i;
static vu_t VU[2];
static int vu_ready;
static int vu_no_kick;          /* VU_CHECK: the reference run must not send GIF data */
/* Register files: VU1 always uses the fixed arrays below. The recompiled VU1
 * chunks run with a local vu_t whose rf1 is the constant 1 (VU_REC_ENTER), so
 * these fold to the globals and the compiler knows no store to them changes
 * any other (with v->vf etc. through pointers every store forced the pointers
 * to be reloaded). */
#define VUSEL(v, one, f) ((v)->rf1 ? (one) : (v)->f)
#define VFP(v)   VUSEL(v, vu1_vf, vf)
#define VIP(v)   VUSEL(v, vu1_vi, vi)
#define ACCP(v)  VUSEL(v, &vu1_acc, acc)
#define QP(v)    VUSEL(v, &vu1_q, q)
#define IP(v)    VUSEL(v, &vu1_i, i)
#define MEMP(v)  VUSEL(v, vu1_mem, mem)
#define QMASK(v) ((v)->rf1 ? 0x3ffu : (v)->qmask)

static void vu_setup(void) {
    if (vu_ready) return;
    vu_ready = 1;
    VU[0].vf = ee.vf; VU[0].vi = ee.vi; VU[0].acc = &ee.vacc; VU[0].q = &ee.vq; VU[0].i = &ee.vi_reg;
    VU[0].mem = vu0_mem; VU[0].qmask = 0xff; VU[0].code = vu0_code; VU[0].cmask = 0xfff;
    VU[1].vf = vu1_vf; VU[1].vi = vu1_vi; VU[1].acc = &vu1_acc; VU[1].q = &vu1_q; VU[1].i = &vu1_i;
    VU[1].mem = vu1_mem; VU[1].qmask = 0x3ff; VU[1].code = vu1_code; VU[1].cmask = 0x3fff; VU[1].rf1 = 1;
    vu1_vf[0].f[3] = 1.0f;
}

#define AI static inline __attribute__((always_inline))
static inline float fx(float x) { return fpu_fix(x); }
static inline int has(int dest, int c) { return (dest >> (3 - c)) & 1; }
AI u32 *qptr(vu_t *v, u32 a) { return (u32 *)(MEMP(v) + ((a & QMASK(v)) << 4)); }

AI void vfget(vu_t *v, int n, float *o) {
    if (n == 0) { o[0] = o[1] = o[2] = 0; o[3] = 1; return; }
    for (int c = 0; c < 4; c++) o[c] = fx(VFP(v)[n].f[c]);
}

/* The upper (FMAC) ops work on all 4 lanes at once in SSE registers. The
 * clamp fx4 is fx on every lane without branches. Each result is the same
 * bit pattern the per-lane code gave: the same IEEE single ops in the same
 * order, MAXPS/MINPS pick exactly S > T ? S : T and S < T ? S : T, and no
 * operand can be NaN after fx. */
typedef __m128 f4;
#define I4(x) _mm_set1_epi32((int)(x))
AI f4 fx4(f4 x) {
    __m128i b = _mm_castps_si128(x), e = _mm_and_si128(b, I4(0x7f800000u)), sg = _mm_and_si128(b, I4(0x80000000u));
    __m128i inf = _mm_cmpeq_epi32(e, I4(0x7f800000u)), m = _mm_or_si128(inf, _mm_cmpeq_epi32(e, _mm_setzero_si128()));
    __m128i r = _mm_or_si128(_mm_andnot_si128(m, b), _mm_and_si128(m, sg));
    return _mm_castsi128_ps(_mm_or_si128(r, _mm_and_si128(inf, I4(0x7f7fffffu))));
}
/* cl: the recompiler proved every lane of the register holds a clamped value */
AI f4 vfget4(vu_t *v, int n, int cl) {
    if (n == 0) return _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
    f4 x = _mm_loadu_ps(VFP(v)[n].f);
    return cl ? x : fx4(x);
}
AI f4 bcast(f4 t, int bc) {
    switch (bc) {
    case 0: return _mm_shuffle_ps(t, t, 0x00);
    case 1: return _mm_shuffle_ps(t, t, 0x55);
    case 2: return _mm_shuffle_ps(t, t, 0xaa);
    default: return _mm_shuffle_ps(t, t, 0xff);
    }
}
/* lane c is written when has(dest, c): dest bit 3 is x (lane 0) */
AI f4 dmask(int dest) {
    return _mm_castsi128_ps(_mm_set_epi32(-(dest & 1), -((dest >> 1) & 1), -((dest >> 2) & 1), -((dest >> 3) & 1)));
}
AI void st4(float *dst, f4 r, int dest) {
    if (dest == 15) { _mm_storeu_ps(dst, r); return; }
    if (dest == 0) return;
    f4 m = dmask(dest);
    _mm_storeu_ps(dst, _mm_or_ps(_mm_and_ps(m, r), _mm_andnot_ps(m, _mm_loadu_ps(dst))));
}

AI void set_flags(vu_t *v, int dest, f4 r) {
    /* movemask bit c is lane c; MAC bit 3 - c is lane c */
    static const u8 rev[16] = { 0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15 };
    int z = _mm_movemask_ps(_mm_cmpeq_ps(r, _mm_setzero_ps())), sgn = _mm_movemask_ps(r);
    u32 mac = (rev[z] & dest) | ((u32)(rev[sgn] & dest) << 4);
    v->mac = mac;
    u32 st = v->status & 0xfc0;
    if (mac & 0x0f) st |= 1 | 0x40;
    if (mac & 0xf0) st |= 2 | 0x80;
    v->status = st | (v->status & 0x30);
    VIP(v)[17] = v->mac; VIP(v)[16] = v->status;
}

/* ---- upper ------------------------------------------------------------------- */
typedef struct { int kind; int reg; int dest; int flags; u32 clip; f4 r; } upres;   /* kind 0 none, 1 vf float, 2 acc, 3 vf raw bits, 4 clip */

/* cl bit 0: fs is clamped already, bit 1: ft (tools/recomp_vu.py clamp_analysis; 0 = clamp both) */
AI void upper(vu_t *v, u32 w, upres *o, int cl) {
    int dest = (w >> 21) & 15, ft = (w >> 16) & 31, fs = (w >> 11) & 31, fd = (w >> 6) & 31, fn = w & 63, bc = w & 3;
    f4 S = vfget4(v, fs, cl & 1), T = vfget4(v, ft, cl & 2), A = _mm_loadu_ps(ACCP(v)->f), R;
    f4 q = _mm_set1_ps(QP(v)->f), I = _mm_set1_ps(IP(v)->f);
    int flags = 1;
    o->kind = 0; o->dest = dest; o->reg = fd; o->flags = 0;
#define MADD(x, y) _mm_add_ps(A, fx4(_mm_mul_ps(x, y)))
#define MSUB(x, y) _mm_sub_ps(A, fx4(_mm_mul_ps(x, y)))
#define YZX(x) _mm_shuffle_ps(x, x, _MM_SHUFFLE(3, 0, 2, 1))
#define ZXY(x) _mm_shuffle_ps(x, x, _MM_SHUFFLE(3, 1, 0, 2))
    if (fn < 0x3c) {
        f4 tb = bcast(T, bc);
        o->kind = 1;
        switch (fn) {
        case 0x00: case 0x01: case 0x02: case 0x03: R = _mm_add_ps(S, tb); break;
        case 0x04: case 0x05: case 0x06: case 0x07: R = _mm_sub_ps(S, tb); break;
        case 0x08: case 0x09: case 0x0a: case 0x0b: R = MADD(S, tb); break;
        case 0x0c: case 0x0d: case 0x0e: case 0x0f: R = MSUB(S, tb); break;
        case 0x10: case 0x11: case 0x12: case 0x13: R = _mm_max_ps(S, tb); flags = 0; break;
        case 0x14: case 0x15: case 0x16: case 0x17: R = _mm_min_ps(S, tb); flags = 0; break;
        case 0x18: case 0x19: case 0x1a: case 0x1b: R = _mm_mul_ps(S, tb); break;
        case 0x1c: R = _mm_mul_ps(S, q); break;
        case 0x1d: R = _mm_max_ps(S, I); flags = 0; break;
        case 0x1e: R = _mm_mul_ps(S, I); break;
        case 0x1f: R = _mm_min_ps(S, I); flags = 0; break;
        case 0x20: R = _mm_add_ps(S, q); break;
        case 0x21: R = MADD(S, q); break;
        case 0x22: R = _mm_add_ps(S, I); break;
        case 0x23: R = MADD(S, I); break;
        case 0x24: R = _mm_sub_ps(S, q); break;
        case 0x25: R = MSUB(S, q); break;
        case 0x26: R = _mm_sub_ps(S, I); break;
        case 0x27: R = MSUB(S, I); break;
        case 0x28: R = _mm_add_ps(S, T); break;
        case 0x29: R = MADD(S, T); break;
        case 0x2a: R = _mm_mul_ps(S, T); break;
        case 0x2b: R = _mm_max_ps(S, T); flags = 0; break;
        case 0x2c: R = _mm_sub_ps(S, T); break;
        case 0x2d: R = MSUB(S, T); break;
        case 0x2e: R = MSUB(YZX(S), ZXY(T)); o->dest = dest = 14; break;   /* OPMSUB: w is not written */
        case 0x2f: R = _mm_min_ps(S, T); flags = 0; break;
        default: o->kind = 0; { static int once; if (!once++) LOG("vu: unknown upper %08x\n", w); } return;
        }
        o->r = fx4(R);
        o->flags = flags;
        return;
    }
    int op = ((w >> 4) & 0x7c) | bc;
    f4 tb = bcast(T, bc);
    o->kind = 2;
    switch (op) {
    case 0x00: case 0x01: case 0x02: case 0x03: R = _mm_add_ps(S, tb); break;
    case 0x04: case 0x05: case 0x06: case 0x07: R = _mm_sub_ps(S, tb); break;
    case 0x08: case 0x09: case 0x0a: case 0x0b: R = MADD(S, tb); break;
    case 0x0c: case 0x0d: case 0x0e: case 0x0f: R = MSUB(S, tb); break;
    case 0x10: case 0x11: case 0x12: case 0x13: {   /* ITOF0/4/12/15: from the raw register bits */
        static const float sc[4] = { 1.0f, 1.0f / 16, 1.0f / 4096, 1.0f / 32768 };
        o->kind = 3; o->reg = ft;
        if (fs == 0) { o->r = _mm_setzero_ps(); return; }
        o->r = _mm_castsi128_ps(_mm_castps_si128(_mm_mul_ps(_mm_cvtepi32_ps(_mm_loadu_si128((const __m128i *)VFP(v)[fs].u)), _mm_set1_ps(sc[op & 3]))));
        return; }
    case 0x14: case 0x15: case 0x16: case 0x17: {   /* FTOI0/4/12/15: x >= 2^31 saturates to 0x7fffffff, cvtt gives 0x80000000 below -2^31 */
        static const float sc[4] = { 1.0f, 16.0f, 4096.0f, 32768.0f };
        o->kind = 3; o->reg = ft;
        f4 x = _mm_mul_ps(S, _mm_set1_ps(sc[op & 3]));
        __m128i r = _mm_cvttps_epi32(x), big = _mm_castps_si128(_mm_cmpge_ps(x, _mm_set1_ps(2147483648.0f)));
        o->r = _mm_castsi128_ps(_mm_or_si128(_mm_andnot_si128(big, r), _mm_and_si128(big, I4(0x7fffffffu))));
        return; }
    case 0x18: case 0x19: case 0x1a: case 0x1b: R = _mm_mul_ps(S, tb); break;
    case 0x1c: R = _mm_mul_ps(S, q); break;
    case 0x1d: o->kind = 1; o->reg = ft; o->r = _mm_and_ps(S, _mm_castsi128_ps(I4(0x7fffffffu))); return;   /* ABS */
    case 0x1f: {   /* CLIPw.xyz fs, ft */
        float s[4]; _mm_storeu_ps(s, S);
        float wv = fabsf(_mm_cvtss_f32(_mm_shuffle_ps(T, T, 0xff))); u32 f = 0;
        if (s[0] > wv) f |= 1; if (s[0] < -wv) f |= 2;
        if (s[1] > wv) f |= 4; if (s[1] < -wv) f |= 8;
        if (s[2] > wv) f |= 16; if (s[2] < -wv) f |= 32;
        o->kind = 4; o->clip = f; return; }
    case 0x1e: R = _mm_mul_ps(S, I); break;
    case 0x20: R = _mm_add_ps(S, q); break;
    case 0x21: R = MADD(S, q); break;
    case 0x22: R = _mm_add_ps(S, I); break;
    case 0x23: R = MADD(S, I); break;
    case 0x24: R = _mm_sub_ps(S, q); break;
    case 0x25: R = MSUB(S, q); break;
    case 0x26: R = _mm_sub_ps(S, I); break;
    case 0x27: R = MSUB(S, I); break;
    case 0x28: R = _mm_add_ps(S, T); break;
    case 0x29: R = MADD(S, T); break;
    case 0x2a: R = _mm_mul_ps(S, T); break;
    case 0x2c: R = _mm_sub_ps(S, T); break;
    case 0x2d: R = MSUB(S, T); break;
    case 0x2e: R = _mm_mul_ps(YZX(S), ZXY(T)); o->dest = dest = 14; break;   /* OPMULA */
    case 0x2f: o->kind = 0; return;   /* NOP */
    default: o->kind = 0; { static int once; if (!once++) LOG("vu: unknown upper %08x\n", w); } return;
    }
#undef MADD
#undef MSUB
#undef YZX
#undef ZXY
    o->r = fx4(R);
    o->flags = 1;
}

/* fl = 0: the recompiler proved nothing reads this MAC/status update (tools/recomp_vu.py flag_liveness) */
AI void upper_commit_fl(vu_t *v, const upres *o, int fl) {
    switch (o->kind) {
    case 1: case 3: if (o->reg) st4(VFP(v)[o->reg].f, o->r, o->dest); break;
    case 2: st4(ACCP(v)->f, o->r, o->dest); break;
    case 4:
        if (!v->cp_n) v->clip_new = VIP(v)[18];
        v->clip_new = ((v->clip_new << 6) | o->clip) & 0xffffff;
        if (v->cp_n == 4) { VIP(v)[18] = v->cp_val[0]; for (int k = 0; k < 3; k++) { v->cp_val[k] = v->cp_val[k + 1]; v->cp_cnt[k] = v->cp_cnt[k + 1]; } v->cp_n = 3; }
        v->cp_val[v->cp_n] = v->clip_new; v->cp_cnt[v->cp_n] = 4; v->cp_n++;   /* visible to the lower op 4 pairs on */
        break;
    }
    if (o->flags && fl) set_flags(v, o->dest, o->r);
}
AI void upper_commit(vu_t *v, const upres *o) { upper_commit_fl(v, o, 1); }
/* once per instruction pair, after the commit: move the clip pipeline on */
AI void clip_tick(vu_t *v) {
    if (!v->cp_n) return;
    for (int k = 0; k < v->cp_n; k++) v->cp_cnt[k]--;
    while (v->cp_n && v->cp_cnt[0] <= 0) {
        VIP(v)[18] = v->cp_val[0];
        for (int k = 0; k + 1 < v->cp_n; k++) { v->cp_val[k] = v->cp_val[k + 1]; v->cp_cnt[k] = v->cp_cnt[k + 1]; }
        v->cp_n--;
    }
}
AI void clip_flush(vu_t *v) { if (v->cp_n) { VIP(v)[18] = v->clip_new; v->cp_n = 0; } }

/* ---- XGKICK: walk the GIF packet in VU1 memory up to EOP --------------------- */
static void vu_dump(void) {
    /* called on every XGKICK: read the environment once (getenv was 40% of level time) */
    static int done, init; static const char *d; static u64 at;
    if (done) return;
    if (!init) { init = 1; d = getenv("VU_DUMP"); const char *a = getenv("VU_DUMP_AT"); at = a ? strtoull(a, 0, 10) : 0; if (!d) done = 1; }
    if (done || g_frame < at) return;
    done = 1;
    char name[512]; FILE *f;
    snprintf(name, sizeof name, "%s/vu1_code.bin", d); if ((f = fopen(name, "wb"))) { fwrite(vu1_code, 1, sizeof vu1_code, f); fclose(f); }
    snprintf(name, sizeof name, "%s/vu1_mem.bin", d); if ((f = fopen(name, "wb"))) { fwrite(vu1_mem, 1, sizeof vu1_mem, f); fclose(f); }
    LOG("vu: dumped VU1 code and memory to %s\n", d);
}

static void xgkick(vu_t *v, u32 addr) {
    vu_dump();
    for (int guard = 0; guard < 1024; guard++) {
        u64 lo; memcpy(&lo, qptr(v, addr), 8);
        u32 nloop = lo & 0x7fff, eop = (lo >> 15) & 1, flg = (lo >> 58) & 3, nreg = (lo >> 60) & 15;
        if (!nreg) nreg = 16;
        u32 n = flg == 0 ? nloop * nreg : flg == 1 ? (nloop * nreg + 1) / 2 : nloop;
        for (u32 k = 0; k <= n; ) {   /* in runs that do not wrap at the end of VU memory */
            u32 a = (addr + k) & QMASK(v), run = QMASK(v) + 1 - a;
            if (run > n + 1 - k) run = n + 1 - k;
            gif_transfer((const u8 *)qptr(v, a), run, 1);
            k += run;
        }
        addr += n + 1;
        if (eop) return;
    }
}

/* ---- EFU ------------------------------------------------------------------------- */
AI void efu(vu_t *v, float r, int lat) { v->p_pend = ((fr){ .f = fx(r) }).u; v->p_wait = lat; }

/* ---- lower ------------------------------------------------------------------------ */
static inline s32 imm11(u32 w) { s32 x = w & 0x7ff; return x & 0x400 ? x - 0x800 : x; }
AI u16 VI(vu_t *v, int n) { return n ? (u16)VIP(v)[n] : 0; }
AI void setvi(vu_t *v, int n, u32 x) { if (n && n < 16) VIP(v)[n] = x & 0xffff; }

AI void q_set(vu_t *v, float r, int lat) {
    if (v->q_wait) { QP(v)->u = v->q_pend; }
    v->q_pend = ((fr){ .f = fx(r) }).u; v->q_wait = lat;
}

/* returns 1 with *target set when the op branches */
AI int lower(vu_t *v, u32 w, u32 pc, u32 *target, int vu) {
    int op = w >> 25, dest = (w >> 21) & 15, it = (w >> 16) & 31, is = (w >> 11) & 31, id = (w >> 6) & 31;
    float S[4];
    switch (op) {
    case 0x00: { u32 *m = qptr(v, VI(v, is) + imm11(w)); if (it) for (int c = 0; c < 4; c++) if (has(dest, c)) VFP(v)[it].u[c] = m[c]; return 0; }   /* LQ */
    case 0x01: { u32 *m = qptr(v, VI(v, it) + imm11(w)); vf128 s = VFP(v)[is]; if (!is) { s.u[0] = s.u[1] = s.u[2] = 0; s.f[3] = 1; }
        for (int c = 0; c < 4; c++) if (has(dest, c)) m[c] = s.u[c]; return 0; }                                                               /* SQ */
    case 0x04: { u32 *m = qptr(v, VI(v, is) + imm11(w)); for (int c = 0; c < 4; c++) if (has(dest, c)) setvi(v, it, m[c]); return 0; }        /* ILW */
    case 0x05: { u32 *m = qptr(v, VI(v, is) + imm11(w)); for (int c = 0; c < 4; c++) if (has(dest, c)) m[c] = VI(v, it); return 0; }          /* ISW */
    case 0x08: setvi(v, it, VI(v, is) + (((w >> 10) & 0x7800) | (w & 0x7ff))); return 0;                                                        /* IADDIU */
    case 0x09: setvi(v, it, VI(v, is) - (((w >> 10) & 0x7800) | (w & 0x7ff))); return 0;                                                        /* ISUBIU */
    case 0x10: VIP(v)[1] = (VIP(v)[18] & 0xffffff) == (w & 0xffffff); return 0;                                                                  /* FCEQ */
    case 0x11: VIP(v)[18] = w & 0xffffff; v->cp_n = 0; return 0;                                                                                             /* FCSET */
    case 0x12: VIP(v)[1] = (VIP(v)[18] & w & 0xffffff) != 0; return 0;                                                                           /* FCAND */
    case 0x13: VIP(v)[1] = ((VIP(v)[18] | w) & 0xffffff) == 0xffffff; return 0;                                                                  /* FCOR */
    case 0x14: setvi(v, it, (v->status & 0xfff) == (((w >> 10) & 0x800) | (w & 0x7ff))); return 0;                                            /* FSEQ */
    case 0x15: v->status = (v->status & 0x3f) | ((((w >> 10) & 0x800) | (w & 0x7ff)) & 0xfc0); VIP(v)[16] = v->status; return 0;              /* FSSET */
    case 0x16: setvi(v, it, v->status & (((w >> 10) & 0x800) | (w & 0x7ff))); return 0;                                                       /* FSAND */
    case 0x17: setvi(v, it, v->status | (((w >> 10) & 0x800) | (w & 0x7ff))); return 0;                                                       /* FSOR */
    case 0x18: setvi(v, it, (v->mac & 0xffff) == VI(v, is)); return 0;                                                                         /* FMEQ */
    case 0x1a: setvi(v, it, v->mac & VI(v, is)); return 0;                                                                                     /* FMAND */
    case 0x1b: setvi(v, it, v->mac | VI(v, is)); return 0;                                                                                     /* FMOR */
    case 0x1c: setvi(v, it, VIP(v)[18] & 0xfff); return 0;                                                                                      /* FCGET */
    case 0x20: *target = pc + 8 + imm11(w) * 8; return 1;                                                                                     /* B */
    case 0x21: setvi(v, it, (pc + 16) / 8); *target = pc + 8 + imm11(w) * 8; return 1;                                                         /* BAL */
    case 0x24: *target = VI(v, is) * 8; return 1;                                                                                              /* JR */
    case 0x25: { u32 t = VI(v, is) * 8; setvi(v, it, (pc + 16) / 8); *target = t; return 1; }                                                  /* JALR */
    case 0x28: if (VI(v, it) == VI(v, is)) { *target = pc + 8 + imm11(w) * 8; return 1; } return 0;                                           /* IBEQ */
    case 0x29: if (VI(v, it) != VI(v, is)) { *target = pc + 8 + imm11(w) * 8; return 1; } return 0;                                           /* IBNE */
    case 0x2c: if ((s16)VI(v, is) < 0) { *target = pc + 8 + imm11(w) * 8; return 1; } return 0;                                               /* IBLTZ */
    case 0x2d: if ((s16)VI(v, is) > 0) { *target = pc + 8 + imm11(w) * 8; return 1; } return 0;                                               /* IBGTZ */
    case 0x2e: if ((s16)VI(v, is) <= 0) { *target = pc + 8 + imm11(w) * 8; return 1; } return 0;                                              /* IBLEZ */
    case 0x2f: if ((s16)VI(v, is) >= 0) { *target = pc + 8 + imm11(w) * 8; return 1; } return 0;                                              /* IBGEZ */
    case 0x40: break;
    default: { static int once; if (!once++) LOG("vu%d: unknown lower %08x at %04x\n", vu, w, pc); } return 0;
    }
    int fn = w & 63;
    switch (fn) {
    case 0x30: setvi(v, id, VI(v, is) + VI(v, it)); return 0;   /* IADD */
    case 0x31: setvi(v, id, VI(v, is) - VI(v, it)); return 0;   /* ISUB */
    case 0x32: { int imm = (w >> 6) & 31; if (imm & 16) imm -= 32; setvi(v, it, VI(v, is) + imm); return 0; }   /* IADDI */
    case 0x34: setvi(v, id, VI(v, is) & VI(v, it)); return 0;   /* IAND */
    case 0x35: setvi(v, id, VI(v, is) | VI(v, it)); return 0;   /* IOR */
    }
    if (fn < 0x3c) { static int once; if (!once++) LOG("vu%d: unknown lower %08x at %04x\n", vu, w, pc); return 0; }
    int x = ((w >> 4) & 0x7c) | (w & 3);
    int ft = it, fs = is, fsf = (w >> 21) & 3, ftf = (w >> 23) & 3;
    switch (x) {
    case 0x30: if (ft) { vf128 s = VFP(v)[fs]; if (!fs) { s.u[0] = s.u[1] = s.u[2] = 0; s.f[3] = 1; }
        for (int c = 0; c < 4; c++) if (has(dest, c)) VFP(v)[ft].u[c] = s.u[c]; } return 0;   /* MOVE */
    case 0x31: if (ft) { vf128 s = VFP(v)[fs]; if (!fs) { s.u[0] = s.u[1] = s.u[2] = 0; s.f[3] = 1; }
        u32 r[4] = { s.u[1], s.u[2], s.u[3], s.u[0] };
        for (int c = 0; c < 4; c++) if (has(dest, c)) VFP(v)[ft].u[c] = r[c]; } return 0;     /* MR32 */
    case 0x34: { u32 *m = qptr(v, VI(v, is)); if (ft) for (int c = 0; c < 4; c++) if (has(dest, c)) VFP(v)[ft].u[c] = m[c];
        setvi(v, is, VI(v, is) + 1); return 0; }                                                 /* LQI */
    case 0x35: { u32 *m = qptr(v, VI(v, it)); vf128 s = VFP(v)[fs]; if (!fs) { s.u[0] = s.u[1] = s.u[2] = 0; s.f[3] = 1; }
        for (int c = 0; c < 4; c++) if (has(dest, c)) m[c] = s.u[c]; setvi(v, it, VI(v, it) + 1); return 0; }   /* SQI */
    case 0x36: { setvi(v, is, VI(v, is) - 1); u32 *m = qptr(v, VI(v, is));
        if (ft) for (int c = 0; c < 4; c++) if (has(dest, c)) VFP(v)[ft].u[c] = m[c]; return 0; }   /* LQD */
    case 0x37: { setvi(v, it, VI(v, it) - 1); u32 *m = qptr(v, VI(v, it)); vf128 s = VFP(v)[fs]; if (!fs) { s.u[0] = s.u[1] = s.u[2] = 0; s.f[3] = 1; }
        for (int c = 0; c < 4; c++) if (has(dest, c)) m[c] = s.u[c]; return 0; }               /* SQD */
    case 0x38: { float T[4]; vfget(v, fs, S); vfget(v, ft, T); float d = T[ftf], r;           /* DIV */
        if (d == 0.0f) r = ((VFP(v)[fs].u[fsf] ^ VFP(v)[ft].u[ftf]) & 0x80000000u) ? -3.402823466e38f : 3.402823466e38f;
        else r = S[fsf] / d;
        q_set(v, r, 7); return 0; }
    case 0x39: { float T[4]; vfget(v, ft, T); q_set(v, sqrtf(fabsf(T[ftf])), 7); return 0; }  /* SQRT */
    case 0x3a: { float T[4]; vfget(v, fs, S); vfget(v, ft, T); float d = sqrtf(fabsf(T[ftf]));   /* RSQRT */
        q_set(v, d == 0.0f ? 3.402823466e38f : S[fsf] / d, 13); return 0; }
    case 0x3b: if (v->q_wait) { QP(v)->u = v->q_pend; v->q_wait = 0; } return 0;              /* WAITQ */
    case 0x3c: setvi(v, ft, VFP(v)[fs].u[fsf]); return 0;                                       /* MTIR */
    case 0x3d: if (ft) for (int c = 0; c < 4; c++) if (has(dest, c)) VFP(v)[ft].s[c] = (s16)VI(v, fs); return 0;   /* MFIR */
    case 0x3e: { u32 *m = qptr(v, VI(v, is)); for (int c = 0; c < 4; c++) if (has(dest, c)) setvi(v, it, m[c]); return 0; }   /* ILWR */
    case 0x3f: { u32 *m = qptr(v, VI(v, is)); for (int c = 0; c < 4; c++) if (has(dest, c)) m[c] = VI(v, it); return 0; }     /* ISWR */
    case 0x40: case 0x41: { u32 r = VIP(v)[20];                                                 /* RNEXT / RGET */
        if (x == 0x40) { int b = ((r >> 4) ^ (r >> 22)) & 1; r = ((r << 1) | b) & 0x7fffff; VIP(v)[20] = r; }
        if (ft) for (int c = 0; c < 4; c++) if (has(dest, c)) VFP(v)[ft].u[c] = 0x3f800000u | (r & 0x7fffff);
        return 0; }
    case 0x42: VIP(v)[20] = VFP(v)[fs].u[fsf] & 0x7fffff; return 0;                              /* RINIT */
    case 0x43: VIP(v)[20] = (VIP(v)[20] ^ VFP(v)[fs].u[fsf]) & 0x7fffff; return 0;                /* RXOR */
    case 0x64: if (ft) for (int c = 0; c < 4; c++) if (has(dest, c)) VFP(v)[ft].u[c] = v->p; return 0;   /* MFP */
    case 0x68: setvi(v, it, vu ? vif1_top_value() : 0); return 0;                              /* XTOP */
    case 0x69: setvi(v, it, vu ? vif1_itop_value() : vif0_itop_value()); return 0;             /* XITOP */
    case 0x6c: if (vu && !vu_no_kick) xgkick(&VU[1], VI(v, is)); return 0;                                         /* XGKICK */
    case 0x7b: if (v->p_wait) { v->p = v->p_pend; v->p_wait = 0; } return 0;                   /* WAITP */
    }
    vfget(v, fs, S);
    float sx = S[0], sy = S[1], sz = S[2], sw = S[3], f1 = S[fsf];
    switch (x) {
    case 0x70: efu(v, sx * sx + sy * sy + sz * sz, 11); return 0;                          /* ESADD */
    case 0x71: efu(v, 1.0f / (sx * sx + sy * sy + sz * sz), 18); return 0;                  /* ERSADD */
    case 0x72: efu(v, sqrtf(sx * sx + sy * sy + sz * sz), 18); return 0;                    /* ELENG */
    case 0x73: efu(v, 1.0f / sqrtf(sx * sx + sy * sy + sz * sz), 24); return 0;             /* ERLENG */
    case 0x74: efu(v, atan2f(sy, sx), 54); return 0;                                        /* EATANxy */
    case 0x75: efu(v, atan2f(sz, sx), 54); return 0;                                        /* EATANxz */
    case 0x76: efu(v, sx + sy + sz + sw, 12); return 0;                                     /* ESUM */
    case 0x78: efu(v, sqrtf(fabsf(f1)), 12); return 0;                                      /* ESQRT */
    case 0x79: efu(v, 1.0f / sqrtf(fabsf(f1)), 18); return 0;                               /* ERSQRT */
    case 0x7a: efu(v, 1.0f / f1, 12); return 0;                                             /* ERCPR */
    case 0x7c: efu(v, sinf(f1), 29); return 0;                                              /* ESIN */
    case 0x7d: efu(v, atanf(f1), 54); return 0;                                             /* EATAN */
    case 0x7e: efu(v, expf(-f1), 44); return 0;                                             /* EEXP */
    }
    { static int once; if (!once++) LOG("vu%d: unknown lower %08x at %04x\n", vu, w, pc); }
    return 0;
}

/* ---- VU1 code snapshots ---------------------------------------------------------------
 * MPG writes that change VU1 micro memory bump vu1_code_gen (vif.c). The code
 * is then hashed again; VU_RECORD=<dir> writes each new snapshot there as
 * vu1_<hash>.bin plus the entry points used (vu1_<hash>.pcs) for the VU1
 * recompiler (tools/recomp_vu.py). */
u32 vu1_code_gen = 1;
static u32 seen_gen;
u64 vu1_code_hash;
static const char *rec_dir;
static int rec_init;
typedef struct { u64 hash; u16 pcs[64]; int npcs; } snap_t;
static snap_t snaps[64]; static int nsnaps; static snap_t *cur_snap;

static u64 hash_code(void) {
    u64 h = 0xcbf29ce484222325ull;
    const u64 *p = (const u64 *)vu1_code;
    for (int i = 0; i < 0x4000 / 8; i++) { h ^= p[i]; h *= 0x100000001b3ull; }
    return h;
}

static void rec_pcs(snap_t *s) {
    if (!rec_dir) return;
    char name[512]; snprintf(name, sizeof name, "%s/vu1_%016llx.pcs", rec_dir, (unsigned long long)s->hash);
    FILE *f = fopen(name, "w"); if (!f) return;
    for (int i = 0; i < s->npcs; i++) fprintf(f, "%04x\n", s->pcs[i]);
    fclose(f);
}

static void vu1_code_check(u32 pc) {
    if (!rec_init) { rec_init = 1; rec_dir = getenv("VU_RECORD"); }
    if (seen_gen != vu1_code_gen) {
        seen_gen = vu1_code_gen;
        vu1_code_hash = hash_code();
        cur_snap = 0;
        for (int i = 0; i < nsnaps; i++) if (snaps[i].hash == vu1_code_hash) cur_snap = &snaps[i];
        if (!cur_snap && nsnaps < 64) {
            cur_snap = &snaps[nsnaps++]; cur_snap->hash = vu1_code_hash; cur_snap->npcs = 0;
            LOG("vu: VU1 code snapshot %016llx (%d so far)\n", (unsigned long long)vu1_code_hash, nsnaps);
            if (rec_dir) {
                char name[512]; snprintf(name, sizeof name, "%s/vu1_%016llx.bin", rec_dir, (unsigned long long)vu1_code_hash);
                FILE *f = fopen(name, "wb"); if (f) { fwrite(vu1_code, 1, 0x4000, f); fclose(f); }
            }
        }
    }
    if (!cur_snap) return;
    for (int i = 0; i < cur_snap->npcs; i++) if (cur_snap->pcs[i] == pc) return;
    if (cur_snap->npcs < 64) { cur_snap->pcs[cur_snap->npcs++] = (u16)pc; rec_pcs(cur_snap); }
}

/* ---- recompiled VU1 programs (tools/recomp_vu.py -> gen_vu/vu1_rec.h) ----------------
 * VU_PAIR is one pair, the same steps as the interpreter loop below. FL: keep
 * the flag update; CL: the sources are clamped already (upper()). VU_PAIR_JRG
 * is the delay slot of a JR/JALR: a target outside OK (the run entries and
 * return points the clamp analysis starts from) runs in the interpreter. */
#define VU_PAIR(P, LO, HI, FL, CL) VU_PAIR_G(P, LO, HI, FL, CL, (const u8 *)0)
#define VU_PAIR_JRG(P, LO, HI, FL, CL, OK) VU_PAIR_G(P, LO, HI, FL, CL, OK)
#define VU_PAIR_G(P, LO, HI, FL, CL, OK) do {                                        \
        upres up_; u32 tg_ = 0; int br_ = 0;                                         \
        upper(v, (HI), &up_, (CL));                                                  \
        if ((HI) & 0x80000000u) IP(v)->u = (LO);                                      \
        else br_ = lower(v, (LO), (P), &tg_, 1);                                     \
        upper_commit_fl(v, &up_, (FL)); clip_tick(v);                                \
        if (v->q_wait && --v->q_wait == 0) QP(v)->u = v->q_pend;                      \
        if (v->p_wait && --v->p_wait == 0) v->p = v->p_pend;                         \
        next = (P) + 8;                                                              \
        int jr_ = bp;                                                                \
        if (bp) { next = bt; bp = 0; }                                               \
        if (br_) { bt = tg_; bp = 1; }                                               \
        if (ea == 0) { v->tpc = next; goto out; }                                    \
        if (ea > 0) ea--;                                                            \
        if ((HI) & 0x40000000u) ea = 0;                                              \
        if ((OK) && jr_ && !(OK)[(next & 0x3ff8) >> 3]) goto fb;   /* also to P + 8 */\
        if (next != (P) + 8) {                                                       \
            if (++guard > 50000000) goto out;                                        \
            goto *L[(next & 0x3ff8) >> 3];                                           \
        }                                                                            \
    } while (0)
/* a pair the recompiler proved is no branch, has no E bit and does not follow
 * one (tools/recomp_vu.py plain_pairs): bp is 0 and ea -1 here, so only the
 * work and the Q/P countdowns remain; it falls through to the next pair */
#define VU_PAIR_PLAIN(P, LO, HI, FL, CL) do {                                        \
        upres up_; u32 tg_ = 0;                                                      \
        upper(v, (HI), &up_, (CL));                                                  \
        if ((HI) & 0x80000000u) IP(v)->u = (LO);                                      \
        else (void)lower(v, (LO), (P), &tg_, 1);                                     \
        upper_commit_fl(v, &up_, (FL)); clip_tick(v);                                \
        if (v->q_wait && --v->q_wait == 0) QP(v)->u = v->q_pend;                      \
        if (v->p_wait && --v->p_wait == 0) v->p = v->p_pend;                         \
    } while (0)
/* A recompiled chunk works on a local copy of VU1's pipeline state (Q/P
 * countdowns, status, MAC, P): clang keeps it in registers across the pairs,
 * where VU[1]'s fields were loaded and stored at every pair. The register
 * files stay the globals (rf1). VU_REC_LEAVE writes the copy back before the
 * chunk returns or hands over to the interpreter. */
#define VU_REC_ENTER                                                                  \
    vu_t lv_; lv_.rf1 = 1; lv_.p = VU[1].p; lv_.status = VU[1].status;                \
    lv_.mac = VU[1].mac; lv_.q_pend = VU[1].q_pend; lv_.p_pend = VU[1].p_pend;        \
    lv_.q_wait = VU[1].q_wait; lv_.p_wait = VU[1].p_wait; lv_.tpc = VU[1].tpc;        \
    lv_.cp_n = VU[1].cp_n; lv_.clip_new = VU[1].clip_new;   /* clip pipeline */        \
    for (int k_ = 0; k_ < lv_.cp_n; k_++) { lv_.cp_val[k_] = VU[1].cp_val[k_]; lv_.cp_cnt[k_] = VU[1].cp_cnt[k_]; } \
    vu_t *const v = &lv_
/* VU_REC_LOCAL_VF: the chunk also keeps the VF registers it uses in a local
 * array (copied in after this, back before it leaves: tools/recomp_vu.py);
 * the other register files stay the globals, now through constant pointers. */
#define VU_REC_LOCAL_VF(arr)                                                          \
    vf128 arr[32]; lv_.rf1 = 0; lv_.vf = arr; lv_.vi = vu1_vi; lv_.acc = &vu1_acc;    \
    lv_.q = &vu1_q; lv_.i = &vu1_i; lv_.mem = vu1_mem; lv_.qmask = 0x3ff
#define VU_REC_LEAVE do {                                                             \
        VU[1].p = lv_.p; VU[1].status = lv_.status; VU[1].mac = lv_.mac;             \
        VU[1].q_pend = lv_.q_pend; VU[1].p_pend = lv_.p_pend;                         \
        VU[1].q_wait = lv_.q_wait; VU[1].p_wait = lv_.p_wait; VU[1].tpc = lv_.tpc;    \
        VU[1].cp_n = lv_.cp_n; VU[1].clip_new = lv_.clip_new;                         \
        for (int k_ = 0; k_ < lv_.cp_n; k_++) { VU[1].cp_val[k_] = lv_.cp_val[k_]; VU[1].cp_cnt[k_] = lv_.cp_cnt[k_]; } \
    } while (0)
static void vu_interp_state(vu_t *v, u32 pc, int n, int end_after, int branch_pending, u32 branch_to);
static void vu_interp(vu_t *v, u32 pc, int n) { vu_interp_state(v, pc, n, -1, 0, 0); }
static u64 n_rec_fallback;
/* a recompiled program jumped to a pair the recompiler did not generate */
static void vu_rec_fallback(vu_t *v, u32 next, int ea, int bp, u32 bt) {
    static u16 seen[64]; static int nseen;
    n_rec_fallback++;
    u16 p = (u16)(next & 0x3ff8); int k;
    for (k = 0; k < nseen && seen[k] != p; k++) {}
    if (k == nseen && nseen < 64) { seen[nseen++] = p; LOG("vu: recompiled VU1 code jumped to %04x, not generated (add it as an entry)\n", p); }
    vu_interp_state(v, next, 1, ea, bp, bt);
}
/* a run starts at a pc that was not recorded as an entry (VU_RECORD): the
 * clamp analysis did not start from it, so it is interpreted */
static void vu_rec_new_entry(vu_t *v, u32 pc) {
    static u16 seen[64]; static int nseen;
    u16 p = (u16)(pc & 0x3ff8); int k;
    for (k = 0; k < nseen && seen[k] != p; k++) {}
    if (k == nseen && nseen < 64) { seen[nseen++] = p; LOG("vu: VU1 run starts at %04x, not a recorded entry: interpreted (record it with VU_RECORD)\n", p); }
    vu_interp(v, pc, 1);
}
#if __has_include("vu1_rec.h")
#include "vu1_rec.h"
#else
static const struct { u64 hash; void (*fn)(vu_t *, u32); } vu1_rec_table[] = { { 0, 0 } };
#endif

static void (*rec_fn)(vu_t *, u32);
static u32 rec_gen;
static int rec_mode = -1;        /* 0 interpreter only (VU_INTERP), 1 compiled, 2 compiled + check every run (VU_CHECK) */
static u64 n_rec_runs, n_check_runs, n_check_bad;


/* VU_CHECK: run the interpreter on a copy first, then the compiled code, and compare */
typedef struct { vf128 vf[32], acc; u32 vi[32]; fr q, i; u32 p, status, mac, q_pend, p_pend, tpc; int q_wait, p_wait; u8 mem[0x4000]; } vu1_snap;
static void snap_take(vu1_snap *s, vu_t *v) {
    memcpy(s->vf, VFP(v), sizeof s->vf); s->acc = *ACCP(v); memcpy(s->vi, VIP(v), sizeof s->vi);
    s->q = *v->q; s->i = *v->i; s->p = v->p; s->status = v->status; s->mac = v->mac;
    s->q_pend = v->q_pend; s->p_pend = v->p_pend; s->q_wait = v->q_wait; s->p_wait = v->p_wait; s->tpc = v->tpc;
    memcpy(s->mem, MEMP(v), sizeof s->mem);
}
static void snap_put(const vu1_snap *s, vu_t *v) {
    memcpy(VFP(v), s->vf, sizeof s->vf); *ACCP(v) = s->acc; memcpy(VIP(v), s->vi, sizeof s->vi);
    *v->q = s->q; *v->i = s->i; v->p = s->p; v->status = s->status; v->mac = s->mac;
    v->q_pend = s->q_pend; v->p_pend = s->p_pend; v->q_wait = s->q_wait; v->p_wait = s->p_wait; v->tpc = s->tpc;
    memcpy(MEMP(v), s->mem, sizeof s->mem);
}
static int snap_diff(const vu1_snap *a, const vu1_snap *b, char *why, size_t n) {
    for (int r = 0; r < 32; r++) for (int c = 0; c < 4; c++)
        if (a->vf[r].u[c] != b->vf[r].u[c]) { snprintf(why, n, "vf%d.%c %08x vs %08x", r, "xyzw"[c], a->vf[r].u[c], b->vf[r].u[c]); return 1; }
    for (int c = 0; c < 4; c++) if (a->acc.u[c] != b->acc.u[c]) { snprintf(why, n, "acc.%c", "xyzw"[c]); return 1; }
    /* MAC/status (vi16/vi17) are not compared: the recompiled code drops flag updates nothing reads */
    for (int r = 0; r < 32; r++) if (r != 16 && r != 17 && a->vi[r] != b->vi[r]) { snprintf(why, n, "vi%d %x vs %x", r, a->vi[r], b->vi[r]); return 1; }
    if (a->q.u != b->q.u || a->i.u != b->i.u || a->p != b->p) { snprintf(why, n, "q/i/p"); return 1; }
    if (a->tpc != b->tpc) { snprintf(why, n, "tpc %x vs %x", a->tpc, b->tpc); return 1; }
    for (u32 k = 0; k < sizeof a->mem; k += 4) if (memcmp(a->mem + k, b->mem + k, 4)) { snprintf(why, n, "mem qw %x", k >> 4); return 1; }
    return 0;
}

static void vu1_run_compiled(vu_t *v, u32 pc) {
    if (rec_mode < 0) rec_mode = getenv("VU_INTERP") ? 0 : getenv("VU_CHECK") ? 2 : 1;
    if (rec_gen != vu1_code_gen || !rec_fn) {
        if (rec_gen != vu1_code_gen) {
            rec_gen = vu1_code_gen; rec_fn = 0;
            for (int k = 0; vu1_rec_table[k].fn; k++) if (vu1_rec_table[k].hash == vu1_code_hash) rec_fn = vu1_rec_table[k].fn;
            if (rec_mode) LOG("vu: VU1 code %016llx %s\n", (unsigned long long)vu1_code_hash, rec_fn ? "runs recompiled" : "not recompiled, interpreting");
        }
    }
    if (!rec_mode || !rec_fn) { vu_interp(v, pc, 1); return; }
    n_rec_runs++;
    if (rec_mode == 2) {
        static vu1_snap before, ref, got;
        snap_take(&before, v);
        vu_no_kick = 1; vu_interp(v, pc, 1); vu_no_kick = 0;
        snap_take(&ref, v);
        snap_put(&before, v);
        rec_fn(v, pc);
        snap_take(&got, v);
        n_check_runs++;
        char why[96];
        if (snap_diff(&ref, &got, why, sizeof why)) {
            if (n_check_bad++ < 20) LOG("vu: VU_CHECK mismatch at entry %04x (frame %llu): %s\n", pc, (unsigned long long)g_frame, why);
        }
        return;
    }
    rec_fn(v, pc);
}

/* ---- run --------------------------------------------------------------------------- */
static void vu_run(int n, u32 pc) {
    vu_setup();
    vu_t *v = &VU[n];
    v->status = VIP(v)[16] & 0xfff; v->mac = VIP(v)[17] & 0xffff;
    if (pc == 0xffffffffu) pc = v->tpc;
    v->runs++;
    if (n == 1) { vu1_code_check(pc & 0x3ff8); vu1_run_compiled(v, pc); }
    else vu_interp(v, pc, n);
    VIP(v)[16] = v->status; VIP(v)[17] = v->mac;
    clip_flush(v);
    if (v->q_wait) { QP(v)->u = v->q_pend; v->q_wait = 0; }
    if (v->p_wait) { v->p = v->p_pend; v->p_wait = 0; }
}

/* the interpreter loop; recompiled code also enters it mid-program (ea/bp/bt) */
static void vu_interp_state(vu_t *v, u32 pc, int n, int end_after, int branch_pending, u32 branch_to) {
    for (u64 guard = 0; guard < 50000000; guard++) {
        pc &= v->cmask & ~7u;
        u32 lo, hi;
        memcpy(&lo, v->code + pc, 4); memcpy(&hi, v->code + pc + 4, 4);
        v->pairs++;
        upres up;
        upper(v, hi, &up, 0);
        u32 target = 0; int br = 0;
        if (hi & 0x80000000u) { IP(v)->u = lo; }               /* I bit: lower word is the I constant */
        else br = lower(v, lo, pc, &target, n);
        upper_commit(v, &up); clip_tick(v);
        VIP(v)[0] = 0;
        if (v->q_wait && --v->q_wait == 0) QP(v)->u = v->q_pend;
        if (v->p_wait && --v->p_wait == 0) v->p = v->p_pend;
        u32 next = pc + 8;
        if (branch_pending) { next = branch_to; branch_pending = 0; }
        if (br) { branch_to = target; branch_pending = 1; }
        if (end_after == 0) { v->tpc = next; clip_flush(v); break; }
        if (end_after > 0) end_after--;
        if (hi & 0x40000000u) end_after = 0;                   /* E bit: one more pair */
        pc = next;
    }
}

void vu1_start(u32 pc) { vu_run(1, pc); }
void vu_init(void) { vu_setup(); }
void vu_micro_run(int vu, u32 pc) { vu_run(vu ? 1 : 0, pc); }

void vu_stats(void) {
    LOG("vu: vu0 %llu runs %llu pairs, vu1 %llu runs %llu interpreted pairs\n", (unsigned long long)VU[0].runs,
        (unsigned long long)VU[0].pairs, (unsigned long long)VU[1].runs, (unsigned long long)VU[1].pairs);
    LOG("vu: vu1 %llu recompiled runs, %llu checked, %llu mismatches\n", (unsigned long long)n_rec_runs,
        (unsigned long long)n_check_runs, (unsigned long long)n_check_bad);
    if (n_rec_fallback) LOG("vu: vu1 %llu jumps to pairs that were not recompiled (interpreted)\n", (unsigned long long)n_rec_fallback);
}
