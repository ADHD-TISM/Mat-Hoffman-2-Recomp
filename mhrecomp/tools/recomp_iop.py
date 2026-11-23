"""Static recompiler for IOP modules: IRX (relocatable MIPS R3000 ELF) -> C.

usage: python recomp_iop.py <file.irx> <prefix> <base_hex> <out_dir>

The module is placed at a fixed IOP address, relocations are applied here, and
the relocated image is emitted as a C array the runtime copies into IOP RAM
when the game asks for the module to be loaded. Import stubs become calls to
iop_import("library", index). Same code model as recomp_ee.py.
"""
import os, struct, sys

NL = chr(10)
def s16(x): return x - 0x10000 if x & 0x8000 else x

class Irx:
    def __init__(self, path, base):
        d = open(path, 'rb').read()
        self.base = base
        entry, = struct.unpack_from('<I', d, 0x18)
        shoff, = struct.unpack_from('<I', d, 0x20)
        shnum, shstr = struct.unpack_from('<HH', d, 0x30)
        secs = [struct.unpack_from('<10I', d, shoff + 40 * k) for k in range(shnum)]
        so = secs[shstr][4]
        name = lambda n: d[so + n:d.index(b'\0', so + n)].decode()
        top = 0
        for s in secs:
            if s[2] & 2: top = max(top, s[3] + s[5])
        img = bytearray((top + 15) & ~15)
        for s in secs:
            nm, typ, fl, addr, off, size = s[:6]
            if fl & 2 and typ != 8: img[addr:addr + size] = d[off:off + size]
            if name(nm) == '.text': self.text = (base + addr, base + addr + size)
            if typ == 0x70000080: self.gp = base + struct.unpack_from('<I', d, off + 8)[0]
        # relocations (all relative to the module base)
        for s in secs:
            nm, typ, fl, addr, off, size = s[:6]
            if typ != 9: continue
            n = size // 8; k = 0
            while k < n:
                ro, ri = struct.unpack_from('<II', d, off + 8 * k); rt = ri & 0xff
                w, = struct.unpack_from('<I', img, ro)
                if rt == 2: w = (w + base) & 0xffffffff
                elif rt == 4: w = (w & 0xfc000000) | (((w & 0x3ffffff) + (base >> 2)) & 0x3ffffff)
                elif rt == 5:
                    # HI16 pairs with the next LO16
                    ro2, ri2 = struct.unpack_from('<II', d, off + 8 * (k + 1))
                    w2, = struct.unpack_from('<I', img, ro2)
                    v = ((w & 0xffff) << 16) + s16(w2 & 0xffff) + base
                    w = (w & 0xffff0000) | (((v + 0x8000) >> 16) & 0xffff)
                elif rt == 6: w = (w & 0xffff0000) | ((w + base) & 0xffff)
                elif rt == 7: pass      # GPREL16: gp is set from a relocated address
                else: print(f'warning: reloc type {rt} at {ro:x}')
                struct.pack_into('<I', img, ro, w)
                k += 1
        self.data = bytes(img)
        self.end = base + len(img)
        self.pc0 = base + entry
        # import stubs
        self.stubs = {}
        a = self.text[0]
        while a < self.text[1] - 20:
            if self.word(a) == 0x41e00000:
                o = a - base
                lib = self.data[o + 12:o + 20].split(b'\0')[0].decode('ascii', 'replace')
                b = a + 20
                while b < self.text[1] and self.word(b) == 0x03e00008:
                    self.stubs[b] = (lib, self.word(b + 4) & 0xffff); b += 8
                a = b
            else: a += 4
    def word(self, a):
        o = a - self.base
        if o < 0 or o + 4 > len(self.data): return None
        return struct.unpack_from('<I', self.data, o)[0]
    def inside(self, a): return self.text[0] <= a < self.text[1] and (a & 3) == 0

