/* PS2 EE runtime interface used by the generated code. */
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>

typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef int8_t s8; typedef int16_t s16; typedef int32_t s32; typedef int64_t s64;

typedef union { u64 d[2]; s64 sd[2]; u32 w[4]; s32 sw[4]; u16 h[8]; s16 sh[8]; u8 b[16]; s8 sb[16]; } r128;
typedef union { float f; u32 u; s32 s; } fr;
typedef union { float f[4]; u32 u[4]; s32 s[4]; } vf128;

typedef struct {
    r128 r[32];
    r128 hi, lo;
    u32 sa;
    fr fpr[32];
    fr acc;
    u32 fcr31;
    u32 cop0[32];
    /* VU0 (macro mode) */
    vf128 vf[32];
    u32 vi[32];
    vf128 vacc;
    fr vq, vp, vi_reg;   /* Q, P, I */
    u64 cycles, next_event;
    int intr_enabled;
} ee_state;

extern ee_state ee;

#define GD(n) ee.r[n].d[0]
#define GW(n) ((u32)ee.r[n].d[0])

/* ---- memory ----------------------------------------------------------- */
extern u8 *ee_page[1 << 20];       /* 4 KB pages, NULL = I/O */
extern u8 ee_ram[32 << 20];
extern u8 ee_spr[16 << 10];

u32  io_read32(u32 a);  void io_write32(u32 a, u32 v);
u64  io_read64(u32 a);  void io_write64(u32 a, u64 v);
void io_read128(u32 a, r128 *v); void io_write128(u32 a, const r128 *v);
u16  io_read16(u32 a);  void io_write16(u32 a, u16 v);
u8   io_read8(u32 a);   void io_write8(u32 a, u8 v);

static inline u8 *ee_ptr(u32 a) { u8 *p = ee_page[a >> 12]; return p ? p + (a & 0xfff) : 0; }

static inline u8  mr8(u32 a)  { u8 *p = ee_ptr(a); return p ? *p : io_read8(a); }
static inline u16 mr16(u32 a) { u8 *p = ee_ptr(a); u16 v; if (!p) return io_read16(a); memcpy(&v, p, 2); return v; }
static inline u32 mr32(u32 a) { u8 *p = ee_ptr(a); u32 v; if (!p) return io_read32(a); memcpy(&v, p, 4); return v; }
static inline u64 mr64(u32 a) { u8 *p = ee_ptr(a & ~7u); u64 v; if (!p) return io_read64(a & ~7u); memcpy(&v, p, 8); return v; }
#ifdef EE_WATCH
/* debug build: EE_WATCH=addr[:len] logs every EE store into that range (main.c) */
extern u32 ee_watch_lo, ee_watch_hi;
void ee_watch_hit(u32 a, int n, u64 v);
#define WATCH(a, n, v) do { u32 a_ = (a) & 0x1ffffff; if (a_ < ee_watch_hi && a_ + (n) > ee_watch_lo) ee_watch_hit(a_, (n), (u64)(v)); } while (0)
#else
#define WATCH(a, n, v) do {} while (0)
#endif
static inline void mw8(u32 a, u8 v)   { WATCH(a, 1, v); u8 *p = ee_ptr(a); if (p) *p = v; else io_write8(a, v); }
static inline void mw16(u32 a, u16 v) { WATCH(a, 2, v); u8 *p = ee_ptr(a); if (p) memcpy(p, &v, 2); else io_write16(a, v); }
static inline void mw32(u32 a, u32 v) { WATCH(a, 4, v); u8 *p = ee_ptr(a); if (p) memcpy(p, &v, 4); else io_write32(a, v); }
static inline void mw64(u32 a, u64 v) { WATCH(a & ~7u, 8, v); u8 *p = ee_ptr(a & ~7u); if (p) memcpy(p, &v, 8); else io_write64(a & ~7u, v); }
static inline void mrq(u32 a, int rt) { a &= ~15u; u8 *p = ee_ptr(a); if (p) memcpy(&ee.r[rt], p, 16); else io_read128(a, &ee.r[rt]); }
static inline void mwq(u32 a, int rt) { a &= ~15u; WATCH(a, 16, ee.r[rt].d[0]); u8 *p = ee_ptr(a); if (p) memcpy(p, &ee.r[rt], 16); else io_write128(a, &ee.r[rt]); }

static inline u32 mlwl(u32 a, u32 rt) {
    u32 sh = (a & 3) * 8, w = mr32(a & ~3u);
    return (rt & (0x00ffffffu >> sh)) | (w << (24 - sh));
}
static inline u32 mlwr(u32 a, u32 rt) {
    u32 sh = (a & 3) * 8, w = mr32(a & ~3u);
    return (rt & ~(0xffffffffu >> sh)) | (w >> sh);
}
static inline void mswl(u32 a, u32 rt) {
    u32 sh = (a & 3) * 8, w = mr32(a & ~3u);
    mw32(a & ~3u, (w & ~(0xffffffffu >> (24 - sh))) | (rt >> (24 - sh)));
}
static inline void mswr(u32 a, u32 rt) {
    u32 sh = (a & 3) * 8, w = mr32(a & ~3u);
    mw32(a & ~3u, (w & ~(0xffffffffu << sh)) | (rt << sh));
}
static inline u64 mldl(u32 a, u64 rt) {
    u32 sh = (a & 7) * 8; u64 w = mr64(a & ~7u);
    return (rt & (0x00ffffffffffffffull >> sh)) | (w << (56 - sh));
}
static inline u64 mldr(u32 a, u64 rt) {
    u32 sh = (a & 7) * 8; u64 w = mr64(a & ~7u);
    return sh ? ((rt & ~(~0ull >> sh)) | (w >> sh)) : w;
}
static inline void msdl(u32 a, u64 rt) {
    u32 sh = (a & 7) * 8; u64 w = mr64(a & ~7u);
    u64 m = sh == 56 ? 0 : (~0ull << (sh + 8));
    mw64(a & ~7u, (w & m) | (rt >> (56 - sh)));
}
static inline void msdr(u32 a, u64 rt) {
    u32 sh = (a & 7) * 8; u64 w = mr64(a & ~7u);
    u64 m = sh ? (~0ull >> (64 - sh)) : 0;
    mw64(a & ~7u, (w & m) | (rt << sh));
}

