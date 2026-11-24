"""Static recompiler: PS2 EE ELF (MIPS R5900) -> C.

usage: python recomp_ee.py <elf> <out_dir> [--extra FILE] [--fullcost HEX]

Same model as the PS1 recompiler (dmrecomp/tools/recomp.py): every entry point
becomes `void f_XXXXXXXX(void)` holding everything reachable by local control
flow; jal is a native call, jr ra a return, jr reg goes through recovered
switch tables or the runtime dispatcher (ee_call). Guest state lives in the
global `ee` struct (128-bit GPRs, FPU, HI/LO pairs, SA). MMI instructions and
VU0 macro instructions are handed to interpreter helpers in the runtime.
"""
import os, struct, sys

def s16(x): return x - 0x10000 if x & 0x8000 else x
NL = chr(10)

JR_RA = bytes([8, 0, 0xe0, 3])   # jr ra, little endian

class Elf:
    def __init__(self, path):
        d = open(path, 'rb').read()
        assert d[:4] == b'\x7fELF'
        self.pc0, = struct.unpack_from('<I', d, 0x18)
        phoff, = struct.unpack_from('<I', d, 0x1c)
        shoff, = struct.unpack_from('<I', d, 0x20)
        phnum, = struct.unpack_from('<H', d, 0x2c)
        shnum, shstr = struct.unpack_from('<HH', d, 0x30)
        self.segs = []
        for k in range(phnum):
            typ, off, va, pa, fsz, msz = struct.unpack_from('<6I', d, phoff + 32 * k)
            if typ == 1: self.segs.append((va, d[off:off + fsz], msz))
        # executable ranges from section headers (.text only; .vutext is VU code)
        secs = []
        for k in range(shnum):
            nm, typ, fl, addr, off, sz = struct.unpack_from('<6I', d, shoff + 40 * k)
            secs.append((nm, typ, fl, addr, off, sz))
        so = secs[shstr][4]
        def name(n): return d[so + n:d.index(b'\0', so + n)].decode()
        self.text = None
        self.sections = []
        for nm, typ, fl, addr, off, sz in secs:
            self.sections.append((name(nm), addr, sz, typ))
            if name(nm) == '.text': self.text = (addr, addr + sz)
        self.base = self.segs[0][0]
        self.data = self.segs[0][1]
        self.end = self.base + len(self.data)
        if self.text is None:
            # stripped section names (Mat Hoffman 2): code runs from the segment
            # start to the last `jr ra` before the first 64 KB block without one
            o = 0
            while o < len(self.data) and JR_RA in self.data[o:o + 0x10000]: o += 0x10000
            while o > 0 and self.data[o - 4:o] != JR_RA: o -= 4
            self.text = (self.base, self.base + o + 4)
    def word(self, a):
        o = a - self.base
        if o < 0 or o + 4 > len(self.data): return None
        return struct.unpack_from('<I', self.data, o)[0]
    more = []   # extra code ranges outside .text (--code A-B), e.g. static initialisers
    def inside(self, a):
        if a & 3: return False
        if self.text[0] <= a < self.text[1]: return True
        for lo, hi in self.more:
            if lo <= a < hi: return True
        return False

class Insn:
    __slots__ = ('a','w','op','rs','rt','rd','sa','fn','imm','simm','tgt')
    def __init__(self, a, w):
        self.a, self.w = a, w
        self.op = w >> 26; self.rs = (w >> 21) & 31; self.rt = (w >> 16) & 31
        self.rd = (w >> 11) & 31; self.sa = (w >> 6) & 31; self.fn = w & 63
        self.imm = w & 0xffff; self.simm = s16(self.imm)
        self.tgt = ((a + 4) & 0xf0000000) | ((w & 0x3ffffff) << 2)

# --- classification ---------------------------------------------------------
def kind(i):
    """None, 'b', 'bl' (likely), 'bal', 'ball', 'j', 'jal', 'jr', 'jalr'"""
    op = i.op
    if op == 0:
        if i.fn == 8: return 'jr'
        if i.fn == 9: return 'jalr'
        return None
    if op == 1:
        rt = i.rt
        if rt in (0, 1): return 'b'
        if rt in (2, 3): return 'bl'
        if rt in (0x10, 0x11): return 'bal'
        if rt in (0x12, 0x13): return 'ball'
        return None
    if op == 2: return 'j'
    if op == 3: return 'jal'
    if 4 <= op <= 7: return 'b'
    if 0x14 <= op <= 0x17: return 'bl'
    if op in (0x10, 0x11, 0x12) and i.rs == 8:
        return 'bl' if i.rt & 2 else 'b'
    return None

