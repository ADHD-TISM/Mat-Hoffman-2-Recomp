/* R5900 MMI (multimedia) instructions, pipeline-1 mult/div, FPU control. */
#include "rt.h"

static int warned[256];
static void unimpl(u32 w, int key) {
    if (!warned[key & 255]++) LOG("mmi: unimplemented %08x (fn %02x sa %02x)\n", w, w & 63, (w >> 6) & 31);
}

static inline s64 sx32(u32 v) { return (s64)(s32)v; }

void ee_div(u32 a, u32 b, int p) {
    s32 q, r;
    if (b == 0) { q = (s32)a < 0 ? 1 : -1; r = (s32)a; }
    else if (a == 0x80000000u && b == 0xffffffffu) { q = (s32)a; r = 0; }
    else { q = (s32)a / (s32)b; r = (s32)a % (s32)b; }
    ee.lo.sd[p] = q; ee.hi.sd[p] = r;
}
void ee_divu(u32 a, u32 b, int p) {
    u32 q, r;
    if (b == 0) { q = 0xffffffffu; r = a; } else { q = a / b; r = a % b; }
    ee.lo.sd[p] = sx32(q); ee.hi.sd[p] = sx32(r);
}

u32 fpu_cfc1(int r) { return r == 31 ? ee.fcr31 : r == 0 ? 0x2e30 : 0; }
void fpu_ctc1(int r, u32 v) { if (r == 31) ee.fcr31 = v; }

static inline s16 sat16(s32 v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (s16)v; }

