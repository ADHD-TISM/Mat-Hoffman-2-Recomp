"""vudis.py <micro.bin> [start_hex [count]] : disassemble VU micro code (64-bit pairs).

Dump the code with VU_DUMP=<dir> (runtime writes vu1_code.bin / vu1_mem.bin
at the first VU1 run that XGKICKs). Upper | lower per line, E/I bits shown.
"""
import sys, struct

BC = 'xyzw'
def dest(w):
    d = (w >> 21) & 15
    return ''.join(c for i, c in enumerate('xyzw') if d & (8 >> i))

def upper(w):
    fn = w & 63; ft = (w >> 16) & 31; fs = (w >> 11) & 31; fd = (w >> 6) & 31; bc = w & 3; d = dest(w)
    names = {0: 'add', 1: 'sub', 2: 'madd', 3: 'msub', 4: 'max', 5: 'mini', 6: 'mul'}
    if fn < 0x1c:
        g = fn >> 2
        n = names.get(g, None)
        if n is None: return '?u%08x' % w
        return '%s%s.%s vf%d, vf%d, vf%d%s' % (n, BC[bc], d, fd, fs, ft, BC[bc])
    single = {0x1c: ('mulq', 'q'), 0x1d: ('maxi', 'i'), 0x1e: ('muli', 'i'), 0x1f: ('minii', 'i'),
              0x20: ('addq', 'q'), 0x21: ('maddq', 'q'), 0x22: ('addi', 'i'), 0x23: ('maddi', 'i'),
              0x24: ('subq', 'q'), 0x25: ('msubq', 'q'), 0x26: ('subi', 'i'), 0x27: ('msubi', 'i')}
    if fn in single:
        n, r = single[fn]; return '%s.%s vf%d, vf%d, %s' % (n, d, fd, fs, r)
    three = {0x28: 'add', 0x29: 'madd', 0x2a: 'mul', 0x2b: 'max', 0x2c: 'sub', 0x2d: 'msub', 0x2e: 'opmsub', 0x2f: 'mini'}
    if fn in three: return '%s.%s vf%d, vf%d, vf%d' % (three[fn], d, fd, fs, ft)
    if fn < 0x3c: return '?u%08x' % w
    op = ((w >> 4) & 0x7c) | bc
    if op < 0x10:
        n = ['adda', 'suba', 'madda', 'msuba'][op >> 2]
        return '%s%s.%s acc, vf%d, vf%d%s' % (n, BC[bc], d, fs, ft, BC[bc])
    if op < 0x14: return 'itof%s.%s vf%d, vf%d' % (['0', '4', '12', '15'][op & 3], d, ft, fs)
    if op < 0x18: return 'ftoi%s.%s vf%d, vf%d' % (['0', '4', '12', '15'][op & 3], d, ft, fs)
    if op < 0x1c: return 'mula%s.%s acc, vf%d, vf%d%s' % (BC[bc], d, fs, ft, BC[bc])
    ex = {0x1c: 'mulaq', 0x1d: 'abs', 0x1e: 'mulai', 0x1f: 'clipw', 0x20: 'addaq', 0x21: 'maddaq', 0x22: 'addai',
          0x23: 'maddai', 0x24: 'subaq', 0x25: 'msubaq', 0x26: 'subai', 0x27: 'msubai', 0x28: 'adda', 0x29: 'madda',
          0x2a: 'mula', 0x2c: 'suba', 0x2d: 'msuba', 0x2e: 'opmula', 0x2f: 'nop'}
    n = ex.get(op)
    if n is None: return '?u%08x' % w
    if n == 'nop': return 'nop'
    if n == 'abs': return 'abs.%s vf%d, vf%d' % (d, ft, fs)
    if n == 'clipw': return 'clipw.xyz vf%d, vf%d' % (fs, ft)
    return '%s.%s acc, vf%d, vf%d' % (n, d, fs, ft)

def s11(w):
    x = w & 0x7ff
    return x - 0x800 if x & 0x400 else x