def btarget(i): return (i.a + 4 + (i.simm << 2)) & 0xffffffff

VALID_OPS = set(range(0, 0x40)) - {0x13, 0x1d}
VALID_OPS -= {0x13, 0x34, 0x35, 0x38, 0x3a, 0x3b, 0x3c, 0x3d}   # unused on EE
VALID_OPS -= {0x30, 0x32}                                        # ll/lwc2 not on EE
VALID_SPECIAL = {0,2,3,4,6,7,8,9,10,11,12,13,15,16,17,18,19,20,22,23,24,25,26,27,
                 32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,54,
                 56,58,59,60,62,63}

def valid(i):
    if i.op not in VALID_OPS: return False
    if i.op == 0 and i.fn not in VALID_SPECIAL: return False
    if i.op == 0x11 and i.rs not in (0, 2, 4, 6, 8, 0x10, 0x14): return False
    return True

# --- analysis ---------------------------------------------------------------
class Func:
    def __init__(self, entry):
        self.entry = entry
        self.addrs = set(); self.labels = set(); self.tables = {}
        self.calls = set(); self.ptrs = set()

def is_addu(w): return (w >> 26) == 0 and (w & 0x7ff) in (0x21, 0x2d)

def find_jump_table(exe, i):
    reg = i.rs
    lw = None; n = None; luis = []; addius = {}; idx = None
    a = i.a - 4
    for _ in range(24):
        w = exe.word(a)
        if w is None: break
        j = Insn(a, w)
        if lw is None:
            if j.op == 35 and j.rt == reg: lw = j
        else:
            if j.op == 15: luis.append(j)
            elif j.op in (9, 0x19) and j.rs == j.rt: addius.setdefault(j.rt, j)
            elif j.op == 0 and (w & 63) == 0 and idx is None: idx = (w >> 16) & 31   # sll rd, rt, 2: rt is the case index
            # the bound must test the case index: at 0x14e3e4 an unrelated loop's
            # sltiu (bound 2) was taken and 247 of 249 cases were lost
            elif j.op == 11 and n is None and (idx is None or j.rs == idx): n = j.imm
        a -= 4
    if lw is None: return None
    infer = not n          # no sltiu bound in front (0x189e80, 0x195bac): count the table itself
    if infer: n = 256
    def table(base):
        out = []
        for idx in range(n):
            t = exe.word(base + 4 * idx)
            if t is None or not exe.inside(t) or abs(t - i.a) > (0x4000 if infer else 0x40000) or t & 3: break
            out.append(t)
        if infer: return out if len(out) >= 2 else None
        return out if len(out) == n else None
    best = None
    for l in luis:
        lo = addius[l.rt].simm if l.rt in addius and addius[l.rt].a > l.a else 0
        t = table(((l.imm << 16) + lo + lw.simm) & 0xffffffff)
        if t and (best is None or len(t) > len(best)): best = t
    if best: return best
    cands = {lw.rs}
    a = lw.a - 4
    for _ in range(6):
        w = exe.word(a)
        if w is None: break
        if is_addu(w) and ((w >> 11) & 31) == lw.rs:
            cands |= {(w >> 21) & 31, (w >> 16) & 31}; break
        a -= 4
    a = lw.a - 4
    for _ in range(600):
        w = exe.word(a)
        if w is None: break
        j = Insn(a, w)
        if j.op in (9, 0x19) and j.rt in cands and j.rs not in (0, 29):
            # the base register's lui can be far up the function (a callee-saved
            # s-register loaded once, e.g. 0x190110 addiu v1,s7,-3728): walk back
            # to it, giving up if something else writes that register first
            for b in range(a - 4, a - 4 * 2000, -4):
                w2 = exe.word(b)
                if w2 is None: break
                l = Insn(b, w2)
                if l.op == 15 and l.rt == j.rs:
                    t = table(((l.imm << 16) + j.simm + lw.simm) & 0xffffffff)
                    if t: return t
                    break
                wr = l.rd if l.op == 0 else l.rt
                if wr == j.rs and l.op not in (40, 41, 43, 63, 0x1f, 4, 5, 1, 2, 3): break
        a -= 4
    return None

