"""EE (MIPS R5900) disassembler for PS2 ELFs. Pure Python, no capstone.

usage:
  python -I eedis.py <elf> <addr> [count]      disassemble count insns (default 64)
  python -I eedis.py <elf> -f <addr>           whole function containing addr
  python -I eedis.py <elf> --xref <addr> [--span N]
        every load/store/address-forming insn and jal/j whose resolved target
        is in [addr, addr+span) (span default 1), grouped by function

Annotations: hardware register names (IPU, DMAC, GIF/VIF, GS, INTC, SIF,
timers), function names, and data strings. lui/addiu/ori constants are tracked
linearly through each function (cleared for caller-saved regs at calls and at
branch targets), so `lw v0,0x2010(v1)` shows `; IPU_CTRL`.

Function starts come from gen/entries.txt (written by recomp_ee.py). Names come
from tools/names.txt: one `<hex addr> <name>  # comment` per line. Add names as
you identify functions; every later run shows them.
"""
import os, struct, sys, bisect

HERE = os.path.dirname(os.path.abspath(__file__))
R = ['zero','at','v0','v1','a0','a1','a2','a3','t0','t1','t2','t3','t4','t5','t6','t7',
     's0','s1','s2','s3','s4','s5','s6','s7','t8','t9','k0','k1','gp','sp','fp','ra']
CALLER_SAVED = set(range(1, 16)) | {24, 25, 31}

def s16(x): return x - 0x10000 if x & 0x8000 else x

# ---- hardware register names -------------------------------------------------
HW = {}
def _hw():
    for t in range(4):
        b = 0x10000000 + 0x800 * t
        for o, n in ((0, 'COUNT'), (0x10, 'MODE'), (0x20, 'COMP'), (0x30, 'HOLD')):
            HW[b + o] = f'T{t}_{n}'
    HW.update({0x10002000: 'IPU_CMD', 0x10002010: 'IPU_CTRL', 0x10002020: 'IPU_BP',
               0x10002030: 'IPU_TOP', 0x10003000: 'GIF_CTRL', 0x10003010: 'GIF_MODE',
               0x10003020: 'GIF_STAT', 0x10003800: 'VIF0_STAT', 0x10003810: 'VIF0_FBRST',
               0x10003c00: 'VIF1_STAT', 0x10003c10: 'VIF1_FBRST', 0x10004000: 'VIF0_FIFO',
               0x10005000: 'VIF1_FIFO', 0x10006000: 'GIF_FIFO', 0x10007000: 'IPU_OUT_FIFO',
               0x10007010: 'IPU_IN_FIFO', 0x1000e000: 'D_CTRL', 0x1000e010: 'D_STAT',
               0x1000e020: 'D_PCR', 0x1000e030: 'D_SQWC', 0x1000e040: 'D_RBSR',
               0x1000e050: 'D_RBOR', 0x1000e060: 'D_STADR', 0x1000f000: 'INTC_STAT',
               0x1000f010: 'INTC_MASK', 0x1000f200: 'SIF_MSCOM', 0x1000f210: 'SIF_SMCOM',
               0x1000f220: 'SIF_MSFLG', 0x1000f230: 'SIF_SMFLG', 0x1000f520: 'D_ENABLER',
               0x1000f590: 'D_ENABLEW', 0x1000f180: 'KPUTCHAR',
               0x12001000: 'GS_CSR', 0x12001010: 'GS_IMR', 0x12001040: 'GS_BUSDIR',
               0x12001080: 'GS_SIGLBLID'})
    gs = ['PMODE','SMODE1','SMODE2','SRFSH','SYNCH1','SYNCH2','SYNCV','DISPFB1',
          'DISPLAY1','DISPFB2','DISPLAY2','EXTBUF','EXTDATA','EXTWRITE','BGCOLOR']
    for k, n in enumerate(gs): HW[0x12000000 + 0x10 * k] = 'GS_' + n
    chans = [(0x8000,'VIF0'),(0x9000,'VIF1'),(0xa000,'GIF'),(0xb000,'fromIPU'),(0xb400,'toIPU'),
             (0xc000,'SIF0'),(0xc400,'SIF1'),(0xc800,'SIF2'),(0xd000,'fromSPR'),(0xd400,'toSPR')]
    for k, (b, n) in enumerate(chans):
        for o, r in ((0,'CHCR'),(0x10,'MADR'),(0x20,'QWC'),(0x30,'TADR'),(0x40,'ASR0'),(0x50,'ASR1'),(0x80,'SADR')):
            HW[0x10000000 + b + o] = f'D{k}_{n}_{r}'