/* ---- FPU (PS2: no inf/nan/denormals, results clamp to +-max) ----------- */
#define FPU_C (1u << 23)
#define FPU_SETC(x) do { if (x) ee.fcr31 |= FPU_C; else ee.fcr31 &= ~FPU_C; } while (0)
static inline float fpu_fix(float x) {
    fr v; v.f = x; u32 e = v.u & 0x7f800000u;
    if (e == 0x7f800000u) v.u = (v.u & 0x80000000u) | 0x7f7fffffu;
    else if (e == 0) v.u &= 0x80000000u;
    return v.f;
}
static inline float fpu_in(float x) { return fpu_fix(x); }
static inline float fpu_add(float a, float b) { return fpu_fix(fpu_in(a) + fpu_in(b)); }
static inline float fpu_sub(float a, float b) { return fpu_fix(fpu_in(a) - fpu_in(b)); }
static inline float fpu_mul(float a, float b) { return fpu_fix(fpu_in(a) * fpu_in(b)); }
static inline float fpu_div(float a, float b) {
    a = fpu_in(a); b = fpu_in(b);
    if (b == 0.0f) { fr r; fr x, y; x.f = a; y.f = b; r.u = ((x.u ^ y.u) & 0x80000000u) | 0x7f7fffffu; return r.f; }
    return fpu_fix(a / b);
}
static inline float fpu_sqrt(float a) { return fpu_fix(sqrtf(fabsf(fpu_in(a)))); }
static inline float fpu_rsqrt(float a, float b) {
    b = fpu_in(b);
    if (b == 0.0f) { fr r, x; x.f = a; r.u = (x.u & 0x80000000u) | 0x7f7fffffu; return r.f; }
    return fpu_fix(fpu_in(a) / sqrtf(fabsf(b)));
}
static inline float fpu_max(float a, float b) { return a > b ? a : b; }
static inline float fpu_min(float a, float b) { return a < b ? a : b; }
static inline u32 fpu_cvtw(float a) {
    fr v; v.f = a;
    if ((v.u & 0x7f800000u) >= 0x4f000000u) return (v.u & 0x80000000u) ? 0x80000000u : 0x7fffffffu;
    return (u32)(s32)a;
}
u32 fpu_cfc1(int r); void fpu_ctc1(int r, u32 v);

/* ---- misc helpers ------------------------------------------------------ */
void ee_div(u32 a, u32 b, int pipe1);
void ee_divu(u32 a, u32 b, int pipe1);
void mmi_exec(u32 w);
void vu0_macro(u32 w);
void vu0_qmfc2(int rt, int vf); void vu0_qmtc2(int rt, int vf);
void vu0_cfc2(int rt, int vi);  void vu0_ctc2(int rt, int vi);
void vu0_lqc2(u32 a, int vf);   void vu0_sqc2(u32 a, int vf);
int  vu0_bc2(void);
int  ee_bc0(void);
u32  cop0_read(int r); void cop0_write(int r, u32 v);
void ee_eret(void); void ee_ei(void); void ee_di(void);

void ee_syscall(u32 pc);
void ee_break(u32 pc, u32 code);
void ee_bad(u32 pc, u32 w);
void ee_fallthrough(u32 pc);
void ee_call(u32 addr);
void ee_bb(u32 a);   /* main.c: basic-block trace (recomp_ee.py --bbtrace) */
void ee_event(void);

#ifdef EE_TRACE
extern u32 ee_trace_buf[1 << 22]; extern u32 ee_trace_pos;
/* EE_COUNT=addr[,addr...] (up to 8): count entries of those functions, printed at exit */
extern u32 ee_count_addr[8], ee_count_hits[8]; extern int ee_count_n;
static inline void ee_fentry(u32 a) {
    ee_trace_buf[ee_trace_pos++ & 0x3fffff] = a;
    for (int i = 0; i < ee_count_n; i++) if (ee_count_addr[i] == a) ee_count_hits[i]++;
}
#define FENTRY(a) ee_fentry(a)
#else
#define FENTRY(a) ((void)0)
#endif
int ee_hook(u32 a);    /* main.c: called at the entry of tools/recomp_ee.py HOOKS functions; nonzero = return at once */
#define CYC(n)  do { ee.cycles += (n); if (ee.cycles >= ee.next_event) ee_event(); } while (0)
#define CYCF(n) CYC(n)

typedef void (*ee_fn)(void);
typedef struct { u32 addr; ee_fn fn; } ee_func_entry;
extern const ee_func_entry ee_table[];
extern const unsigned ee_table_count;
extern const u32 ee_entry;
