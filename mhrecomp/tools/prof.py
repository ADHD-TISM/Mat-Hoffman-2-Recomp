"""prof.py <exe> <prof.txt> [N]: resolve runtime/prof.c samples to functions.

Uses llvm-nm (on PATH) for the symbol table. Prints the top N functions
(default 40) and the share per source group (gen EE code, gen_iop, runtime file).
"""
import bisect, subprocess, sys, collections

exe, prof = sys.argv[1], sys.argv[2]
top = int(sys.argv[3]) if len(sys.argv) > 3 else 40

nm = subprocess.run(['llvm-nm', '--defined-only', '-n', '-l', exe], capture_output=True, text=True).stdout
# image base: first text symbol address minus its RVA is not given by nm; read the PE header
d = open(exe, 'rb').read(4096)
pe = int.from_bytes(d[0x3c:0x40], 'little')
image_base = int.from_bytes(d[pe + 24 + 24:pe + 24 + 32], 'little')
image_size = int.from_bytes(d[pe + 24 + 56:pe + 24 + 60], 'little')

addrs, names, files = [], [], []
for line in nm.splitlines():
    parts = line.split(None, 3)
    if len(parts) < 3 or parts[1] not in 'tT':
        continue
    addrs.append(int(parts[0], 16) - image_base)
    names.append(parts[2].split('\t')[0])
    loc = line.split('\t')[1] if '\t' in line else ''
    files.append(loc.replace('\\', '/').rsplit(':', 1)[0].rsplit('/', 2)[-2:] if loc else [])

byfn = collections.Counter()
bygroup = collections.Counter()
total = 0
for line in open(prof):
    parts = line.split()
    off, n = int(parts[0], 16), int(parts[1])
    total += n
    if len(parts) > 2 or off >= image_size:
        mod = parts[2].split('+')[0] if len(parts) > 2 else '?'
        byfn['(dll) ' + mod] += n
        bygroup['dll ' + mod] += n
        continue
    i = bisect.bisect_right(addrs, off) - 1
    if i < 0:
        byfn['?'] += n; bygroup['?'] += n; continue
    name = names[i]
    byfn[name] += n
    f = files[i]
    if name.startswith('fn_'):
        g = 'EE recompiled (gen)'
    elif name.startswith('iop_fn') or (f and f[0] == 'gen_iop'):
        g = 'IOP recompiled'
    elif f:
        g = '/'.join(f)
    else:
        g = 'other (' + name + ')' if n * 50 > total else 'other'
    bygroup[g] += n

print(f'{total} samples')
print('-- by group')
for g, n in bygroup.most_common(25):
    print(f'{100.0 * n / total:6.2f}%  {g}')
print('-- by function')
for f, n in byfn.most_common(top):
    print(f'{100.0 * n / total:6.2f}%  {f}')