WEAK = set()
UNRESOLVED_JR = set()   # jr reg (not ra) with no table found: run through ee_call at run time

def analyse(exe, entry, entries):
    f = Func(entry)
    work = [entry]
    while work:
        a = work.pop()
        while True:
            if a in f.addrs or not exe.inside(a): break
            w = exe.word(a); i = Insn(a, w)
            f.addrs.add(a)
            if not valid(i): break
            if i.op == 15:
                # lui rX, hi ... addiu/ori rY, rX, lo within the next few instructions
                for k in range(1, 12):
                    nw = exe.word(a + 4 * k)
                    if nw is None: break
                    n = Insn(a + 4 * k, nw)
                    if n.op in (9, 13, 0x19) and n.rs == i.rt:
                        p = (i.imm << 16) + (n.simm if n.op != 13 else n.imm)
                        f.ptrs.add(p & 0xffffffff)
                    if kind(n) in ('j', 'jr'): break
                    wr = n.rd if n.op == 0 else n.rt
                    if n.op not in (40, 41, 43, 63, 0x1f, 4, 5, 1) and wr == i.rt and not (n.op in (9, 13, 0x19) and n.rs == i.rt):
                        break
            k = kind(i)
            if k is None:
                a += 4; continue
            f.addrs.add(a + 4)
            if k in ('b', 'bl'):
                t = btarget(i); f.labels.add(t); work.append(t)
                if i.op == 4 and i.rs == 0 and i.rt == 0: break
                if i.op == 1 and i.rs == 0 and i.rt == 1: break   # bgez zero
                a += 8; continue
            if k in ('bal', 'ball'):
                f.calls.add(btarget(i)); a += 8; continue
            if k == 'jal':
                f.calls.add(i.tgt); a += 8; continue
            if k == 'jalr':
                a += 8; continue
            if k == 'j':
                t = i.tgt
                if t != entry and t in entries and t not in WEAK: break
                f.labels.add(t); work.append(t); break
            if k == 'jr':
                if i.rs != 31:
                    tab = find_jump_table(exe, i)
                    if tab:
                        f.tables[a] = tab
                        for t in tab: f.labels.add(t); work.append(t)
                    else: UNRESOLVED_JR.add(a)
                break
    return f

def plausible_start(exe, t):
    if not exe.inside(t): return False
    w = exe.word(t)
    if w is None or not valid(Insn(t, w)): return False
    if t == exe.text[0]: return True
    if exe.word(t - 8) == 0x03e00008: return True
    v = exe.word(t - 8)
    if v is not None and (v >> 26) == 2: return True     # after a tail jump `j fn; <delay>`
    if (w >> 16) in (0x27bd, 0x67bd) and (w & 0x8000): return True
    # after zero padding that follows a `jr ra` or a `j` (C++ thunks: `j fn; nop`
    # padded to 16 bytes, reached only through vtables)
    p = t - 4
    while p >= exe.text[0] and exe.word(p) == 0 and t - p <= 32: p -= 4
    if p < t - 4:
        for q in (p, p - 4):
            v = exe.word(q)
            if v is not None and (v == 0x03e00008 or (v >> 26) == 2): return True
    return False

def discover(exe, extra):
    entries = set(e for e in extra if exe.inside(e))
    entries.add(exe.pc0)
    funcs = {}
    for o in range(0, len(exe.data) - 3, 4):
        v = struct.unpack_from('<I', exe.data, o)[0]
        if exe.inside(v) and plausible_start(exe, v): entries.add(v)
    swept = set()
    a = exe.text[0] + 8
    while a < exe.text[1]:
        # a function ends with `jr ra` or with a `j` tail jump (+ delay slot); what
        # follows is the next function. Without the `j` case 64 functions reached only
        # through pointers were missing or existed only as labels inside the function
        # before them ("call to unknown address" in play). A `j` in the middle of a
        # function gives a spare entry, which is harmless (WEAK below).
        pw = exe.word(a - 8)
        if pw == 0x03e00008 or (pw is not None and (pw >> 26) == 2):
            t = a
            while exe.word(t) == 0 and t < exe.text[1] - 4: t += 4
            w = exe.word(t)
            if w is not None and exe.inside(t) and valid(Insn(t, w)) and t not in entries:
                entries.add(t); swept.add(t)
        a += 4
    changed = True
    while changed:
        changed = False
        for e in sorted(entries):
            if e in funcs: continue
            f = analyse(exe, e, entries); funcs[e] = f
            new = set(c for c in f.calls if exe.inside(c))
            new |= set(p for p in f.ptrs if plausible_start(exe, p))
            if new - entries: entries |= new; changed = True
    called = set()
    for f in funcs.values(): called |= f.calls
    WEAK.update(swept - called)
    for e in sorted(entries): funcs[e] = analyse(exe, e, entries)
    return funcs

