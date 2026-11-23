/* SPU2 output (first step: block-transfer streams only).
 *
 * Samples are produced in emulated time, 48 kHz, from ee.cycles: spu2_update()
 * (called every vblank and when the game asks for a stream position) mixes
 * up to "now" into a ring. A waveOut thread plays the ring; if it runs dry it
 * plays silence, if it overflows the oldest samples are dropped, so audio
 * never changes emulation timing.
 *
 * Block transfer (libsd sceSdBlockTrans through SDRDRV, used by the movies'
 * PCM): a looping buffer in IOP RAM, stereo 16-bit in 512-byte blocks (256
 * left samples, then 256 right). The play position is a function of time
 * since the transfer started, the same number BlockTransStatus reports to the
 * game, so the game refills the half it is told is free.
 *
 * --no-audio (g_no_audio) keeps the mixing but opens no device.
 */
#include "rt.h"
#include <windows.h>
#include <mmsystem.h>
#include <stdlib.h>

#define RATE 48000
#define RING (1 << 15)               /* stereo frames, ~0.68 s */
#define WBUF 1024                    /* frames per waveOut buffer */
#define NWBUF 8

int g_no_audio, g_log_spu;
static u32 ring[RING];               /* L in the low half, R in the high half */
static volatile u32 rd, wr;          /* frame counters; wr written by the emulator, rd by the audio thread */
static u64 mixed;                    /* frames produced since start (emulated time) */
static u64 start_cyc; static int started;

typedef struct { int on; u32 addr, size; u64 start; u16 vol_l, vol_r; } bt_t;
static bt_t bt[2];
u64 n_spu_frames, n_spu_underruns, n_spu_dropped;
static FILE *wav; static u32 wav_frames;
static void wav_header(void); static void wav_close(void);

static inline s16 iop16(u32 a) { s16 v; memcpy(&v, iop_ram + (a & 0x1ffffe), 2); return v; }

/* ---- block transfer streams ---------------------------------------------------- */
void spu2_block_start(int ch, u32 addr, u32 size) {
    spu2_update();
    bt[ch & 1].on = size >= 1024; bt[ch & 1].addr = addr; bt[ch & 1].size = size & ~1023u; bt[ch & 1].start = mixed;
}
void spu2_block_stop(int ch) { spu2_update(); bt[ch & 1].on = 0; }

/* bytes played into the buffer: (half << 24) | address, as BlockTransStatus */
u32 spu2_block_status(int ch) {
    spu2_update();
    bt_t *b = &bt[ch & 1];
    if (!b->on) return 0;
    u64 frames = mixed - b->start;                     /* one frame = 2 bytes of L + 2 of R = 4 bytes */
    u32 off = (u32)((frames * 4) % b->size);
    return (off >= b->size / 2 ? 1u << 24 : 0) | ((b->addr + off) & 0xffffff);
}

/* ---- voices ----------------------------------------------------------------------
 * Register-level model driven by libsd (iop.c): sceSdSetParam/SetSwitch/
 * SetAddr land in spu2_set_param/set_switch/set_addr. Per voice: ADPCM blocks
 * of 16 bytes (shift/filter, flags 1 end, 2 repeat, 4 loop start, 28
 * samples), 4.12 pitch, linear interpolation, ADSR as on the PS1/PS2 SPU
 * (shift/step rates), fixed volumes (sweep modes not modelled yet). Not
 * modelled: reverb, noise, pitch modulation, IRQ address, volume sweeps. */
static u16 sram[1 << 20];            /* 2 MB sound RAM, halfwords */
enum { ENV_OFF, ENV_A, ENV_D, ENV_S, ENV_R };
typedef struct {
    u16 voll, volr, pitch, adsr1, adsr2;
    u32 ssa, lsa, nax;               /* halfword addresses */
    int lsa_written;                 /* software set LSA: loop-start flags no longer move it */
    int phase; s32 env; s32 env_wait;
    u32 counter; int pos; u8 flags; s16 dec[28]; s16 p1, p2; s16 h0, h1;
} voice_t;
typedef struct {
    voice_t v[24];
    u32 endx, vmixl, vmixr, vmixel, vmixer, pmon, non;
    u16 mvoll, mvolr, attr;
    u16 bvoll, bvolr;                /* core-external input (block transfer PCM) volume */
} core_t;
static core_t core[2];
static u64 n_kon, n_sweep_vol;

static const int adpcm_f[5][2] = { { 0, 0 }, { 60, 0 }, { 115, -52 }, { 98, -55 }, { 122, -60 } };

