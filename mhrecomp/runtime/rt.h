/* Runtime-internal declarations (not needed by generated code). */
#pragma once
#include "ee.h"
#include <stdio.h>

extern int g_log_sys, g_log_io, g_turbo;
extern u64 g_frame;

#define LOG(...) do { fprintf(stderr, __VA_ARGS__); } while (0)

/* hw.c */
void hw_init(void);
extern u32 intc_stat, intc_mask;
void intc_raise(int bit);
void dmac_raise(int ch);
extern u32 dmac_stat;        /* D_STAT: bits 0-9 channel done, 16-25 mask */
extern u32 gs_csr, gs_imr;
extern u64 gs_priv[0x20];    /* 0x12000000.. regs at 0x10 stride */
extern u8 vu0_mem[0x1000], vu0_code[0x1000], vu1_mem[0x4000], vu1_code[0x4000];

/* kernel.c */
void kernel_init(void);
void kernel_vblank(int start);
void kernel_run_dmac_handlers(int ch);
void kernel_run_intc_handlers(int cause);
void thread_reschedule_if_needed(void);
void ee_set_event(u64 at);

/* sif.c */
void sif_init(void);
void spu2_init(void); void spu2_update(void); void spu2_stats(void); void spu2_reset(void);
void spu2_set_param(u32 e, u32 v); u32 spu2_get_param(u32 e); void spu2_set_switch(u32 e, u32 v); u32 spu2_get_switch(u32 e);
void spu2_set_addr(u32 e, u32 a); u32 spu2_get_addr(u32 e); void spu2_core_attr(u32 e, u32 v); u32 spu2_voice_trans(int ch, int mode, u32 iop, u32 spu, u32 size);
extern int g_log_spu;
void spu2_block_start(int ch, u32 addr, u32 size); void spu2_block_stop(int ch); u32 spu2_block_status(int ch);
extern int g_no_audio;
/* gsthread.c: graphics worker (VIF1, VU1, GS, OpenGL) */
extern int g_gs_mt;
void gst_init(void); void gst_sync(void); void gst_stats(void); int gst_busy(void); void gst_apply_csr(void);
void gst_present(const u64 *priv); void gst_gif(const u8 *p, u32 nqw, int path); void gst_post_csr(u32 bits);
void gst_vu1_start(u32 pc); void gif_dma(const u8 *p, u32 nqw, int path);
u32  sif_get_reg(u32 r);
void sif_set_reg(u32 r, u32 v);
int  sif_set_dma(u32 list, int n);
int  sif_dma_stat(int id);
void sif_poll(void);          /* deliver pending IOP->EE packets */
extern u8 iop_ram[2 << 20];

/* gs.c */
void gif_transfer(const u8 *qw, u32 nqw, int path);
u32  vif1_transfer(const u8 *qw, u32 nqw);   /* returns qw taken (stops at an interrupt) */
u32  vif0_transfer(const u8 *qw, u32 nqw);
void dma_resume(int ch);
void gs_vsync(void);

/* ipu.c */
void ipu_init(void);
void ipu_write_cmd(u32 v);
void ipu_write_ctrl(u32 v);
u32  ipu_read32(u32 off);
void ipu_fifo_write(const u8 *qw);
void ipu_fifo_read(u8 *qw);
void ipu_dma_start(int ch);
void ipu_poll(void);
void ipu_stats(void);

/* main.c */
ee_fn ee_lookup(u32 addr);
void call_guest(u32 addr);