# --- code generation --------------------------------------------------------
def D(n): return '0ull' if n == 0 else f'GD({n})'       # 64-bit read (typed zero: `0 << 56` on an int is undefined and clang drops the code)
def W(n): return '0' if n == 0 else f'GW({n})'          # low 32 bits
def SX(e): return f'(u64)(s64)(s32)({e})'

def setd(r, expr): return f'GD({r}) = {expr};' if r else ''

def fop(i):
    """COP1 instruction text."""
    rs, ft, fs, fd, fn = i.rs, i.rt, i.rd, i.sa, i.fn
    F = lambda n: f'ee.fpr[{n}].f'
    U = lambda n: f'ee.fpr[{n}].u'
    if rs == 0: return setd(i.rt, SX(U(fs)))
    if rs == 2: return setd(i.rt, SX(f'fpu_cfc1({fs})'))
    if rs == 4: return f'{U(fs)} = {W(i.rt)};'
    if rs == 6: return f'fpu_ctc1({fs}, {W(i.rt)});'
    if rs == 0x14:
        if fn == 0x20: return f'{F(fd)} = (float)(s32){U(fs)};'
        return f'ee_bad(0x{i.a:08x}, 0x{i.w:08x});'
    if rs != 0x10: return f'ee_bad(0x{i.a:08x}, 0x{i.w:08x});'
    m = {0: f'{F(fd)} = fpu_add({F(fs)}, {F(ft)});',
         1: f'{F(fd)} = fpu_sub({F(fs)}, {F(ft)});',
         2: f'{F(fd)} = fpu_mul({F(fs)}, {F(ft)});',
         3: f'{F(fd)} = fpu_div({F(fs)}, {F(ft)});',
         4: f'{F(fd)} = fpu_sqrt({F(ft)});',
         5: f'{U(fd)} = {U(fs)} & 0x7fffffffu;',
         6: f'{U(fd)} = {U(fs)};',
         7: f'{U(fd)} = {U(fs)} ^ 0x80000000u;',
         0x16: f'{F(fd)} = fpu_rsqrt({F(fs)}, {F(ft)});',
         0x18: f'ee.acc.f = fpu_add({F(fs)}, {F(ft)});',
         0x19: f'ee.acc.f = fpu_sub({F(fs)}, {F(ft)});',
         0x1a: f'ee.acc.f = fpu_mul({F(fs)}, {F(ft)});',
         0x1c: f'{F(fd)} = fpu_add(ee.acc.f, fpu_mul({F(fs)}, {F(ft)}));',
         0x1d: f'{F(fd)} = fpu_sub(ee.acc.f, fpu_mul({F(fs)}, {F(ft)}));',
         0x1e: f'ee.acc.f = fpu_add(ee.acc.f, fpu_mul({F(fs)}, {F(ft)}));',
         0x1f: f'ee.acc.f = fpu_sub(ee.acc.f, fpu_mul({F(fs)}, {F(ft)}));',
         0x24: f'{U(fd)} = fpu_cvtw({F(fs)});',
         0x28: f'{F(fd)} = fpu_max({F(fs)}, {F(ft)});',
         0x29: f'{F(fd)} = fpu_min({F(fs)}, {F(ft)});',
         0x30: 'ee.fcr31 &= ~FPU_C;',
         0x32: f'FPU_SETC({F(fs)} == {F(ft)});',
         0x34: f'FPU_SETC({F(fs)} < {F(ft)});',
         0x36: f'FPU_SETC({F(fs)} <= {F(ft)});'}
    return m.get(fn, f'ee_bad(0x{i.a:08x}, 0x{i.w:08x});')