_hw()

def hwname(a):
    a &= 0xffffffff
    for m in (a, a & 0x1fffffff):
        if m in HW: return HW[m]
    if 0x70000000 <= a < 0x70004000: return f'SPR+{a - 0x70000000:#x}'
    return None

# ---- ELF ---------------------------------------------------------------------
class Elf:
    def __init__(self, path):
        d = open(path, 'rb').read()
        assert d[:4] == b'\x7fELF', 'not an ELF'
        phoff, = struct.unpack_from('<I', d, 0x1c)
        shoff, = struct.unpack_from('<I', d, 0x20)
        phnum, = struct.unpack_from('<H', d, 0x2c)
        shnum, shstr = struct.unpack_from('<HH', d, 0x30)
        self.segs = []
        for k in range(phnum):
            typ, off, va, pa, fsz, msz = struct.unpack_from('<6I', d, phoff + 32 * k)
            if typ == 1: self.segs.append((va, d[off:off + fsz]))
        self.text = (0, 0)
        if shnum:
            secs = [struct.unpack_from('<6I', d, shoff + 40 * k) for k in range(shnum)]
            so = secs[shstr][4]
            for nm, typ, fl, addr, off, sz in secs:
                if d[so + nm:d.index(b'\0', so + nm)] == b'.text': self.text = (addr, addr + sz)
        if self.text == (0, 0) and self.segs:
            va, data = self.segs[0]; o = 0; jr = bytes([8, 0, 0xe0, 3])
            while o < len(data) and jr in data[o:o + 0x10000]: o += 0x10000
            while o > 0 and data[o - 4:o] != jr: o -= 4
            self.text = (va, va + o + 4)
    def bytes_at(self, a, n):
        for va, data in self.segs:
            if va <= a and a + n <= va + len(data): return data[a - va:a - va + n]
        return None
    def word(self, a):
        b = self.bytes_at(a, 4)
        return struct.unpack('<I', b)[0] if b else None
    def cstr(self, a):
        b = self.bytes_at(a, 1)
        if not b: return None
        out = bytearray()
        while len(out) < 80:
            c = self.bytes_at(a + len(out), 1)
            if not c or c == b'\0': break
            if not (32 <= c[0] < 127 or c[0] in (9, 10)): return None
            out += c
        return out.decode() if len(out) >= 4 else None

def load_entries(elf_path):
    for p in (os.path.join(HERE, '..', 'gen', 'entries.txt'),):
        if os.path.exists(p): return sorted(int(l, 16) for l in open(p) if l.strip())
    return []

def load_names():
    names = {}
    p = os.path.join(HERE, 'names.txt')
    if os.path.exists(p):
        for ln in open(p):
            ln = ln.split('#')[0].split()
            if len(ln) >= 2: names[int(ln[0], 16)] = ln[1]
    return names

# ---- decoder -----------------------------------------------------------------
SPECIAL = {0:'sll',2:'srl',3:'sra',4:'sllv',6:'srlv',7:'srav',8:'jr',9:'jalr',10:'movz',11:'movn',
           12:'syscall',13:'break',15:'sync',16:'mfhi',17:'mthi',18:'mflo',19:'mtlo',20:'dsllv',
           22:'dsrlv',23:'dsrav',24:'mult',25:'multu',26:'div',27:'divu',32:'add',33:'addu',
           34:'sub',35:'subu',36:'and',37:'or',38:'xor',39:'nor',40:'mfsa',41:'mtsa',42:'slt',
           43:'sltu',44:'dadd',45:'daddu',46:'dsub',47:'dsubu',48:'tge',49:'tgeu',50:'tlt',
           51:'tltu',52:'teq',54:'tne',56:'dsll',58:'dsrl',59:'dsra',60:'dsll32',62:'dsrl32',63:'dsra32'}
