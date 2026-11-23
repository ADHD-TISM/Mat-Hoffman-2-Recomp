#!/usr/bin/env python3
"""Write vu_recipe.json: where each recorded VU1 code snapshot (gen_vu/vu1_*.bin)
is found inside the game's own files, so the launcher can rebuild the snapshots
from the user's disc and the repository carries no game code.

    python -I tools/mkvurecipe.py gen_vu vu_recipe.json ../disc/SLUS_203.35 [more disc files]

Run it again after recording new VU1 programs (VU_RECORD) and adding them to gen_vu/.
"""
import sys, os, json, hashlib, glob


def find_pieces(code, srcs):
    """Cover every non-zero 8-byte word of `code` with runs copied from one of srcs."""
    pieces, o, n = [], 0, len(code)
    while o < n:
        if code[o:o + 8] == bytes(8):
            o += 8
            continue
        best = None
        for name, src in srcs:
            for seed in (64, 32, 16, 8):
                key = code[o:o + seed]
                at = src.find(key)
                if at < 0:
                    continue
                ln = len(key)
                while o + ln < n and at + ln < len(src) and code[o + ln] == src[at + ln]:
                    ln += 1
                ln &= ~7                         # whole instruction pairs only
                if ln and (best is None or ln > best[2]):
                    best = (name, at, ln)
                break
        if best is None:
            return None, o
        pieces.append({"vu": o, "from_file": best[0], "file": best[1], "len": best[2]})
        o += best[2]
    return pieces, 0


def main(argv):
    if len(argv) < 4:
        sys.exit(__doc__)
    gen_vu, out, files = argv[1], argv[2], argv[3:]
    srcs = []
    for f in files:
        with open(f, "rb") as fh:
            srcs.append((os.path.basename(f), fh.read()))
    snaps, bad = [], 0
    for b in sorted(glob.glob(os.path.join(gen_vu, "vu1_*.bin"))):
        with open(b, "rb") as fh:
            code = fh.read()
        with open(b[:-4] + ".pcs") as fh:
            pcs = fh.read().split()
        pieces, at = find_pieces(code, srcs)
        if pieces is None:
            print("%s: code at VU offset 0x%x is not in the given files" % (os.path.basename(b), at))
            bad += 1
            continue
        snaps.append({"hash": os.path.basename(b)[4:-4], "size": len(code),
                      "sha256": hashlib.sha256(code).hexdigest(), "pieces": pieces, "pcs": pcs})
    if bad:
        sys.exit("%d snapshot(s) not found: name the disc file that holds them" % bad)
    doc = {"comment": "VU1 code snapshots rebuilt from the game's files at build time (the launcher reads this); "
                      "vu = offset in VU1 code memory, file = offset in from_file",
           "snapshots": snaps}
    with open(out, "w", newline="\n") as fh:
        json.dump(doc, fh, indent=1)
        fh.write("\n")
    print("%d snapshots, %d pieces -> %s" % (len(snaps), sum(len(s["pieces"]) for s in snaps), out))


if __name__ == "__main__":
    main(sys.argv)