void mmi_exec(u32 w) {
    int rs = (w >> 21) & 31, rt = (w >> 16) & 31, rd = (w >> 11) & 31, sa = (w >> 6) & 31, fn = w & 63;
    r128 S = ee.r[rs], T = ee.r[rt], D; memset(&D, 0, sizeof D);
    if (rs == 0) memset(&S, 0, 16);
    if (rt == 0) memset(&T, 0, 16);
    int k;
    switch (fn) {
    case 0x00: case 0x01: case 0x20: case 0x21: {   /* MADD / MADDU / MADD1 / MADDU1 */
        int p = fn >= 0x20;
        u64 acc = ((u64)ee.hi.w[2 * p] << 32) | ee.lo.w[2 * p];
        if (fn & 1) acc += (u64)S.w[0] * T.w[0]; else acc += (u64)((s64)S.sw[0] * T.sw[0]);
        ee.lo.sd[p] = sx32((u32)acc); ee.hi.sd[p] = sx32((u32)(acc >> 32));
        if (rd) ee.r[rd].sd[0] = ee.lo.sd[p];
        return; }
    case 0x04:   /* PLZCW */
        for (k = 0; k < 2; k++) {
            u32 v = S.w[k]; if (v & 0x80000000u) v = ~v;
            int n = 0; while (n < 31 && !(v & (0x40000000u >> n))) n++;
            D.w[k] = n;
        }
        if (rd) { ee.r[rd].w[0] = D.w[0]; ee.r[rd].w[1] = D.w[1]; }
        return;
    case 0x10: if (rd) ee.r[rd].d[0] = ee.hi.d[1]; return;
    case 0x11: ee.hi.d[1] = S.d[0]; return;
    case 0x12: if (rd) ee.r[rd].d[0] = ee.lo.d[1]; return;
    case 0x13: ee.lo.d[1] = S.d[0]; return;
    case 0x18: { s64 m = (s64)S.sw[0] * T.sw[0]; ee.lo.sd[1] = sx32((u32)m); ee.hi.sd[1] = sx32((u32)(m >> 32)); if (rd) ee.r[rd].sd[0] = ee.lo.sd[1]; return; }
    case 0x19: { u64 m = (u64)S.w[0] * T.w[0]; ee.lo.sd[1] = sx32((u32)m); ee.hi.sd[1] = sx32((u32)(m >> 32)); if (rd) ee.r[rd].sd[0] = ee.lo.sd[1]; return; }
    case 0x1a: ee_div(S.w[0], T.w[0], 1); return;
    case 0x1b: ee_divu(S.w[0], T.w[0], 1); return;
    case 0x34: for (k = 0; k < 8; k++) D.h[k] = T.h[k] << (sa & 15); break;
    case 0x36: for (k = 0; k < 8; k++) D.h[k] = T.h[k] >> (sa & 15); break;
    case 0x37: for (k = 0; k < 8; k++) D.sh[k] = T.sh[k] >> (sa & 15); break;
    case 0x3c: for (k = 0; k < 4; k++) D.w[k] = T.w[k] << sa; break;
    case 0x3e: for (k = 0; k < 4; k++) D.w[k] = T.w[k] >> sa; break;
    case 0x3f: for (k = 0; k < 4; k++) D.sw[k] = T.sw[k] >> sa; break;
    case 0x30:   /* PMFHL */
        switch (sa) {
        case 0: D.w[0] = ee.lo.w[0]; D.w[1] = ee.hi.w[0]; D.w[2] = ee.lo.w[2]; D.w[3] = ee.hi.w[2]; break;
        case 1: D.w[0] = ee.lo.w[1]; D.w[1] = ee.hi.w[1]; D.w[2] = ee.lo.w[3]; D.w[3] = ee.hi.w[3]; break;
        case 3: for (k = 0; k < 2; k++) { D.h[4*k] = ee.lo.h[4*k]; D.h[4*k+1] = ee.lo.h[4*k+2]; D.h[4*k+2] = ee.hi.h[4*k]; D.h[4*k+3] = ee.hi.h[4*k+2]; } break;
        default: unimpl(w, 0x80 + sa); return;
        }
        break;
    case 0x31:   /* PMTHL.LW */
        ee.lo.w[0] = S.w[0]; ee.hi.w[0] = S.w[1]; ee.lo.w[2] = S.w[2]; ee.hi.w[2] = S.w[3];
        return;
    case 0x08:   /* MMI0 */
        switch (sa) {
        case 0x00: for (k = 0; k < 4; k++) D.w[k] = S.w[k] + T.w[k]; break;
        case 0x01: for (k = 0; k < 4; k++) D.w[k] = S.w[k] - T.w[k]; break;
        case 0x02: for (k = 0; k < 4; k++) D.w[k] = S.sw[k] > T.sw[k] ? ~0u : 0; break;
        case 0x03: for (k = 0; k < 4; k++) D.sw[k] = S.sw[k] > T.sw[k] ? S.sw[k] : T.sw[k]; break;
        case 0x04: for (k = 0; k < 8; k++) D.h[k] = S.h[k] + T.h[k]; break;
        case 0x05: for (k = 0; k < 8; k++) D.h[k] = S.h[k] - T.h[k]; break;
        case 0x06: for (k = 0; k < 8; k++) D.h[k] = S.sh[k] > T.sh[k] ? 0xffff : 0; break;
        case 0x07: for (k = 0; k < 8; k++) D.sh[k] = S.sh[k] > T.sh[k] ? S.sh[k] : T.sh[k]; break;
        case 0x08: for (k = 0; k < 16; k++) D.b[k] = S.b[k] + T.b[k]; break;
        case 0x09: for (k = 0; k < 16; k++) D.b[k] = S.b[k] - T.b[k]; break;
        case 0x0a: for (k = 0; k < 16; k++) D.b[k] = S.sb[k] > T.sb[k] ? 0xff : 0; break;
        case 0x14: for (k = 0; k < 8; k++) D.sh[k] = sat16(S.sh[k] + T.sh[k]); break;
        case 0x15: for (k = 0; k < 8; k++) D.sh[k] = sat16(S.sh[k] - T.sh[k]); break;
        case 0x12: D.w[0] = T.w[0]; D.w[1] = S.w[0]; D.w[2] = T.w[1]; D.w[3] = S.w[1]; break;
        case 0x13: D.w[0] = T.w[0]; D.w[1] = T.w[2]; D.w[2] = S.w[0]; D.w[3] = S.w[2]; break;
        case 0x18: for (k = 0; k < 16; k++) { int v = S.sb[k] + T.sb[k]; D.sb[k] = (s8)(v > 127 ? 127 : v < -128 ? -128 : v); } break;   /* PADDSB */
        case 0x19: for (k = 0; k < 16; k++) { int v = S.sb[k] - T.sb[k]; D.sb[k] = (s8)(v > 127 ? 127 : v < -128 ? -128 : v); } break;   /* PSUBSB */
        case 0x16: for (k = 0; k < 4; k++) { D.h[2*k] = T.h[k]; D.h[2*k+1] = S.h[k]; } break;
        case 0x17: for (k = 0; k < 4; k++) { D.h[k] = T.h[2*k]; D.h[4+k] = S.h[2*k]; } break;
        case 0x1a: for (k = 0; k < 8; k++) { D.b[2*k] = T.b[k]; D.b[2*k+1] = S.b[k]; } break;
        case 0x1b: for (k = 0; k < 8; k++) { D.b[k] = T.b[2*k]; D.b[8+k] = S.b[2*k]; } break;
        default: unimpl(w, sa); return;
        }
        break;
    case 0x28:   /* MMI1 */
        switch (sa) {
        case 0x01: for (k = 0; k < 4; k++) D.sw[k] = T.sw[k] == (s32)0x80000000 ? 0x7fffffff : T.sw[k] < 0 ? -T.sw[k] : T.sw[k]; break;
        case 0x02: for (k = 0; k < 4; k++) D.w[k] = S.w[k] == T.w[k] ? ~0u : 0; break;
        case 0x03: for (k = 0; k < 4; k++) D.sw[k] = S.sw[k] < T.sw[k] ? S.sw[k] : T.sw[k]; break;
        case 0x05: for (k = 0; k < 8; k++) D.sh[k] = T.sh[k] == -32768 ? 32767 : T.sh[k] < 0 ? -T.sh[k] : T.sh[k]; break;
        case 0x06: for (k = 0; k < 8; k++) D.h[k] = S.h[k] == T.h[k] ? 0xffff : 0; break;
        case 0x07: for (k = 0; k < 8; k++) D.sh[k] = S.sh[k] < T.sh[k] ? S.sh[k] : T.sh[k]; break;
        case 0x0a: for (k = 0; k < 16; k++) D.b[k] = S.b[k] == T.b[k] ? 0xff : 0; break;
        case 0x12: D.w[0] = T.w[2]; D.w[1] = S.w[2]; D.w[2] = T.w[3]; D.w[3] = S.w[3]; break;
        case 0x16: for (k = 0; k < 4; k++) { D.h[2*k] = T.h[4+k]; D.h[2*k+1] = S.h[4+k]; } break;
        case 0x18: for (k = 0; k < 16; k++) { int v = S.b[k] + T.b[k]; D.b[k] = v > 255 ? 255 : v; } break;
        case 0x19: for (k = 0; k < 16; k++) { int v = S.b[k] - T.b[k]; D.b[k] = v < 0 ? 0 : v; } break;
        case 0x1a: for (k = 0; k < 8; k++) { D.b[2*k] = T.b[8+k]; D.b[2*k+1] = S.b[8+k]; } break;
        case 0x1b: {   /* QFSRV: funnel shift rs:rt right by SA bytes */
            u8 cat[32]; memcpy(cat, T.b, 16); memcpy(cat + 16, S.b, 16);
            memcpy(D.b, cat + (ee.sa & 15), 16); break; }
        default: unimpl(w, 0x20 + sa); return;
        }
        break;
    case 0x09:   /* MMI2 */
        switch (sa) {
        case 0x02: for (k = 0; k < 2; k++) D.sd[k] = sx32(T.w[2*k] << (S.w[2*k] & 31)); break;
        case 0x03: for (k = 0; k < 2; k++) D.sd[k] = sx32(T.w[2*k] >> (S.w[2*k] & 31)); break;
        case 0x08: D = ee.hi; break;
        case 0x09: D = ee.lo; break;
        case 0x0a: for (k = 0; k < 4; k++) { D.h[2*k] = T.h[k]; D.h[2*k+1] = S.h[4+k]; } break;
        case 0x0c: for (k = 0; k < 2; k++) { s64 m = (s64)S.sw[2*k] * T.sw[2*k]; D.sd[k] = m; ee.lo.sd[k] = sx32((u32)m); ee.hi.sd[k] = sx32((u32)(m >> 32)); } break;
        case 0x0e: D.d[0] = T.d[0]; D.d[1] = S.d[0]; break;
        case 0x10: case 0x1c: {   /* PMADDH / PMULTH: 8 signed halfword products. Products 0,1,4,5 go to
                                     LO words 0-3, products 2,3,6,7 to HI words 0-3 (PMADDH adds to what is
                                     there); rd gets the even products: LO0, HI0, LO2, HI2. Used by fn_002ec470. */
            for (k = 0; k < 8; k++) {
                s32 p = (s32)S.sh[k] * (s32)T.sh[k];
                r128 *acc = (k & 2) ? &ee.hi : &ee.lo;
                int i = (k & 1) | ((k & 4) >> 1);
                acc->w[i] = sa == 0x10 ? acc->w[i] + (u32)p : (u32)p;
            }
            D.w[0] = ee.lo.w[0]; D.w[1] = ee.hi.w[0]; D.w[2] = ee.lo.w[2]; D.w[3] = ee.hi.w[2];
            break; }
        case 0x12: D.d[0] = S.d[0] & T.d[0]; D.d[1] = S.d[1] & T.d[1]; break;
        case 0x13: D.d[0] = S.d[0] ^ T.d[0]; D.d[1] = S.d[1] ^ T.d[1]; break;
        case 0x1a: D = T; D.h[0] = T.h[2]; D.h[2] = T.h[0]; D.h[4] = T.h[6]; D.h[6] = T.h[4]; break;
        case 0x1b: for (k = 0; k < 2; k++) for (int j = 0; j < 4; j++) D.h[4*k+j] = T.h[4*k+3-j]; break;
        case 0x1e: D.w[0] = T.w[2]; D.w[1] = T.w[1]; D.w[2] = T.w[0]; D.w[3] = T.w[3]; break;
        case 0x1f: D.w[0] = T.w[1]; D.w[1] = T.w[2]; D.w[2] = T.w[0]; D.w[3] = T.w[3]; break;
        default: unimpl(w, 0x40 + sa); return;
        }
        break;
    case 0x29:   /* MMI3 */
        switch (sa) {
        case 0x03: for (k = 0; k < 2; k++) D.sd[k] = sx32((u32)(T.sw[2*k] >> (S.w[2*k] & 31))); break;
        case 0x08: ee.hi = S; return;
        case 0x09: ee.lo = S; return;
        case 0x0a: for (k = 0; k < 4; k++) { D.h[2*k] = T.h[2*k]; D.h[2*k+1] = S.h[2*k]; } break;
        case 0x0c: for (k = 0; k < 2; k++) { u64 m = (u64)S.w[2*k] * T.w[2*k]; D.d[k] = m; ee.lo.sd[k] = sx32((u32)m); ee.hi.sd[k] = sx32((u32)(m >> 32)); } break;
        case 0x0e: D.d[0] = S.d[1]; D.d[1] = T.d[1]; break;
        case 0x12: D.d[0] = S.d[0] | T.d[0]; D.d[1] = S.d[1] | T.d[1]; break;
        case 0x13: D.d[0] = ~(S.d[0] | T.d[0]); D.d[1] = ~(S.d[1] | T.d[1]); break;
        case 0x1a: D = T; D.h[1] = T.h[2]; D.h[2] = T.h[1]; D.h[5] = T.h[6]; D.h[6] = T.h[5]; break;
        case 0x1b: for (k = 0; k < 4; k++) { D.h[k] = T.h[0]; D.h[4+k] = T.h[4]; } break;
        case 0x1e: D.w[0] = T.w[0]; D.w[1] = T.w[2]; D.w[2] = T.w[1]; D.w[3] = T.w[3]; break;
        default: unimpl(w, 0x60 + sa); return;
        }
        break;
    default:
        unimpl(w, 0xa0 + fn); return;
    }
    if (rd) ee.r[rd] = D;
}