REGIMM = {0:'bltz',1:'bgez',2:'bltzl',3:'bgezl',8:'tgei',9:'tgeiu',10:'tlti',11:'tltiu',12:'teqi',
          14:'tnei',16:'bltzal',17:'bgezal',18:'bltzall',19:'bgezall',24:'mtsab',25:'mtsah'}
IMM = {8:'addi',9:'addiu',10:'slti',11:'sltiu',12:'andi',13:'ori',14:'xori',24:'daddi',25:'daddiu'}
MEM = {26:'ldl',27:'ldr',30:'lq',31:'sq',32:'lb',33:'lh',34:'lwl',35:'lw',36:'lbu',37:'lhu',38:'lwr',
       39:'lwu',40:'sb',41:'sh',42:'swl',43:'sw',44:'sdl',45:'sdr',46:'swr',55:'ld',63:'sd'}
FMEM = {49:'lwc1',57:'swc1',54:'lqc2',62:'sqc2'}
BR = {4:'beq',5:'bne',6:'blez',7:'bgtz',20:'beql',21:'bnel',22:'blezl',23:'bgtzl'}
MMI = {0:'madd',1:'maddu',4:'plzcw',16:'mfhi1',17:'mthi1',18:'mflo1',19:'mtlo1',24:'mult1',
       25:'multu1',26:'div1',27:'divu1',32:'madd1',33:'maddu1',48:'pmfhl',49:'pmthl',52:'psllh',
       54:'psrlh',55:'psrah',60:'psllw',62:'psrlw',63:'psraw'}
MMI0 = {0:'paddw',1:'psubw',2:'pcgtw',3:'pmaxw',4:'paddh',5:'psubh',6:'pcgth',7:'pmaxh',8:'paddb',
        9:'psubb',10:'pcgtb',16:'paddsw',17:'psubsw',18:'pextlw',19:'ppacw',20:'paddsh',21:'psubsh',
        22:'pextlh',23:'ppach',24:'paddsb',25:'psubsb',26:'pextlb',27:'ppacb',30:'pext5',31:'ppac5'}
MMI1 = {1:'pabsw',2:'pceqw',3:'pminw',4:'padsbh',5:'pabsh',6:'pceqh',7:'pminh',10:'pceqb',16:'padduw',
        17:'psubuw',18:'pextuw',20:'padduh',21:'psubuh',22:'pextuh',24:'paddub',25:'psubub',
        26:'pextub',27:'qfsrv'}
MMI2 = {0:'pmaddw',2:'psllvw',3:'psrlvw',4:'pmsubw',8:'pmfhi',9:'pmflo',10:'pinth',12:'pmultw',
        13:'pdivw',14:'pcpyld',16:'pmaddh',17:'phmadh',18:'pand',19:'pxor',20:'pmsubh',21:'phmsbh',
        26:'pexeh',27:'prevh',28:'pmulth',29:'pdivbw',30:'pexew',31:'prot3w'}
MMI3 = {0:'pmadduw',3:'psravw',8:'pmthi',9:'pmtlo',10:'pinteh',12:'pmultuw',13:'pdivuw',14:'pcpyud',
        18:'por',19:'pnor',26:'pexch',27:'pcpyh',30:'pexcw'}
FPU_S = {0:'add.s',1:'sub.s',2:'mul.s',3:'div.s',4:'sqrt.s',5:'abs.s',6:'mov.s',7:'neg.s',22:'rsqrt.s',
         24:'adda.s',25:'suba.s',26:'mula.s',28:'madd.s',29:'msub.s',30:'madda.s',31:'msuba.s',
         36:'cvt.w.s',40:'max.s',41:'min.s',48:'c.f.s',50:'c.eq.s',52:'c.lt.s',54:'c.le.s'}