static void decode_block(voice_t *v) {
    u32 a = v->nax & 0xffff8u;
    u16 h = sram[a & 0xfffff];
    int shift = h & 15, filt = (h >> 4) & 7;
    if (filt > 4) filt = 4;
    if (shift > 12) shift = 9;
    v->flags = (u8)(h >> 8);
    if ((v->flags & 4) && !v->lsa_written) v->lsa = a;
    for (int i = 0; i < 28; i++) {
        u16 w = sram[(a + 1 + i / 4) & 0xfffff];
        s32 s = (s16)(((w >> ((i & 3) * 4)) & 15) << 12) >> shift;
        s += (v->p1 * adpcm_f[filt][0] + v->p2 * adpcm_f[filt][1] + 32) >> 6;
        if (s > 32767) s = 32767; if (s < -32768) s = -32768;
        v->p2 = v->p1; v->p1 = (s16)s;
        v->dec[i] = (s16)s;
    }
    v->pos = 0;
}

static void key_on(core_t *c, int n) {
    voice_t *v = &c->v[n];
    v->nax = v->ssa; v->p1 = v->p2 = v->h0 = v->h1 = 0; v->counter = 0;
    v->phase = ENV_A; v->env = 0; v->env_wait = 0;
    c->endx &= ~(1u << n);
    decode_block(v);
    n_kon++;
}

/* one sample further into the voice; block flags act when a block is used up */
static void next_sample(core_t *c, int n) {
    voice_t *v = &c->v[n];
    v->h1 = v->h0; v->h0 = v->dec[v->pos];
    if (++v->pos < 28) return;
    if (v->flags & 1) {
        c->endx |= 1u << n;
        if (v->flags & 2) v->nax = v->lsa;
        else { v->phase = ENV_OFF; v->env = 0; v->nax += 8; }
    } else v->nax += 8;
    v->nax &= 0xfffff;
    decode_block(v);
}

/* PS1/PS2 SPU envelope: rate = 7 bits, shift = rate >> 2, step from rate & 3 */
static void env_tick(voice_t *v) {
    int rate, ex, dec;
    switch (v->phase) {
    case ENV_A: rate = (v->adsr1 >> 8) & 0x7f; ex = v->adsr1 >> 15; dec = 0; break;
    case ENV_D: rate = ((v->adsr1 >> 4) & 15) << 2; ex = 1; dec = 1; break;
    case ENV_S: rate = (v->adsr2 >> 6) & 0x7f; ex = v->adsr2 >> 15; dec = (v->adsr2 >> 14) & 1; break;
    case ENV_R: rate = (v->adsr2 & 31) << 2; ex = (v->adsr2 >> 5) & 1; dec = 1; break;
    default: return;
    }
    if (--v->env_wait > 0) return;
    int shift = rate >> 2, step = dec ? -8 + (rate & 3) : 7 - (rate & 3);
    s32 cycles = 1 << (shift > 11 ? shift - 11 : 0);
    s32 stepv = step << (shift < 11 ? 11 - shift : 0);
    if (ex && !dec && v->env > 0x6000) cycles *= 4;
    if (ex && dec) stepv = (s32)((s64)stepv * v->env >> 15);
    v->env_wait = cycles;
    v->env += stepv;
    if (v->env > 0x7fff) v->env = 0x7fff;
    if (v->env < 0) v->env = 0;
    switch (v->phase) {
    case ENV_A: if (v->env >= 0x7fff) v->phase = ENV_D; break;
    case ENV_D: if (v->env <= (((v->adsr1 & 15) + 1) << 11)) v->phase = ENV_S; break;
    case ENV_R: if (v->env <= 0) v->phase = ENV_OFF; break;
    }
}

static s32 vol15(u16 r) {             /* fixed volume: 15-bit signed in bits 0-14 */
    if (r & 0x8000) { n_sweep_vol++; return 0x7fff; }
    return (s32)(s16)(r << 1);
}

static void mix_voices(s32 *outl, s32 *outr) {
    for (int ci = 0; ci < 2; ci++) {
        core_t *c = &core[ci];
        s32 l = 0, r = 0;
        for (int n = 0; n < 24; n++) {
            voice_t *v = &c->v[n];
            if (v->phase == ENV_OFF) continue;
            u32 p = v->pitch > 0x3fff ? 0x3fff : v->pitch;
            v->counter += p;
            while (v->counter >= 0x1000) { v->counter -= 0x1000; next_sample(c, n); }
            s32 s = v->h1 + (((v->h0 - v->h1) * (s32)v->counter) >> 12);
            env_tick(v);
            s = (s * v->env) >> 15;
            if (c->vmixl & (1u << n)) l += (s * vol15(v->voll)) >> 15;
            if (c->vmixr & (1u << n)) r += (s * vol15(v->volr)) >> 15;
        }
        *outl += (l * vol15(c->mvoll)) >> 15;
        *outr += (r * vol15(c->mvolr)) >> 15;
    }
}