def emit_simple(i):
    op, rs, rt, rd, sa, fn = i.op, i.rs, i.rt, i.rd, i.sa, i.fn
    imm, simm = i.imm, i.simm
    bad = f'ee_bad(0x{i.a:08x}, 0x{i.w:08x});'
    if op == 0:
        if i.w == 0: return ''
        S = {0: SX(f'{W(rt)} << {sa}'), 2: SX(f'{W(rt)} >> {sa}'), 3: SX(f'(s32){W(rt)} >> {sa}'),
             4: SX(f'{W(rt)} << ({W(rs)} & 31)'), 6: SX(f'{W(rt)} >> ({W(rs)} & 31)'),
             7: SX(f'(s32){W(rt)} >> ({W(rs)} & 31)'),
             20: f'{D(rt)} << ({W(rs)} & 63)', 22: f'(u64){D(rt)} >> ({W(rs)} & 63)',
             23: f'(u64)((s64){D(rt)} >> ({W(rs)} & 63))',
             32: SX(f'{W(rs)} + {W(rt)}'), 33: SX(f'{W(rs)} + {W(rt)}'),
             34: SX(f'{W(rs)} - {W(rt)}'), 35: SX(f'{W(rs)} - {W(rt)}'),
             36: f'{D(rs)} & {D(rt)}', 37: f'{D(rs)} | {D(rt)}', 38: f'{D(rs)} ^ {D(rt)}',
             39: f'~({D(rs)} | {D(rt)})',
             42: f'(u64)((s64){D(rs)} < (s64){D(rt)})', 43: f'(u64)((u64){D(rs)} < (u64){D(rt)})',
             44: f'{D(rs)} + {D(rt)}', 45: f'{D(rs)} + {D(rt)}',
             46: f'{D(rs)} - {D(rt)}', 47: f'{D(rs)} - {D(rt)}',
             56: f'{D(rt)} << {sa}', 58: f'(u64){D(rt)} >> {sa}', 59: f'(u64)((s64){D(rt)} >> {sa})',
             60: f'{D(rt)} << {sa + 32}', 62: f'(u64){D(rt)} >> {sa + 32}',
             63: f'(u64)((s64){D(rt)} >> {sa + 32})'}
        if fn in S: return setd(rd, S[fn])
        if fn == 10: return f'if ({D(rt)} == 0) {{ {setd(rd, D(rs))} }}' if rd else ''
        if fn == 11: return f'if ({D(rt)} != 0) {{ {setd(rd, D(rs))} }}' if rd else ''
        if fn == 12: return f'ee_syscall(0x{i.a:08x});'
        if fn == 13: return f'ee_break(0x{i.a:08x}, {(i.w >> 6) & 0xfffff});'
        if fn == 15: return ''
        if fn == 16: return setd(rd, 'ee.hi.d[0]')
        if fn == 17: return f'ee.hi.d[0] = {D(rs)};'
        if fn == 18: return setd(rd, 'ee.lo.d[0]')
        if fn == 19: return f'ee.lo.d[0] = {D(rs)};'
        if fn == 24: return f'{{ s64 m = (s64)(s32){W(rs)} * (s32){W(rt)}; ee.lo.d[0] = {SX("m")}; ee.hi.d[0] = {SX("m >> 32")}; {setd(rd, "ee.lo.d[0]")} }}'
        if fn == 25: return f'{{ u64 m = (u64){W(rs)} * {W(rt)}; ee.lo.d[0] = {SX("m")}; ee.hi.d[0] = {SX("m >> 32")}; {setd(rd, "ee.lo.d[0]")} }}'
        if fn == 26: return f'ee_div({W(rs)}, {W(rt)}, 0);'
        if fn == 27: return f'ee_divu({W(rs)}, {W(rt)}, 0);'
        if fn == 40: return setd(rd, 'ee.sa')
        if fn == 41: return f'ee.sa = {W(rs)} & 15;'
        if 48 <= fn <= 54: return ''      # traps
        return bad
    if op == 1:
        if rt == 0x18: return f'ee.sa = (({W(rs)} ^ {imm & 0xffff}) & 15);'
        if rt == 0x19: return f'ee.sa = ((({W(rs)} ^ {imm & 0xffff}) & 7) << 1);'
        if 8 <= rt <= 14: return ''
        return bad
    if op in (8, 9):  return setd(rt, SX(f'{W(rs)} + {simm & 0xffffffff:#x}u'))
    if op in (0x18, 0x19): return setd(rt, f'{D(rs)} + (u64){simm}ll')
    if op == 10: return setd(rt, f'(u64)((s64){D(rs)} < {simm}ll)')
    if op == 11: return setd(rt, f'(u64)((u64){D(rs)} < (u64){simm}ll)')
    if op == 12: return setd(rt, f'{D(rs)} & {imm:#x}')
    if op == 13: return setd(rt, f'{D(rs)} | {imm:#x}')
    if op == 14: return setd(rt, f'{D(rs)} ^ {imm:#x}')
    if op == 15: return setd(rt, SX(f'{(imm << 16):#x}u'))
    if op == 0x10:
        if rs == 0: return setd(rt, SX(f'cop0_read({rd})'))
        if rs == 4: return f'cop0_write({rd}, {W(rt)});'
        if rs == 0x10:
            if fn == 0x18: return f'ee_eret(); return;'
            if fn == 0x38: return 'ee_ei();'
            if fn == 0x39: return 'ee_di();'
            return ''
        return ''
    if op == 0x11: return fop(i)
    if op == 0x12:
        if rs == 1: return f'vu0_qmfc2({rt}, {rd});'
        if rs == 2: return f'vu0_cfc2({rt}, {rd});'
        if rs == 5: return f'vu0_qmtc2({rt}, {rd});'
        if rs == 6: return f'vu0_ctc2({rt}, {rd});'
        if rs >= 0x10: return f'vu0_macro(0x{i.w:08x}u);'
        return bad
    if op == 0x1c: return f'mmi_exec(0x{i.w:08x}u);'
    ea = f'{W(rs)} + {simm & 0xffffffff:#x}u'
    if op == 0x1e: return f'mrq({ea}, {rt});' if rt else ''
    if op == 0x1f: return f'mwq({ea}, {rt});'
    if op == 0x1a: return setd(rt, f'mldl({ea}, {D(rt)})')
    if op == 0x1b: return setd(rt, f'mldr({ea}, {D(rt)})')
    if op == 32: return (setd(rt, f'(u64)(s64)(s8)mr8({ea})') or f'mr8({ea});')
    if op == 33: return (setd(rt, f'(u64)(s64)(s16)mr16({ea})') or f'mr16({ea});')
    if op == 34: return setd(rt, SX(f'mlwl({ea}, {W(rt)})'))
    if op == 35: return (setd(rt, SX(f'mr32({ea})')) or f'mr32({ea});')
    if op == 36: return (setd(rt, f'mr8({ea})') or f'mr8({ea});')
    if op == 37: return (setd(rt, f'mr16({ea})') or f'mr16({ea});')
    if op == 38: return setd(rt, SX(f'mlwr({ea}, {W(rt)})'))
    if op == 39: return (setd(rt, f'(u64)mr32({ea})') or f'mr32({ea});')
    if op == 40: return f'mw8({ea}, (u8){W(rt)});'
    if op == 41: return f'mw16({ea}, (u16){W(rt)});'
    if op == 42: return f'mswl({ea}, {W(rt)});'
    if op == 43: return f'mw32({ea}, {W(rt)});'
    if op == 44: return f'msdl({ea}, {D(rt)});'
    if op == 45: return f'msdr({ea}, {D(rt)});'
    if op == 46: return f'mswr({ea}, {W(rt)});'
    if op == 47: return ''        # cache
    if op == 49: return f'ee.fpr[{rt}].u = mr32({ea});'
    if op == 51: return ''        # pref
    if op == 54: return f'vu0_lqc2({ea}, {rt});'
    if op == 55: return (setd(rt, f'mr64({ea})') or f'mr64({ea});')
    if op == 57: return f'mw32({ea}, ee.fpr[{rt}].u);'
    if op == 62: return f'vu0_sqc2({ea}, {rt});'
    if op == 63: return f'mw64({ea}, {D(rt)});'
    return bad