class Insn:
    __slots__ = ('a','w','op','rs','rt','rd','sa','fn','imm','simm','tgt')
    def __init__(self, a, w):
        self.a, self.w = a, w
        self.op = w >> 26; self.rs = (w >> 21) & 31; self.rt = (w >> 16) & 31
        self.rd = (w >> 11) & 31; self.sa = (w >> 6) & 31; self.fn = w & 63
        self.imm = w & 0xffff; self.simm = s16(self.imm)
        self.tgt = ((a + 4) & 0xf0000000) | ((w & 0x3ffffff) << 2)

def kind(i):
    if i.op == 0:
        if i.fn == 8: return 'jr'
        if i.fn == 9: return 'jalr'
        return None
    if i.op == 1: return 'bal' if i.rt & 0x1e == 0x10 else 'b'
    if i.op == 2: return 'j'
    if i.op == 3: return 'jal'
    if 4 <= i.op <= 7: return 'b'
    return None

def btarget(i): return (i.a + 4 + (i.simm << 2)) & 0xffffffff
VALID_OPS = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,32,33,34,35,36,37,38,40,41,42,43,46}
VALID_SPECIAL = {0,2,3,4,6,7,8,9,12,13,16,17,18,19,24,25,26,27,32,33,34,35,36,37,38,39,42,43}
def valid(i): return i.op in VALID_OPS and (i.op != 0 or i.fn in VALID_SPECIAL)

class Func:
    def __init__(self, entry):
        self.entry = entry; self.addrs = set(); self.labels = set(); self.tables = {}
        self.calls = set(); self.ptrs = set()

def find_jump_table(exe, i):
    reg = i.rs; lw = None; n = None; luis = []; addius = {}
    a = i.a - 4
    for _ in range(24):
        w = exe.word(a)
        if w is None: break
        j = Insn(a, w)
        if lw is None:
            if j.op == 35 and j.rt == reg: lw = j
        else:
            if j.op == 15: luis.append(j)
            elif j.op == 9 and j.rs == j.rt: addius.setdefault(j.rt, j)
            elif j.op == 11 and n is None: n = j.imm
        a -= 4
    if lw is None or not n: return None
    best = None
    for l in luis:
        lo = addius[l.rt].simm if l.rt in addius and addius[l.rt].a > l.a else 0
        base = ((l.imm << 16) + lo + lw.simm) & 0xffffffff
        out = []
        for idx in range(n):
            t = exe.word(base + 4 * idx)
            if t is None or not exe.inside(t): break
            out.append(t)
        if len(out) == n and (best is None or len(out) > len(best)): best = out
    return best

def analyse(exe, entry, entries):
    f = Func(entry); work = [entry]
    while work:
        a = work.pop()
        while True:
            if a in f.addrs or not exe.inside(a) or (a in exe.stubs and a != entry): break
            i = Insn(a, exe.word(a)); f.addrs.add(a)
            if not valid(i): break
            if i.op == 15:
                for k in range(1, 12):
                    nw = exe.word(a + 4 * k)
                    if nw is None: break
                    n = Insn(a + 4 * k, nw)
                    if n.op in (9, 13) and n.rs == i.rt:
                        f.ptrs.add(((i.imm << 16) + (n.simm if n.op == 9 else n.imm)) & 0xffffffff)
                    if kind(n) in ('j', 'jr'): break
            k = kind(i)
            if k is None: a += 4; continue
            f.addrs.add(a + 4)
            if k == 'b':
                t = btarget(i); f.labels.add(t); work.append(t)
                if i.op == 4 and i.rs == 0 and i.rt == 0: break
                a += 8; continue
            if k == 'bal': f.calls.add(btarget(i)); a += 8; continue
            if k == 'jal': f.calls.add(i.tgt); a += 8; continue
            if k == 'jalr': a += 8; continue
            if k == 'j':
                t = i.tgt
                if t != entry and t in entries: break
                f.labels.add(t); work.append(t); break
            if k == 'jr':
                if i.rs != 31:
                    tab = find_jump_table(exe, i)
                    if tab:
                        f.tables[a] = tab
                        for t in tab: f.labels.add(t); work.append(t)
                break
    return f