/* ---- libsd entry points (entry: core bit 0, voice bits 1-5, register bits 8-15) ---- */
void spu2_set_param(u32 entry, u32 val) {
    spu2_update();
    core_t *c = &core[entry & 1]; voice_t *v = &c->v[((entry >> 1) & 31) % 24];
    switch ((entry >> 8) & 0xff) {
    case 0x00: v->voll = (u16)val; break;
    case 0x01: v->volr = (u16)val; break;
    case 0x02: v->pitch = (u16)val; break;
    case 0x03: v->adsr1 = (u16)val; break;
    case 0x04: v->adsr2 = (u16)val; break;
    case 0x09: c->mvoll = (u16)val; break;
    case 0x0a: c->mvolr = (u16)val; break;
    case 0x0f: c->bvoll = (u16)val; break;   /* BVOLL/R: the EE sets these over SDRDRV around movies */
    case 0x10: c->bvolr = (u16)val; break;
    default: break;                  /* effect/aux volumes etc. */
    }
    if (g_log_spu > 1) LOG("spu2: param %04x = %04x\n", entry, val);
}
u32 spu2_get_param(u32 entry) {
    spu2_update();
    core_t *c = &core[entry & 1]; voice_t *v = &c->v[((entry >> 1) & 31) % 24];
    switch ((entry >> 8) & 0xff) {
    case 0x00: return v->voll; case 0x01: return v->volr; case 0x02: return v->pitch;
    case 0x03: return v->adsr1; case 0x04: return v->adsr2;
    case 0x05: return (u32)v->env;                        /* ENVX */
    case 0x06: return (u32)(v->env * vol15(v->voll) >> 15) & 0xffff;
    case 0x07: return (u32)(v->env * vol15(v->volr) >> 15) & 0xffff;
    case 0x09: return c->mvoll; case 0x0a: return c->mvolr;
    case 0x0f: return c->bvoll; case 0x10: return c->bvolr;
    }
    return 0;
}
void spu2_set_switch(u32 entry, u32 val) {
    spu2_update();
    core_t *c = &core[entry & 1]; val &= 0xffffff;
    switch ((entry >> 8) & 0xff) {
    case 0x13: c->pmon = val; break;
    case 0x14: c->non = val; break;
    case 0x15: for (int n = 0; n < 24; n++) if (val & (1u << n)) key_on(c, n); break;
    case 0x16: for (int n = 0; n < 24; n++) if ((val & (1u << n)) && c->v[n].phase != ENV_OFF) { c->v[n].phase = ENV_R; c->v[n].env_wait = 0; } break;
    case 0x17: c->endx = val; break;
    case 0x18: c->vmixl = val; break;
    case 0x19: c->vmixel = val; break;
    case 0x1a: c->vmixr = val; break;
    case 0x1b: c->vmixer = val; break;
    }
    if (g_log_spu) LOG("spu2: switch %04x = %06x\n", entry, val);
}
u32 spu2_get_switch(u32 entry) {
    spu2_update();
    core_t *c = &core[entry & 1];
    switch ((entry >> 8) & 0xff) {
    case 0x13: return c->pmon; case 0x14: return c->non;
    case 0x17: return c->endx;
    case 0x18: return c->vmixl; case 0x19: return c->vmixel; case 0x1a: return c->vmixr; case 0x1b: return c->vmixer;
    }
    return 0;
}
/* byte addresses */
void spu2_set_addr(u32 entry, u32 addr) {
    spu2_update();
    core_t *c = &core[entry & 1]; voice_t *v = &c->v[((entry >> 1) & 31) % 24];
    u32 h = (addr >> 1) & 0xfffff;
    switch ((entry >> 8) & 0xff) {
    case 0x20: v->ssa = h; break;
    case 0x21: v->lsa = h; v->lsa_written = 1; break;
    case 0x22: v->nax = h; break;
    default: break;                  /* ESA/EEA/TSA/IRQA */
    }
    if (g_log_spu > 1) LOG("spu2: addr %04x = %06x\n", entry, addr);
}
u32 spu2_get_addr(u32 entry) {
    spu2_update();
    core_t *c = &core[entry & 1]; voice_t *v = &c->v[((entry >> 1) & 31) % 24];
    switch ((entry >> 8) & 0xff) {
    case 0x20: return v->ssa << 1; case 0x21: return v->lsa << 1; case 0x22: return v->nax << 1;
    }
    return 0;
}
void spu2_core_attr(u32 entry, u32 val) { spu2_update(); core[entry & 1].attr = (u16)val; }
/* IOP RAM -> sound RAM (write) or back (read), done at once */
u32 spu2_voice_trans(int ch, int mode, u32 iop_addr, u32 spu_addr, u32 size) {
    spu2_update();
    u32 h = (spu_addr >> 1) & 0xfffff;
    for (u32 k = 0; k + 1 < size; k += 2) {
        u16 *p = &sram[(h + k / 2) & 0xfffff];
        if (mode & 1) memcpy(iop_ram + ((iop_addr + k) & 0x1ffffe), p, 2);
        else memcpy(p, iop_ram + ((iop_addr + k) & 0x1ffffe), 2);
    }
    if (g_log_spu) LOG("spu2: voice trans ch%d mode %x iop %08x -> spu %06x size %x\n", ch, mode, iop_addr, spu_addr, size);
    return size;
}
void spu2_reset(void) {
    spu2_update();
    memset(core, 0, sizeof core);
    for (int c = 0; c < 2; c++) {
        for (int n = 0; n < 24; n++) core[c].v[n].phase = ENV_OFF;
        /* sceSdInit leaves every voice in the dry mix; ZAUDIO never writes VMIX */
        core[c].vmixl = core[c].vmixr = 0xffffff;
    }
}

