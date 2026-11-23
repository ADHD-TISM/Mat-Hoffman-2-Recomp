"""Quick EE disassembler: python dis.py <elf> <start_hex> <count>"""
import struct, sys, capstone
d = open(sys.argv[1], 'rb').read()
base, off = 0x100000, 0x1000
a = int(sys.argv[2], 16); n = int(sys.argv[3])
md = capstone.Cs(capstone.CS_ARCH_MIPS, capstone.CS_MODE_MIPS64 | capstone.CS_MODE_LITTLE_ENDIAN)
for k in range(n):
    o = a - base + off + 4 * k
    w = struct.unpack_from('<I', d, o)[0]
    ins = list(md.disasm(d[o:o + 4], a + 4 * k))
    s = f'{ins[0].mnemonic} {ins[0].op_str}' if ins else '???'
    print(f'{a + 4*k:08x}: {w:08x}  {s}')
