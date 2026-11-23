/* IOP (R3000) side: state and helpers used by recompiled IRX modules. */
#pragma once
#include "ee.h"

typedef struct { u32 r[32], hi, lo; u32 spin; } iop_state;
extern iop_state iop;
#define IR iop.r

extern u8 iop_ram[2 << 20];
u32  iop_io_read(u32 a, int size);
void iop_io_write(u32 a, u32 v, int size);

static inline int iop_is_ram(u32 a) { return (a & 0x1fffffffu) < 0x200000u; }
static inline u8  ir8(u32 a)  { return iop_is_ram(a) ? iop_ram[a & 0x1fffff] : (u8)iop_io_read(a, 1); }
static inline u16 ir16(u32 a) { u16 v; if (!iop_is_ram(a)) return (u16)iop_io_read(a, 2); memcpy(&v, iop_ram + (a & 0x1ffffe), 2); return v; }
static inline u32 ir32(u32 a) { u32 v; if (!iop_is_ram(a)) return iop_io_read(a, 4); memcpy(&v, iop_ram + (a & 0x1ffffc), 4); return v; }
static inline void iw8(u32 a, u8 v)   { if (iop_is_ram(a)) iop_ram[a & 0x1fffff] = v; else iop_io_write(a, v, 1); }
static inline void iw16(u32 a, u16 v) { if (iop_is_ram(a)) memcpy(iop_ram + (a & 0x1ffffe), &v, 2); else iop_io_write(a, v, 2); }
static inline void iw32(u32 a, u32 v) { if (iop_is_ram(a)) memcpy(iop_ram + (a & 0x1ffffc), &v, 4); else iop_io_write(a, v, 4); }
static inline u32 ilwl(u32 a, u32 rt) { u32 sh = (a & 3) * 8, w = ir32(a & ~3u); return (rt & (0x00ffffffu >> sh)) | (w << (24 - sh)); }
static inline u32 ilwr(u32 a, u32 rt) { u32 sh = (a & 3) * 8, w = ir32(a & ~3u); return (rt & ~(0xffffffffu >> sh)) | (w >> sh); }
static inline void iswl(u32 a, u32 rt) { u32 sh = (a & 3) * 8, w = ir32(a & ~3u); iw32(a & ~3u, (w & ~(0xffffffffu >> (24 - sh))) | (rt >> (24 - sh))); }
static inline void iswr(u32 a, u32 rt) { u32 sh = (a & 3) * 8, w = ir32(a & ~3u); iw32(a & ~3u, (w & ~(0xffffffffu << sh)) | (rt << sh)); }

void iop_import(const char *lib, int index);
void iop_call(u32 addr);
void iop_bad(u32 pc, u32 w);
void iop_fallthrough(u32 pc);
void iop_syscall(u32 pc);
void iop_break(u32 pc);
void iop_div(u32 a, u32 b);
void iop_divu(u32 a, u32 b);
u32  iop_cop0_read(int r);
void iop_cop0_write(int r, u32 v);
void iop_spin_yield(void);
#define ILOOP() do { if (++iop.spin > 2000000u) iop_spin_yield(); } while (0)

typedef struct { u32 addr; void (*fn)(void); } iop_func_entry;
typedef struct {
    const char *name; u32 base, size; const u8 *image; u32 entry;
    const iop_func_entry *table; unsigned count; u32 gp;
} iop_module;

/* interface to the SIF layer (sif.c) */
int  iop_load_module(const char *path);            /* 1 if run as recompiled code */
int  iop_server_lookup(u32 sid, u32 *buf);         /* 1 if a recompiled module registered sid */
int  iop_is_lle_sid(u32 sid);
void iop_rpc_request(u32 sid, u32 fn, u32 size, u32 ee_recv, u32 recv_size, const u32 *echo);
void iop_rpc_end(u32 client, u32 cid, u32 server, u32 buff);
void iop_run(void);
void iop_reset(void);
void sif_send_to_ee(u32 cid, const u32 *words, int nwords);
