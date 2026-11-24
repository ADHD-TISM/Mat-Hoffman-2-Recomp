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
#include "vu_core.h"

vf128 vu1_vf[32], vu1_acc;
u32 vu1_vi[32];
fr vu1_q, vu1_i;
vu_t VU[2];
static int vu_ready;
int vu_no_kick;          /* VU_CHECK: the reference run must not send GIF data */

static void vu_setup(void) {
    if (vu_ready) return;
    vu_ready = 1;
    VU[0].vf = ee.vf; VU[0].vi = ee.vi; VU[0].acc = &ee.vacc; VU[0].q = &ee.vq; VU[0].i = &ee.vi_reg;
    VU[0].mem = vu0_mem; VU[0].qmask = 0xff; VU[0].code = vu0_code; VU[0].cmask = 0xfff;
    VU[1].vf = vu1_vf; VU[1].vi = vu1_vi; VU[1].acc = &vu1_acc; VU[1].q = &vu1_q; VU[1].i = &vu1_i;
    VU[1].mem = vu1_mem; VU[1].qmask = 0x3ff; VU[1].code = vu1_code; VU[1].cmask = 0x3fff; VU[1].rf1 = 1;
    vu1_vf[0].f[3] = 1.0f;
}

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

#undef xgkick
void vu_xgkick(vu_t *v, u32 addr) {
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
#define xgkick vu_xgkick

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

static void vu_interp_state(vu_t *v, u32 pc, int n, int end_after, int branch_pending, u32 branch_to);
static void vu_interp(vu_t *v, u32 pc, int n) { vu_interp_state(v, pc, n, -1, 0, 0); }
static u64 n_rec_fallback;
/* a recompiled program jumped to a pair the recompiler did not generate */
void vu_rec_fallback(vu_t *v, u32 next, int ea, int bp, u32 bt) {
    static u16 seen[64]; static int nseen;
    n_rec_fallback++;
    u16 p = (u16)(next & 0x3ff8); int k;
    for (k = 0; k < nseen && seen[k] != p; k++) {}
    if (k == nseen && nseen < 64) { seen[nseen++] = p; LOG("vu: recompiled VU1 code jumped to %04x, not generated (add it as an entry)\n", p); }
    vu_interp_state(v, next, 1, ea, bp, bt);
}
/* a run starts at a pc that was not recorded as an entry (VU_RECORD): the
 * clamp analysis did not start from it, so it is interpreted */
void vu_rec_new_entry(vu_t *v, u32 pc) {
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