class D:
    """Decoded instruction: text plus fields the annotator needs."""
    __slots__ = ('a','w','text','kind','base','off','dst','target')
    def __init__(self, a, w):
        self.a, self.w = a, w
        self.kind = ''; self.base = self.dst = self.target = None; self.off = 0
        self.text = self._dis()
    def _dis(self):
        a, w = self.a, self.w
        op, rs, rt, rd, sa, fn = w >> 26, (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31, w & 63
        imm = w & 0xffff; simm = s16(imm)
        bt = (a + 4 + (simm << 2)) & 0xffffffff
        if w == 0: return 'nop'
        if op == 0:
            m = SPECIAL.get(fn)
            if m is None: return f'.word {w:08x}'
            if fn in (0, 2, 3, 56, 58, 59, 60, 62, 63): return f'{m} {R[rd]},{R[rt]},{sa}'
            if fn in (4, 6, 7, 20, 22, 23): return f'{m} {R[rd]},{R[rt]},{R[rs]}'
            if fn == 8: self.kind = 'ret' if rs == 31 else 'jr'; return f'jr {R[rs]}'
            if fn == 9: self.kind = 'callr'; return f'jalr {R[rd]},{R[rs]}' if rd != 31 else f'jalr {R[rs]}'
            if fn in (12, 13, 15): return f'{m} {w >> 6 & 0xfffff:#x}' if fn != 15 else m
            if fn in (16, 18, 40): return f'{m} {R[rd]}'
            if fn in (17, 19, 41): return f'{m} {R[rs]}'
            if fn in (24, 25) and rd: return f'{m} {R[rd]},{R[rs]},{R[rt]}'
            if fn in (24, 25, 26, 27) or 48 <= fn <= 54: return f'{m} {R[rs]},{R[rt]}'
            if fn in (33, 45, 37) and rt == 0:
                self.dst = rd; self.kind = 'move'; self.base = rs
                return f'move {R[rd]},{R[rs]}'
            self.dst = rd
            return f'{m} {R[rd]},{R[rs]},{R[rt]}'
        if op == 1:
            m = REGIMM.get(rt, 'regimm?')
            if rt in (24, 25): return f'{m} {R[rs]},{simm}'
            if rt >= 8 and rt < 16: return f'{m} {R[rs]},{simm}'
            self.kind = 'callb' if rt in (16, 17, 18, 19) else 'br'; self.target = bt
            return f'{m} {R[rs]},{bt:x}'
        if op in (2, 3):
            t = ((a + 4) & 0xf0000000) | ((w & 0x3ffffff) << 2)
            self.kind = 'j' if op == 2 else 'call'; self.target = t
            return f'{"j" if op == 2 else "jal"} {t:x}'
        if op in BR:
            self.kind = 'br'; self.target = bt
            if op == 4 and rs == rt == 0: return f'b {bt:x}'
            if op in (4, 5, 20, 21): return f'{BR[op]} {R[rs]},{R[rt]},{bt:x}'
            return f'{BR[op]} {R[rs]},{bt:x}'
        if op in IMM:
            self.dst = rt; self.base = rs; self.off = imm if op in (12, 13, 14) else simm
            self.kind = {9: 'add', 25: 'add', 8: 'add', 24: 'add', 13: 'or'}.get(op, '')
            if op in (9, 25) and rs == 0: return f'li {R[rt]},{simm}'
            if op == 13 and rs == 0: return f'li {R[rt]},{imm:#x}'
            if op in (12, 13, 14): return f'{IMM[op]} {R[rt]},{R[rs]},{imm:#x}'
            return f'{IMM[op]} {R[rt]},{R[rs]},{simm}'
        if op == 15: self.dst = rt; self.kind = 'lui'; self.off = imm << 16; return f'lui {R[rt]},{imm:#x}'
        if op in MEM:
            self.kind = 'mem'; self.base = rs; self.off = simm
            if MEM[op][0] == 'l': self.dst = rt
            return f'{MEM[op]} {R[rt]},{simm}({R[rs]})'
        if op in FMEM:
            self.kind = 'mem'; self.base = rs; self.off = simm
            reg = f'$f{rt}' if op in (49, 57) else f'vf{rt}'
            return f'{FMEM[op]} {reg},{simm}({R[rs]})'
        if op == 47: return f'cache {rt:#x},{simm}({R[rs]})'
        if op == 51: return f'pref {rt},{simm}({R[rs]})'
        if op == 16:
            if rs == 0: self.dst = rt; return f'mfc0 {R[rt]},${rd}'
            if rs == 4: return f'mtc0 {R[rt]},${rd}'
            if rs == 8:
                self.kind = 'br'; self.target = bt
                return f'{["bc0f","bc0t","bc0fl","bc0tl"][rt & 3]} {bt:x}'
            if rs == 16: return {0x18: 'eret', 0x38: 'ei', 0x39: 'di', 2: 'tlbwi', 6: 'tlbwr', 8: 'tlbp', 1: 'tlbr'}.get(fn, f'cop0 {w:08x}')
            return f'cop0 {w:08x}'
        if op == 17:
            if rs == 0: self.dst = rt; return f'mfc1 {R[rt]},$f{rd}'
            if rs == 2: self.dst = rt; return f'cfc1 {R[rt]},${rd}'
            if rs == 4: return f'mtc1 {R[rt]},$f{rd}'
            if rs == 6: return f'ctc1 {R[rt]},${rd}'
            if rs == 8:
                self.kind = 'br'; self.target = bt
                return f'{["bc1f","bc1t","bc1fl","bc1tl"][rt & 3]} {bt:x}'
            if rs == 16:
                m = FPU_S.get(fn, f'fpu.s?{fn}')
                fs, ft, fd = rd, rt, sa
                if fn in (24, 25, 26, 30, 31) or fn >= 48: return f'{m} $f{fs},$f{ft}'
                if fn in (4, 5, 6, 7, 36): return f'{m} $f{fd},$f{fs}' if fn != 4 else f'{m} $f{fd},$f{ft}'
                return f'{m} $f{fd},$f{fs},$f{ft}'
            if rs == 20 and fn == 32: return f'cvt.s.w $f{sa},$f{rd}'
            return f'cop1 {w:08x}'
        if op == 18:
            if rs == 1: self.dst = rt; return f'qmfc2 {R[rt]},vf{rd}'
            if rs == 2: self.dst = rt; return f'cfc2 {R[rt]},vi{rd}'
            if rs == 5: return f'qmtc2 {R[rt]},vf{rd}'
            if rs == 6: return f'ctc2 {R[rt]},vi{rd}'
            if rs == 8:
                self.kind = 'br'; self.target = bt
                return f'{["bc2f","bc2t","bc2fl","bc2tl"][rt & 3]} {bt:x}'
            if rs >= 16:
                if fn == 0x38: return f'vcallms {(w >> 6 & 0x7fff) * 8:#x}'
                if fn == 0x39: return 'vcallmsr vi27'
                return f'vu0 {w & 0x1ffffff:07x}'
            return f'cop2 {w:08x}'
        if op == 28:
            if fn == 8: m = MMI0.get(sa)
            elif fn == 40: m = MMI1.get(sa)
            elif fn == 9: m = MMI2.get(sa)
            elif fn == 41: m = MMI3.get(sa)
            else: m = MMI.get(fn)
            if m is None: return f'mmi? {w:08x}'
            if m in ('psllh','psrlh','psrah','psllw','psrlw','psraw'): return f'{m} {R[rd]},{R[rt]},{sa}'
            if m in ('pmfhi','pmflo','mfhi1','mflo1'): return f'{m} {R[rd]}'
            if m in ('pmthi','pmtlo','mthi1','mtlo1'): return f'{m} {R[rs]}'
            if m in ('pmfhl', 'pmthl'): return f'{m}.{["lw","uw","slw","lh","sh"][sa] if sa < 5 else sa} {R[rd]}'
            return f'{m} {R[rd]},{R[rs]},{R[rt]}'
        return f'.word {w:08x}'

# ---- annotation --------------------------------------------------------------
class Ctx:
    def __init__(self, elf, entries, names):
        self.elf, self.entries, self.names = elf, entries, names
    def fname(self, a):
        if a in self.names: return self.names[a]
        if self.entries:
            k = bisect.bisect_left(self.entries, a)
            if k < len(self.entries) and self.entries[k] == a: return f'fn_{a:08x}'
        return None
    def func_of(self, a):
        k = bisect.bisect_right(self.entries, a) - 1
        if k < 0: return None, None
        end = self.entries[k + 1] if k + 1 < len(self.entries) else self.elf.text[1]
        return self.entries[k], end
    def label(self, a):
        f, _ = self.func_of(a)
        if f is None: return f'{a:08x}'
        n = self.fname(f) or f'fn_{f:08x}'
        return n if f == a else f'{n}+{a - f:#x}'
    def describe(self, v):
        h = hwname(v)
        if h: return h
        n = self.fname(v)
        if n: return n
        s = self.elf.cstr(v)
        if s: return repr(s)
        return None

def walk(ctx, a0, a1, on_insn):
    """Linear pass with constant tracking; on_insn(d, resolved_addr_or_None)."""
    regs = {}
    branch_targets = set()
    for a in range(a0, a1, 4):
        w = ctx.elf.word(a)
        if w is None: break
        d = D(a, w)
        if d.kind == 'br' and d.target is not None: branch_targets.add(d.target)
    prev = None
    for a in range(a0, a1, 4):
        w = ctx.elf.word(a)
        if w is None: break
        if a in branch_targets: regs = {}
        d = D(a, w)
        val = None
        if d.kind == 'lui': val = d.off
        elif d.kind in ('add', 'or') and d.base in regs:
            val = (regs[d.base] + d.off) & 0xffffffff if d.kind == 'add' else regs[d.base] | d.off
        elif d.kind == 'add' and d.base == 0: val = d.off & 0xffffffff
        elif d.kind == 'or' and d.base == 0: val = d.off
        elif d.kind == 'move' and d.base in regs: val = regs[d.base]
        res = None
        if d.kind == 'mem' and d.base in regs: res = (regs[d.base] + d.off) & 0xffffffff
        elif d.kind in ('add', 'or') and val is not None and d.base != 0: res = val
        elif d.kind in ('call', 'j', 'callb'): res = d.target
        on_insn(d, res)
        if d.dst:
            if val is not None: regs[d.dst] = val
            else: regs.pop(d.dst, None)
        if prev is not None and prev.kind in ('call', 'callr'):
            for r in CALLER_SAVED: regs.pop(r, None)
        prev = d

def fmt(ctx, d, res):
    t = d.text
    if d.kind in ('call', 'j') and d.target is not None:
        n = ctx.fname(d.target)
        if n: t = t.rsplit(' ', 1)[0] + ' ' + n
    elif d.kind == 'br' and d.target is not None:
        f, _ = ctx.func_of(d.a)
        if f is not None and ctx.func_of(d.target)[0] != f: t += f'  <{ctx.label(d.target)}>'
    note = ''
    if res is not None and d.kind not in ('call', 'j'):
        desc = ctx.describe(res)
        note = f'  ; {res:08x}' + (f' {desc}' if desc else '')
    head = ''
    if d.a in ctx.names or (ctx.entries and ctx.fname(d.a)):
        head = f'\n{ctx.fname(d.a)}:\n'
    return f'{head}{d.a:08x}: {d.w:08x}  {t}{note}'

def main(argv):
    if len(argv) < 3: print(__doc__); return 1
    elf = Elf(argv[1])
    ctx = Ctx(elf, load_entries(argv[1]), load_names())
    if argv[2] == '--xref':
        tgt = int(argv[3], 16); span = 1
        if '--span' in argv: span = int(argv[argv.index('--span') + 1], 0)
        hits = []
        def hit(d, res):
            if res is not None and tgt <= res < tgt + span: hits.append(fmt(ctx, d, res).strip())
            elif res is not None and tgt <= (res & 0x1fffffff) < tgt + span and tgt >= 0x10000000:
                hits.append(fmt(ctx, d, res).strip())
        starts = ctx.entries or [elf.text[0]]
        for k, f in enumerate(starts):
            end = starts[k + 1] if k + 1 < len(starts) else elf.text[1]
            n0 = len(hits)
            walk(ctx, f, end, hit)
            if len(hits) > n0: hits.insert(n0, f'-- {ctx.fname(f) or f"{f:08x}"}')
        print('\n'.join(hits) if hits else 'no references')
        return 0
    if argv[2] == '-f':
        a = int(argv[3], 16)
        a0, a1 = ctx.func_of(a)
        if a0 is None: print('no function list (gen/entries.txt)'); return 1
    else:
        a0 = int(argv[2], 16) & ~3
        a1 = a0 + 4 * (int(argv[3], 0) if len(argv) > 3 else 64)
    out = []
    walk(ctx, a0, a1, lambda d, res: out.append(fmt(ctx, d, res)))
    print('\n'.join(out))
    return 0

if __name__ == '__main__':
    sys.exit(main(sys.argv))