static void mix_frame(s32 *l, s32 *r) {
    mix_voices(l, r);
    for (int c = 0; c < 2; c++) {
        bt_t *b = &bt[c];
        if (!b->on) continue;
        u64 f = mixed - b->start;
        u32 bytes = (u32)((f * 4) % b->size);           /* position in the stereo stream */
        u32 blk = bytes / 1024, in = (bytes % 1024) / 2; /* 1024 bytes = 256 frames: 512 L + 512 R */
        u32 base = b->addr + blk * 1024 + (in / 2) * 2;
        *l += (iop16(base) * (s32)(s16)core[c].bvoll) >> 15;      /* BVOL: plain 16-bit signed, no sweep bits */
        *r += (iop16(base + 512) * (s32)(s16)core[c].bvolr) >> 15;
    }
}

int g_spu_volume = -1;
static void spu2_volume_init(void) {
    const char *e = getenv("SPU_VOLUME"); int v = e ? atoi(e) : 100;
    g_spu_volume = v < 0 ? 0 : v > 100 ? 100 : v;
}

/* mix up to the current emulated time */
void spu2_update(void) {
    if (!started) { started = 1; start_cyc = ee.cycles; }
    u64 target = (ee.cycles - start_cyc) * RATE / 294912000ull;
    if (target - mixed > RATE) mixed = target - RATE;   /* a long stall: skip, do not mix a second of catch-up */
    while (mixed < target) {
        s32 l = 0, r = 0;
        mix_frame(&l, &r);
        {   /* SPU_VOLUME=<percent> (default 100, mh2.ini volume): output volume, e.g. 50 for
               test runs; the in-game menu (menu.c) changes g_spu_volume */
            if (g_spu_volume < 0) spu2_volume_init();
            int vol = g_spu_volume;
            if (vol != 100) { l = l * vol / 100; r = r * vol / 100; }
        }
        if (l > 32767) l = 32767; if (l < -32768) l = -32768;
        if (r > 32767) r = 32767; if (r < -32768) r = -32768;
        u32 fr = (u16)l | ((u32)(u16)r << 16);
        u32 w = wr;
        if (w - rd < RING) { ring[w & (RING - 1)] = fr; wr = w + 1; }
        if (wav) { fwrite(&fr, 4, 1, wav); wav_frames++; }
        mixed++; n_spu_frames++;
    }
}

/* SPU_WAV=<file>: everything mixed, in emulated time (works in --turbo) */
static void wav_header(void) {
    u32 data = wav_frames * 4;
    u8 h[44] = "RIFF"; u32 v;
    v = 36 + data; memcpy(h + 4, &v, 4); memcpy(h + 8, "WAVEfmt ", 8);
    v = 16; memcpy(h + 16, &v, 4);
    u16 s = 1; memcpy(h + 20, &s, 2); s = 2; memcpy(h + 22, &s, 2);
    v = RATE; memcpy(h + 24, &v, 4); v = RATE * 4; memcpy(h + 28, &v, 4);
    s = 4; memcpy(h + 32, &s, 2); s = 16; memcpy(h + 34, &s, 2);
    memcpy(h + 36, "data", 4); memcpy(h + 40, &data, 4);
    fseek(wav, 0, SEEK_SET); fwrite(h, 1, 44, wav); fseek(wav, 0, SEEK_END);
}
static void wav_close(void) { if (wav) { wav_header(); fclose(wav); wav = 0; } }