def cond(i):
    rs, rt = D(i.rs), D(i.rt)
    op = i.op
    if op in (0x10, 0x11, 0x12) and i.rs == 8:
        c = {0x10: 'ee_bc0()', 0x11: '((ee.fcr31 & FPU_C) != 0)', 0x12: 'vu0_bc2()'}[op]
        return c if i.rt & 1 else f'!{c}'
    if op >= 0x14: op -= 0x10
    if op == 4: return f'{rs} == {rt}'
    if op == 5: return f'{rs} != {rt}'
    if op == 6: return f'(s64){rs} <= 0'
    if op == 7: return f'(s64){rs} > 0'
    if op == 1: return f'(s64){rs} >= 0' if i.rt & 1 else f'(s64){rs} < 0'
    raise ValueError(hex(i.w))

# functions that start with a call to the runtime's ee_hook(addr) (runtime/main.c),
# which may adjust their arguments or run the body itself (a nonzero return skips it):
# 0x11dfd0 camera aspect setter, 0x101068 view frustum builder (both widescreen)
HOOKS = {0x00125040}   # main.c ee_hook: MH2_UNLOCK fame table

def callexpr(t, entries):
    if t in entries: return f'f_{t:08x}();'
    return f'ee_call(0x{t:08x}u);'

def gen_func(exe, f, entries, out):
    a0 = f.entry
    out.append(f'void f_{a0:08x}(void) {{ FENTRY(0x{a0:08x}u);' + (f' if (ee_hook(0x{a0:08x}u)) return;' if a0 in HOOKS else ''))
    addrs = sorted(f.addrs); aset = f.addrs
    needed = set(t for t in f.labels if t in aset)
    if addrs and addrs[0] != a0:
        needed.add(a0); out.append(f'  goto L_{a0:08x};')
    pend = 0; consumed = set(); prev = None
    for a in addrs:
        if prev is not None and a != prev + 4:
            out.append(f'  ee_fallthrough(0x{prev + 4:08x}); return;')
        prev = a
        if a in needed:
            if pend: out.append(f'  CYC({pend});'); pend = 0
            out.append(f'L_{a:08x}:;')
        if a in consumed and a not in needed: continue
        w = exe.word(a); i = Insn(a, w)
        if a in consumed:
            s = emit_simple(i)
            if s: out.append('  ' + s)
            pend += 1; continue
        if not valid(i):
            out.append(f'  ee_bad(0x{a:08x}, 0x{w:08x}); return;'); continue
        k = kind(i)
        pend += 1
        if k is None:
            s = emit_simple(i)
            if s: out.append('  ' + s)
            continue
        d = Insn(a + 4, exe.word(a + 4) or 0)
        ds = emit_simple(d) if kind(d) is None else f'/* branch in delay slot {d.w:08x} */'
        consumed.add(a + 4); pend += 1
        out.append(f'  CYC({pend});'); pend = 0
        if k in ('b', 'bl'):
            t = btarget(i)
            uncond = (i.op == 4 and i.rs == 0 and i.rt == 0) or (i.op == 1 and i.rs == 0 and i.rt == 1)
            if uncond:
                out.append(f'  {{ {ds} goto L_{t:08x}; }}')
            elif k == 'b':
                out.append(f'  {{ int c = {cond(i)}; {ds} if (c) goto L_{t:08x}; }}')
            else:
                out.append(f'  if ({cond(i)}) {{ {ds} goto L_{t:08x}; }}')
                if a + 8 in aset:
                    needed.add(a + 8); out.append(f'  goto L_{a + 8:08x};')
        elif k in ('bal', 'ball'):
            t = btarget(i); call = callexpr(t, entries)
            if i.rs == 0 and i.rt & 1:
                out.append(f'  {{ {ds} GD(31) = 0x{a + 8:08x}u; {call} }}')
            else:
                out.append(f'  {{ int c = {cond(i)}; GD(31) = 0x{a + 8:08x}u; if (c) {{ {ds} {call} }} else {{ {ds if k == "bal" else ""} }} }}')
        elif k == 'jal':
            out.append(f'  {{ {ds} GD(31) = 0x{a + 8:08x}u; {callexpr(i.tgt, entries)} }}')
        elif k == 'jalr':
            link = f'GD({i.rd}) = 0x{a + 8:08x}u;' if i.rd else ''
            out.append(f'  {{ u32 t = {W(i.rs)}; {ds} {link} ee_call(t); }}')
        elif k == 'j':
            t = i.tgt
            if t in aset and (t == a0 or t not in entries or t in WEAK):
                out.append(f'  {{ {ds} goto L_{t:08x}; }}')
            else:
                out.append(f'  {{ {ds} {callexpr(t, entries)} return; }}')
        elif k == 'jr':
            if i.rs == 31:
                out.append(f'  {{ {ds} return; }}')
            else:
                tab = f.tables.get(a)
                out.append(f'  {{ u32 t = {W(i.rs)}; {ds}')
                if tab:
                    out.append('    switch (t) {')
                    for t in sorted(set(tab)): out.append(f'    case 0x{t:08x}u: goto L_{t:08x};')
                    out.append('    }')
                out.append('    ee_call(t); return; }')
        if a + 4 in needed and k in ('b', 'bal', 'jal', 'jalr') and a + 8 in aset:
            needed.add(a + 8); out.append(f'  goto L_{a + 8:08x};')
    if prev is not None: out.append(f'  ee_fallthrough(0x{prev + 4:08x});')
    out.append('}'); out.append('')