def plausible_start(exe, t):
    if not exe.inside(t) or t in exe.stubs: return exe.inside(t)
    w = exe.word(t)
    if w is None or not valid(Insn(t, w)): return False
    if exe.word(t - 8) == 0x03e00008: return True
    return (w >> 16) == 0x27bd and bool(w & 0x8000)

def discover(exe):
    entries = {exe.pc0} | set(exe.stubs)
    for o in range(0, len(exe.data) - 3, 4):
        v = struct.unpack_from('<I', exe.data, o)[0]
        if exe.inside(v) and plausible_start(exe, v): entries.add(v)
    a = exe.text[0] + 8
    while a < exe.text[1]:
        if exe.word(a - 8) == 0x03e00008 and a not in exe.stubs and (a - 8) not in exe.stubs:
            t = a
            while exe.word(t) == 0 and t < exe.text[1] - 4: t += 4
            w = exe.word(t)
            if w is not None and w != 0x41e00000 and exe.inside(t) and valid(Insn(t, w)): entries.add(t)
        a += 4
    funcs = {}
    changed = True
    while changed:
        changed = False
        for e in sorted(entries):
            if e in funcs: continue
            f = analyse(exe, e, entries); funcs[e] = f
            new = set(c for c in f.calls if exe.inside(c)) | set(p for p in f.ptrs if plausible_start(exe, p))
            if new - entries: entries |= new; changed = True
    for e in sorted(entries): funcs[e] = analyse(exe, e, entries)
    return funcs

def R(n): return '0' if n == 0 else f'IR[{n}]'

def emit_simple(i):
    op, rs, rt, rd, sa, fn, imm, simm = i.op, i.rs, i.rt, i.rd, i.sa, i.fn, i.imm, i.simm
    def setd(r, e): return f'{R(r)} = {e};' if r else ''
    if op == 0:
        if i.w == 0: return ''
        m = {0: f'{R(rt)} << {sa}', 2: f'{R(rt)} >> {sa}', 3: f'(u32)((s32){R(rt)} >> {sa})',
             4: f'{R(rt)} << ({R(rs)} & 31)', 6: f'{R(rt)} >> ({R(rs)} & 31)',
             7: f'(u32)((s32){R(rt)} >> ({R(rs)} & 31))', 16: 'iop.hi', 18: 'iop.lo',
             32: f'{R(rs)} + {R(rt)}', 33: f'{R(rs)} + {R(rt)}', 34: f'{R(rs)} - {R(rt)}', 35: f'{R(rs)} - {R(rt)}',
             36: f'{R(rs)} & {R(rt)}', 37: f'{R(rs)} | {R(rt)}', 38: f'{R(rs)} ^ {R(rt)}', 39: f'~({R(rs)} | {R(rt)})',
             42: f'(s32){R(rs)} < (s32){R(rt)}', 43: f'{R(rs)} < {R(rt)}'}
        if fn in m: return setd(rd, m[fn])
        if fn == 12: return f'iop_syscall(0x{i.a:08x});'
        if fn == 13: return f'iop_break(0x{i.a:08x});'
        if fn == 17: return f'iop.hi = {R(rs)};'
        if fn == 19: return f'iop.lo = {R(rs)};'
        if fn == 24: return f'{{ s64 m = (s64)(s32){R(rs)} * (s32){R(rt)}; iop.lo = (u32)m; iop.hi = (u32)(m >> 32); }}'
        if fn == 25: return f'{{ u64 m = (u64){R(rs)} * {R(rt)}; iop.lo = (u32)m; iop.hi = (u32)(m >> 32); }}'
        if fn == 26: return f'iop_div({R(rs)}, {R(rt)});'
        if fn == 27: return f'iop_divu({R(rs)}, {R(rt)});'
    if op in (8, 9): return setd(rt, f'{R(rs)} + {simm & 0xffffffff:#x}u')
    if op == 10: return setd(rt, f'(s32){R(rs)} < {simm}')
    if op == 11: return setd(rt, f'{R(rs)} < {simm & 0xffffffff:#x}u')
    if op == 12: return setd(rt, f'{R(rs)} & {imm:#x}')
    if op == 13: return setd(rt, f'{R(rs)} | {imm:#x}')
    if op == 14: return setd(rt, f'{R(rs)} ^ {imm:#x}')
    if op == 15: return setd(rt, f'{(imm << 16):#x}u')
    if op == 16:
        if rs == 0: return setd(rt, f'iop_cop0_read({rd})')
        if rs == 4: return f'iop_cop0_write({rd}, {R(rt)});'
        return ''
    ea = f'{R(rs)} + {simm & 0xffffffff:#x}u'
    if op == 32: return setd(rt, f'(u32)(s32)(s8)ir8({ea})') or f'ir8({ea});'
    if op == 33: return setd(rt, f'(u32)(s32)(s16)ir16({ea})') or f'ir16({ea});'
    if op == 34: return setd(rt, f'ilwl({ea}, {R(rt)})')
    if op == 35: return setd(rt, f'ir32({ea})') or f'ir32({ea});'
    if op == 36: return setd(rt, f'ir8({ea})') or f'ir8({ea});'
    if op == 37: return setd(rt, f'ir16({ea})') or f'ir16({ea});'
    if op == 38: return setd(rt, f'ilwr({ea}, {R(rt)})')
    if op == 40: return f'iw8({ea}, (u8){R(rt)});'
    if op == 41: return f'iw16({ea}, (u16){R(rt)});'
    if op == 42: return f'iswl({ea}, {R(rt)});'
    if op == 43: return f'iw32({ea}, {R(rt)});'
    if op == 46: return f'iswr({ea}, {R(rt)});'
    return f'iop_bad(0x{i.a:08x}, 0x{i.w:08x}); return;'