/* ---- host output: waveOut, a thread refills buffers as they finish -------------- */
static HWAVEOUT wo;
static WAVEHDR hdr[NWBUF];
static u32 wbuf[NWBUF][WBUF];
static HANDLE wev;

/* Buffers carry whatever is ready (256..WBUF frames). Silence is queued only
 * when fewer than two buffers are still playing, i.e. the device would
 * really run dry; otherwise the thread waits for the emulator to mix more. */
#define MINF 256
static DWORD WINAPI audio_thread(void *p) {
    (void)p;
    for (;;) {
        WaitForSingleObject(wev, 5);
        int pending = 0;
        for (int k = 0; k < NWBUF; k++) if (!(hdr[k].dwFlags & WHDR_DONE)) pending++;
        for (int k = 0; k < NWBUF; k++) {
            if (!(hdr[k].dwFlags & WHDR_DONE)) continue;
            u32 avail = wr - rd, n = avail < WBUF ? avail : WBUF;
            if (n < MINF) {
                if (pending >= 2) break;                       /* still playing: wait for more */
                memset(wbuf[k], 0, MINF * 4); n = 0;           /* about to run dry: a little silence */
                hdr[k].dwBufferLength = MINF * 4;
                extern volatile int g_menu_open;
                if (g_frame > 120 && !g_menu_open) { n_spu_underruns++; if (g_log_sys && n_spu_underruns <= 60) LOG("spu2: underrun at frame %llu\n", (unsigned long long)g_frame); }
            } else {
                /* With vsync pacing the game runs at the display's 60.00 Hz instead
                 * of 59.94, so it mixes 48048 frames a second for a 48000 device.
                 * While more than ~64 ms is waiting, skip 1 frame in 256 (inaudible). */
                u32 r0 = rd, src = 0, drop = avail > 3 * WBUF;
                for (u32 i = 0; i < n; i++, src++) {
                    if (drop && i && (i & 255) == 0 && src + 1 < avail) { src++; n_spu_dropped++; }
                    wbuf[k][i] = ring[(r0 + src) & (RING - 1)];
                }
                rd = r0 + src;
                hdr[k].dwBufferLength = n * 4;
            }
            hdr[k].dwFlags &= ~WHDR_DONE;
            waveOutWrite(wo, &hdr[k], sizeof hdr[k]);
            pending++;
        }
    }
    return 0;
}

void spu2_init(void) {
    const char *w = getenv("SPU_WAV");
    if (w && (wav = fopen(w, "wb"))) { u8 z[44] = { 0 }; fwrite(z, 1, 44, wav); atexit(wav_close); LOG("spu2: writing %s\n", w); }
    if (g_no_audio || g_turbo) { LOG("spu2: no audio output (%s)\n", g_turbo ? "--turbo" : "--no-audio"); return; }
    WAVEFORMATEX f = { WAVE_FORMAT_PCM, 2, RATE, RATE * 4, 4, 16, 0 };
    wev = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (waveOutOpen(&wo, WAVE_MAPPER, &f, (DWORD_PTR)wev, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
        LOG("spu2: cannot open audio output\n"); wo = 0; return;
    }
    for (int k = 0; k < NWBUF; k++) {
        hdr[k].lpData = (LPSTR)wbuf[k]; hdr[k].dwBufferLength = WBUF * 4;
        waveOutPrepareHeader(wo, &hdr[k], sizeof hdr[k]);
        hdr[k].dwFlags |= WHDR_DONE;
    }
    CreateThread(NULL, 0, audio_thread, NULL, 0, NULL);
    SetEvent(wev);
    LOG("spu2: audio out 48 kHz stereo (waveOut, %d x %d frames)\n", NWBUF, WBUF);
}

void spu2_stats(void) {
    LOG("spu2: %llu key-ons, %llu volume reads in sweep mode (not modelled)\n", (unsigned long long)n_kon, (unsigned long long)n_sweep_vol);
    LOG("spu2: %llu frames mixed, %llu output underruns, %llu frames dropped for drift\n", (unsigned long long)n_spu_frames,
        (unsigned long long)n_spu_underruns, (unsigned long long)n_spu_dropped);
}