def main():
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument('input'); ap.add_argument('out_dir')
    ap.add_argument('--extra', action='append', default=[])
    ap.add_argument('--code', action='append', default=[])
    ap.add_argument('--bbtrace', action='append', default=[], type=lambda x: int(x, 16))
    ap.add_argument('--fullcost', action='append', default=[], type=lambda x: int(x, 16))
    a = ap.parse_args()
    extra = []
    for fn in a.extra:
        if os.path.exists(fn):
            for ln in open(fn):
                ln = ln.split('#')[0].strip()
                if ln: extra.append(int(ln.split()[0], 16))
    exe = Elf(a.input)
    for r in a.code:
        lo, hi = r.split('-'); Elf.more.append((int(lo, 16), int(hi, 16)))
    funcs = discover(exe, extra)
    entries = set(funcs); keys = sorted(funcs)
    os.makedirs(a.out_dir, exist_ok=True)
    hdr = ['/* generated */', '#pragma once', '#include "ee.h"']
    hdr += [f'void f_{e:08x}(void);' for e in keys]
    open(os.path.join(a.out_dir, 'funcs.h'), 'w').write(NL.join(hdr) + NL)
    open(os.path.join(a.out_dir, 'entries.txt'), 'w').write(''.join(f'{e:08x}' + NL for e in keys))
    per = 200; nfiles = 0
    for n in range(0, len(keys), per):
        out = ['#include "funcs.h"', '#if defined(__GNUC__)',
               '#pragma GCC diagnostic ignored "-Wunused-label"',
               '#pragma GCC diagnostic ignored "-Wunused-variable"', '#endif', '']
        for e in keys[n:n + per]:
            n0 = len(out)
            gen_func(exe, funcs[e], entries, out)
            if e in a.bbtrace:   # debug: log every basic block of this function (ee_bb in main.c)
                import re
                out[n0:] = [re.sub(r'^L_([0-9a-f]{8}):;$', r'L_\1:; ee_bb(0x\1u);', l) for l in out[n0:]]
                out[n0:] = [l.replace(');', '); ee_bb(__LINE__);', 1) if l.startswith('  CYC(') else l for l in out[n0:]]
            if e in a.fullcost: out[n0:] = [l.replace('  CYC(', '  CYCF(') for l in out[n0:]]
        open(os.path.join(a.out_dir, f'code_{nfiles:03d}.c'), 'w').write(NL.join(out))
        nfiles += 1
    tab = ['#include "funcs.h"', 'const ee_func_entry ee_table[] = {']
    tab += [f'  {{ 0x{e:08x}u, f_{e:08x} }},' for e in keys]
    tab.append('};')
    tab.append(f'const unsigned ee_table_count = {len(keys)};')
    tab.append(f'const u32 ee_entry = 0x{exe.pc0:08x}u;')
    open(os.path.join(a.out_dir, 'table.c'), 'w').write(NL.join(tab) + NL)
    ninsn = sum(len(f.addrs) for f in funcs.values())
    njt = sum(len(f.tables) for f in funcs.values())
    print(f'{len(keys)} functions, {ninsn} insns, {njt} jump tables, {nfiles} files')
    open(os.path.join(a.out_dir, 'unresolved_jr.txt'), 'w').write(''.join('%08x' % x + chr(10) for x in sorted(UNRESOLVED_JR)))
    print(f'{len(UNRESOLVED_JR)} jr without a jump table (gen/unresolved_jr.txt)')

if __name__ == '__main__':
    main()