def cond(i):
    rs, rt = R(i.rs), R(i.rt)
    if i.op == 4: return f'{rs} == {rt}'
    if i.op == 5: return f'{rs} != {rt}'
    if i.op == 6: return f'(s32){rs} <= 0'
    if i.op == 7: return f'(s32){rs} > 0'
    return f'(s32){rs} >= 0' if i.rt & 1 else f'(s32){rs} < 0'

def gen_func(exe, f, entries, out, P):
    a0 = f.entry
    out.append(f'void {P}_{a0:08x}(void) {{')
    if a0 in exe.stubs:
        lib, idx = exe.stubs[a0]
        out.append(f'  iop_import("{lib}", {idx});'); out.append('}'); out.append(''); return
    call = lambda t: f'{P}_{t:08x}();' if t in entries else f'iop_call(0x{t:08x}u);'
    addrs = sorted(f.addrs); aset = f.addrs
    needed = set(t for t in f.labels if t in aset)
    if addrs and addrs[0] != a0: needed.add(a0); out.append(f'  goto L_{a0:08x};')
    consumed = set(); prev = None
    for a in addrs:
        if prev is not None and a != prev + 4: out.append(f'  iop_fallthrough(0x{prev + 4:08x}); return;')
        prev = a
        if a in needed: out.append(f'L_{a:08x}:;')
        if a in consumed and a not in needed: continue
        i = Insn(a, exe.word(a))
        if a in consumed:
            s = emit_simple(i)
            if s: out.append('  ' + s)
            continue
        if not valid(i): out.append(f'  iop_bad(0x{a:08x}, 0x{i.w:08x}); return;'); continue
        k = kind(i)
        if k is None:
            s = emit_simple(i)
            if s: out.append('  ' + s)
            continue
        d = Insn(a + 4, exe.word(a + 4) or 0)
        ds = emit_simple(d) if kind(d) is None else ''
        consumed.add(a + 4)
        if k == 'b':
            t = btarget(i)
            if i.op == 4 and i.rs == 0 and i.rt == 0: out.append(f'  {{ {ds} ILOOP(); goto L_{t:08x}; }}')
            else: out.append(f'  {{ int c = {cond(i)}; {ds} if (c) {{ ILOOP(); goto L_{t:08x}; }} }}')
        elif k == 'bal':
            t = btarget(i)
            out.append(f'  {{ int c = {cond(i)}; IR[31] = 0x{a + 8:08x}u; {ds} if (c) {{ {call(t)} }} }}')
        elif k == 'jal': out.append(f'  {{ {ds} IR[31] = 0x{a + 8:08x}u; {call(i.tgt)} }}')
        elif k == 'jalr':
            link = f'IR[{i.rd}] = 0x{a + 8:08x}u;' if i.rd else ''
            out.append(f'  {{ u32 t = {R(i.rs)}; {ds} {link} iop_call(t); }}')
        elif k == 'j':
            t = i.tgt
            if t in aset and (t == a0 or t not in entries): out.append(f'  {{ {ds} ILOOP(); goto L_{t:08x}; }}')
            else: out.append(f'  {{ {ds} {call(t)} return; }}')
        elif k == 'jr':
            if i.rs == 31: out.append(f'  {{ {ds} return; }}')
            else:
                tab = f.tables.get(a)
                out.append(f'  {{ u32 t = {R(i.rs)}; {ds}')
                if tab:
                    out.append('    switch (t) {')
                    for t in sorted(set(tab)): out.append(f'    case 0x{t:08x}u: goto L_{t:08x};')
                    out.append('    }')
                out.append('    iop_call(t); return; }')
        if a + 4 in needed and k in ('b', 'bal', 'jal', 'jalr') and a + 8 in aset:
            needed.add(a + 8); out.append(f'  goto L_{a + 8:08x};')
    if prev is not None: out.append(f'  iop_fallthrough(0x{prev + 4:08x});')
    out.append('}'); out.append('')

