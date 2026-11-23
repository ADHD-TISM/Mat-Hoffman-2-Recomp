"""Decode the game's built-in file table (name, first lsn, last lsn, sectors, size)
and compare it with disc/files.txt. usage: python -I tools/filetable.py"""
import struct
BS = chr(92)
d = open('../disc/SLUS_203.35', 'rb').read()[0x80:0x80 + 0x33dc00]
def cs(a):
    o = a - 0x100000; return d[o:d.index(b'\0', o)].decode('latin1')
o = 0x3f0fd0 - 0x100000
while 0x400000 <= struct.unpack_from('<I', d, o - 40)[0] < 0x430000: o -= 40
start = o
ents = []
while 0x400000 <= struct.unpack_from('<I', d, o)[0] < 0x430000:
    p, _, a, b, c, sz = struct.unpack_from('<IIQQQQ', d, o); ents.append((cs(p), a, b, c, sz)); o += 40
print('table %x..%x, %d entries' % (0x100000 + start, 0x100000 + o, len(ents)))
for e in ents[:6] + ents[-3:]: print(e)
disc = {}
for l in open('../disc/files.txt'):
    lba, sz, p = l.split(None, 2); disc[p.strip().upper()] = (int(lba), int(sz))
bad = 0
names = set()
for nm, a, b, c, sz in ents:
    k = nm.strip(BS).replace(BS, '/').split(';')[0].upper(); names.add(k)
    if k not in disc or disc[k] != (a, sz): bad += 1; print('MISMATCH', nm, a, sz, disc.get(k))
print('mismatches', bad, 'disc files', len(disc))
print('on disc but not in table:', [k for k in disc if k not in names][:20])
print(d[start - 64:start].hex())
