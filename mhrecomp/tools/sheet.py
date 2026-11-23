"""Stack PPM screenshots (GS_SHOT) into one PNG contact sheet. Stdlib only.
usage: python -I tools/sheet.py OUT.png COLS FILE.ppm [FILE.ppm ...]"""
import os, struct, sys, zlib
DIV = int(os.environ.get('SHEET_DIV', '1'))

def read_ppm(p):
    d = open(p, 'rb').read()
    parts = d.split(None, 4)
    w, h = int(parts[1]), int(parts[2])
    px = parts[4][:w * h * 3]
    if DIV > 1:   # SHEET_DIV=N: keep every Nth pixel and line (big sheets do not open in the viewer)
        rows = [px[y * w * 3:(y + 1) * w * 3] for y in range(0, h, DIV)]
        rows = [b''.join(r[x * 3:x * 3 + 3] for x in range(0, w, DIV)) for r in rows]
        w, h, px = len(rows[0]) // 3, len(rows), b''.join(rows)
    return w, h, px

def main():
    out, cols, files = sys.argv[1], int(sys.argv[2]), sys.argv[3:]
    imgs = [read_ppm(f) for f in files]
    cw = max(i[0] for i in imgs); ch = max(i[1] for i in imgs)
    rows = (len(imgs) + cols - 1) // cols
    W, H = cw * cols, ch * rows
    buf = bytearray(W * H * 3)
    for n, (w, h, px) in enumerate(imgs):
        x0, y0 = (n % cols) * cw, (n // cols) * ch
        for y in range(h):
            o = ((y0 + y) * W + x0) * 3
            buf[o:o + w * 3] = px[y * w * 3:(y + 1) * w * 3]
    raw = b''.join(b'\0' + bytes(buf[y * W * 3:(y + 1) * W * 3]) for y in range(H))
    def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d))
    open(out, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))
    print(out, W, H, len(imgs))

main()