def main():
    path, P, base, out_dir = sys.argv[1], sys.argv[2], int(sys.argv[3], 16), sys.argv[4]
    exe = Irx(path, base)
    funcs = discover(exe); entries = set(funcs); keys = sorted(funcs)
    os.makedirs(out_dir, exist_ok=True)
    out = ['/* generated from ' + os.path.basename(path) + ' */', '#include "iop.h"',
           '#if defined(__GNUC__)', '#pragma GCC diagnostic ignored "-Wunused-label"',
           '#pragma GCC diagnostic ignored "-Wunused-variable"', '#endif', '']
    out += [f'static void {P}_{e:08x}(void);' for e in keys]
    out.append('')
    for e in keys: gen_func(exe, funcs[e], entries, out, P)
    out = [l.replace(f'void {P}_', f'static void {P}_', 1) if l.startswith(f'void {P}_') else l for l in out]
    out.append(f'static const iop_func_entry {P}_table[] = {{')
    out += [f'  {{ 0x{e:08x}u, {P}_{e:08x} }},' for e in keys]
    out.append('};')
    out.append(f'static const u8 {P}_image[] = {{')
    d = exe.data
    for o in range(0, len(d), 32): out.append('  ' + ','.join(str(b) for b in d[o:o + 32]) + ',')
    out.append('};')
    out.append(f'const iop_module iop_mod_{P} = {{ "{os.path.basename(path).upper()}", 0x{base:08x}u, sizeof {P}_image, {P}_image,')
    out.append(f'  0x{exe.pc0:08x}u, {P}_table, {len(keys)}, 0x{exe.gp:08x}u }};')
    open(os.path.join(out_dir, f'iop_{P}.c'), 'w').write(NL.join(out) + NL)
    print(f'{P}: {len(keys)} functions ({len(exe.stubs)} import stubs), image {len(d):#x} at {base:#x}, entry {exe.pc0:#x}')

if __name__ == '__main__':
    main()