def lower(w, pc):
    op = w >> 25; it = (w >> 16) & 31; is_ = (w >> 11) & 31; id_ = (w >> 6) & 31; d = dest(w)
    imm15 = ((w >> 10) & 0x7800) | (w & 0x7ff); imm12 = ((w >> 10) & 0x800) | (w & 0x7ff)
    br = lambda: '%04x' % ((pc + 8 + s11(w) * 8) & 0x3fff)
    t = {
        0x00: lambda: 'lq.%s vf%d, %d(vi%d)' % (d, it, s11(w), is_),
        0x01: lambda: 'sq.%s vf%d, %d(vi%d)' % (d, is_, s11(w), it),
        0x04: lambda: 'ilw.%s vi%d, %d(vi%d)' % (d, it, s11(w), is_),
        0x05: lambda: 'isw.%s vi%d, %d(vi%d)' % (d, it, s11(w), is_),
        0x08: lambda: 'iaddiu vi%d, vi%d, 0x%x' % (it, is_, imm15),
        0x09: lambda: 'isubiu vi%d, vi%d, 0x%x' % (it, is_, imm15),
        0x10: lambda: 'fceq vi1, 0x%06x' % (w & 0xffffff),
        0x11: lambda: 'fcset 0x%06x' % (w & 0xffffff),
        0x12: lambda: 'fcand vi1, 0x%06x' % (w & 0xffffff),
        0x13: lambda: 'fcor vi1, 0x%06x' % (w & 0xffffff),
        0x14: lambda: 'fseq vi%d, 0x%x' % (it, imm12),
        0x15: lambda: 'fsset 0x%x' % imm12,
        0x16: lambda: 'fsand vi%d, 0x%x' % (it, imm12),
        0x17: lambda: 'fsor vi%d, 0x%x' % (it, imm12),
        0x18: lambda: 'fmeq vi%d, vi%d' % (it, is_),
        0x1a: lambda: 'fmand vi%d, vi%d' % (it, is_),
        0x1b: lambda: 'fmor vi%d, vi%d' % (it, is_),
        0x1c: lambda: 'fcget vi%d' % it,
        0x20: lambda: 'b ' + br(),
        0x21: lambda: 'bal vi%d, %s' % (it, br()),
        0x24: lambda: 'jr vi%d' % is_,
        0x25: lambda: 'jalr vi%d, vi%d' % (it, is_),
        0x28: lambda: 'ibeq vi%d, vi%d, %s' % (it, is_, br()),
        0x29: lambda: 'ibne vi%d, vi%d, %s' % (it, is_, br()),
        0x2c: lambda: 'ibltz vi%d, %s' % (is_, br()),
        0x2d: lambda: 'ibgtz vi%d, %s' % (is_, br()),
        0x2e: lambda: 'iblez vi%d, %s' % (is_, br()),
        0x2f: lambda: 'ibgez vi%d, %s' % (is_, br()),
    }
    if op in t: return t[op]()
    if op != 0x40: return '?l%08x' % w
    fn = w & 63
    if fn == 0x30: return 'iadd vi%d, vi%d, vi%d' % (id_, is_, it)
    if fn == 0x31: return 'isub vi%d, vi%d, vi%d' % (id_, is_, it)
    if fn == 0x32:
        imm = (w >> 6) & 31; imm = imm - 32 if imm & 16 else imm
        return 'iaddi vi%d, vi%d, %d' % (it, is_, imm)
    if fn == 0x34: return 'iand vi%d, vi%d, vi%d' % (id_, is_, it)
    if fn == 0x35: return 'ior vi%d, vi%d, vi%d' % (id_, is_, it)
    if fn < 0x3c: return '?l%08x' % w
    x = ((w >> 4) & 0x7c) | (w & 3); fsf = BC[(w >> 21) & 3]; ftf = BC[(w >> 23) & 3]
    e = {
        0x30: 'move.%s vf%d, vf%d' % (d, it, is_), 0x31: 'mr32.%s vf%d, vf%d' % (d, it, is_),
        0x34: 'lqi.%s vf%d, (vi%d++)' % (d, it, is_), 0x35: 'sqi.%s vf%d, (vi%d++)' % (d, is_, it),
        0x36: 'lqd.%s vf%d, (--vi%d)' % (d, it, is_), 0x37: 'sqd.%s vf%d, (--vi%d)' % (d, is_, it),
        0x38: 'div q, vf%d%s, vf%d%s' % (is_, fsf, it, ftf), 0x39: 'sqrt q, vf%d%s' % (it, ftf),
        0x3a: 'rsqrt q, vf%d%s, vf%d%s' % (is_, fsf, it, ftf), 0x3b: 'waitq',
        0x3c: 'mtir vi%d, vf%d%s' % (it, is_, fsf), 0x3d: 'mfir.%s vf%d, vi%d' % (d, it, is_),
        0x3e: 'ilwr.%s vi%d, (vi%d)' % (d, it, is_), 0x3f: 'iswr.%s vi%d, (vi%d)' % (d, it, is_),
        0x40: 'rnext.%s vf%d, r' % (d, it), 0x41: 'rget.%s vf%d, r' % (d, it),
        0x42: 'rinit r, vf%d%s' % (is_, fsf), 0x43: 'rxor r, vf%d%s' % (is_, fsf),
        0x64: 'mfp.%s vf%d, p' % (d, it), 0x68: 'xtop vi%d' % it, 0x69: 'xitop vi%d' % it,
        0x6c: 'xgkick vi%d' % is_, 0x70: 'esadd p, vf%d' % is_, 0x71: 'ersadd p, vf%d' % is_,
        0x72: 'eleng p, vf%d' % is_, 0x73: 'erleng p, vf%d' % is_, 0x76: 'esum p, vf%d' % is_,
        0x78: 'esqrt p, vf%d%s' % (is_, fsf), 0x79: 'ersqrt p, vf%d%s' % (is_, fsf),
        0x7a: 'ercpr p, vf%d%s' % (is_, fsf), 0x7b: 'waitp', 0x7c: 'esin p, vf%d%s' % (is_, fsf),
        0x7d: 'eatan p, vf%d%s' % (is_, fsf), 0x7e: 'eexp p, vf%d%s' % (is_, fsf),
    }
    return e.get(x, '?l%08x' % w)

def main():
    code = open(sys.argv[1], 'rb').read()
    start = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0
    count = int(sys.argv[3]) if len(sys.argv) > 3 else (len(code) - start) // 8
    for k in range(count):
        pc = start + 8 * k
        if pc + 8 > len(code): break
        lo, hi = struct.unpack_from('<II', code, pc)
        flags = ('E' if hi & 0x40000000 else ' ') + ('I' if hi & 0x80000000 else ' ')
        lw = ('i=%08x (%g)' % (lo, struct.unpack('<f', struct.pack('<I', lo))[0])) if hi & 0x80000000 else lower(lo, pc)
        print('%04x %s %-40s | %s' % (pc, flags, upper(hi), lw))

if __name__ == '__main__':
    main()
