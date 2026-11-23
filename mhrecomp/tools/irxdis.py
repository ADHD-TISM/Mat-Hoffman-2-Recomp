"""Disassemble an IOP module (IRX, MIPS R3000) with import stubs named.

usage: python irxdis.py <file.irx> [start_hex end_hex]
Addresses are module-relative (load base 0). Calls to import stubs print as
library:index, e.g. sifcmd_17. Relocations are not applied, so a lui/addiu
pair shows the module-relative address it points at.
"""
import struct, sys

R = ['zero','at','v0','v1','a0','a1','a2','a3','t0','t1','t2','t3','t4','t5','t6','t7',
     's0','s1','s2','s3','s4','s5','s6','s7','t8','t9','k0','k1','gp','sp','fp','ra']

def load(path):
    d = open(path, 'rb').read()
    shoff, = struct.unpack_from('<I', d, 0x20)
    shnum, shstr = struct.unpack_from('<HH', d, 0x30)
    secs = [struct.unpack_from('<10I', d, shoff + 40 * k) for k in range(shnum)]
    so = secs[shstr][4]
    name = lambda n: d[so + n:d.index(b'\0', so + n)].decode()
    image = bytearray(0x100000)
    top = 0
    for s in secs:
        nm, typ, fl, addr, off, size = s[:6]
        if fl & 2 and typ != 8:
            image[addr:addr + size] = d[off:off + size]
            top = max(top, addr + size)
        elif fl & 2:
            top = max(top, addr + size)
    text = [s for s in secs if name(s[0]) == '.text'][0]
    return bytes(image[:top]), text[3], text[3] + text[5]

def stubs(img, lo, hi):
    out = {}
    a = lo
    while a < hi - 20:
        w, = struct.unpack_from('<I', img, a)
        if w == 0x41e00000:
            lib = img[a + 12:a + 20].split(b'\0')[0].decode('ascii', 'replace')
            b = a + 20
            while b < hi:
                j, k = struct.unpack_from('<II', img, b)
                if j != 0x03e00008: break
                out[b] = f'{lib}_{k & 0xffff}'
                b += 8
            a = b
        else:
            a += 4
    return out

def dis(w, a, names):
    op, rs, rt, rd, sa, fn = w >> 26, (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31, w & 63
    imm = w & 0xffff; simm = imm - 0x10000 if imm & 0x8000 else imm
    bt = (a + 4 + (simm << 2)) & 0xffffffff
    if w == 0: return 'nop'
    if op == 0:
        m = {0:'sll',2:'srl',3:'sra'}
        if fn in m: return f'{m[fn]} {R[rd]},{R[rt]},{sa}'
        m = {4:'sllv',6:'srlv',7:'srav'}
        if fn in m: return f'{m[fn]} {R[rd]},{R[rt]},{R[rs]}'
        if fn == 8: return f'jr {R[rs]}'
        if fn == 9: return f'jalr {R[rd]},{R[rs]}'
        if fn == 12: return 'syscall'
        if fn == 13: return 'break'
        m = {16:'mfhi',18:'mflo'}
        if fn in m: return f'{m[fn]} {R[rd]}'
        m = {17:'mthi',19:'mtlo'}
        if fn in m: return f'{m[fn]} {R[rs]}'
        m = {24:'mult',25:'multu',26:'div',27:'divu'}
        if fn in m: return f'{m[fn]} {R[rs]},{R[rt]}'
        m = {32:'add',33:'addu',34:'sub',35:'subu',36:'and',37:'or',38:'xor',39:'nor',42:'slt',43:'sltu'}
        if fn in m:
            if fn == 33 and rt == 0: return f'move {R[rd]},{R[rs]}'
            return f'{m[fn]} {R[rd]},{R[rs]},{R[rt]}'
    if op == 1:
        m = {0:'bltz',1:'bgez',16:'bltzal',17:'bgezal'}
        return f'{m.get(rt, "regimm?")} {R[rs]},{bt:x}'
    if op in (2, 3):
        t = (a & 0xf0000000) | ((w & 0x3ffffff) << 2)
        return f'{"j" if op == 2 else "jal"} {names.get(t, f"{t:x}")}'
    if op in (4, 5):
        if op == 4 and rs == 0 and rt == 0: return f'b {bt:x}'
        return f'{"beq" if op == 4 else "bne"} {R[rs]},{R[rt]},{bt:x}'
    if op in (6, 7): return f'{"blez" if op == 6 else "bgtz"} {R[rs]},{bt:x}'
    m = {8:'addi',9:'addiu',10:'slti',11:'sltiu'}
    if op in m:
        if op == 9 and rs == 0: return f'li {R[rt]},{simm}'
        return f'{m[op]} {R[rt]},{R[rs]},{simm}'
    m = {12:'andi',13:'ori',14:'xori'}
    if op in m: return f'{m[op]} {R[rt]},{R[rs]},{imm:#x}'
    if op == 15: return f'lui {R[rt]},{imm:#x}'
    if op == 16: return f'cop0 {w:08x}'
    m = {32:'lb',33:'lh',34:'lwl',35:'lw',36:'lbu',37:'lhu',38:'lwr',40:'sb',41:'sh',42:'swl',43:'sw',46:'swr'}
    if op in m: return f'{m[op]} {R[rt]},{simm}({R[rs]})'
    return f'.word {w:08x}'

def main():
    img, lo, hi = load(sys.argv[1])
    names = stubs(img, lo, hi)
    a0 = int(sys.argv[2], 16) if len(sys.argv) > 2 else lo
    a1 = int(sys.argv[3], 16) if len(sys.argv) > 3 else hi
    for a in range(a0, a1, 4):
        w, = struct.unpack_from('<I', img, a)
        tag = f'  <{names[a]}>' if a in names else ''
        print(f'{a:06x}: {w:08x}  {dis(w, a, names)}{tag}')

if __name__ == '__main__':
    main()
