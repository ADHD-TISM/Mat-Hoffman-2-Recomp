"""Extract an ISO9660 image (2048-byte sectors) to a folder. Stdlib only.

usage: python -I tools/isoextract.py IMAGE.iso OUTDIR
Writes OUTDIR/files.txt: one line per file, "lba size path".
"""
import os
import struct
import sys

SECTOR = 2048


def read_sector(f, lba, count=1):
    f.seek(lba * SECTOR)
    return f.read(count * SECTOR)


def parse_dir(f, lba, size, prefix, out, listing):
    data = read_sector(f, lba, (size + SECTOR - 1) // SECTOR)
    pos = 0
    while pos < size:
        rlen = data[pos]
        if rlen == 0:
            pos = (pos // SECTOR + 1) * SECTOR  # records never cross sectors
            continue
        rec = data[pos:pos + rlen]
        ext_lba = struct.unpack_from("<I", rec, 2)[0]
        ext_size = struct.unpack_from("<I", rec, 10)[0]
        flags = rec[25]
        nlen = rec[32]
        name = rec[33:33 + nlen]
        pos += rlen
        if name in (b"\x00", b"\x01"):
            continue
        name = name.decode("ascii", "replace").split(";")[0]
        path = prefix + name
        if flags & 2:
            os.makedirs(os.path.join(out, path), exist_ok=True)
            parse_dir(f, ext_lba, ext_size, path + "/", out, listing)
        else:
            listing.append((ext_lba, ext_size, path))
            with open(os.path.join(out, path), "wb") as o:
                f.seek(ext_lba * SECTOR)
                left = ext_size
                while left:
                    chunk = f.read(min(left, 1 << 24))
                    if not chunk:
                        raise SystemExit("short read in " + path)
                    o.write(chunk)
                    left -= len(chunk)


def main():
    img, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    with open(img, "rb") as f:
        pvd = read_sector(f, 16)
        if pvd[1:6] != b"CD001" or pvd[0] != 1:
            raise SystemExit("no primary volume descriptor at sector 16")
        root = pvd[156:156 + 34]
        lba = struct.unpack_from("<I", root, 2)[0]
        size = struct.unpack_from("<I", root, 10)[0]
        listing = []
        parse_dir(f, lba, size, "", out, listing)
    with open(os.path.join(out, "files.txt"), "w") as o:
        for lba, size, path in sorted(listing):
            o.write("%d %d %s\n" % (lba, size, path))
    print("extracted %d files" % len(listing))


if __name__ == "__main__":
    main()
