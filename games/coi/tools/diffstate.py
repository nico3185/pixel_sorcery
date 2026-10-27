"""Compare --ram-dump snapshots from genrecomp_ref (ground truth) and the recomp.
Usage: diffstate.py REFDIR RECDIR [addr ...]  -- per frame: diff counts per region,
plus the given RAM words (e.g. F300 game state)."""
import os, sys
ref, rec = sys.argv[1], sys.argv[2]
watch = [int(a, 16) for a in sys.argv[3:]]
REG = [("ram", 0, 0x10000), ("vdpreg", 0x10000, 0x20), ("cram", 0x10020, 0x80),
       ("vsram", 0x100A0, 0x80), ("vram", 0x10120, 0x10000)]
def word(b, a):  # GPGX keeps work RAM LSB_FIRST-swapped: word at even addr is little-endian
    return int.from_bytes(b[a:a + 2], "little")
for name in sorted(os.listdir(ref)):
    p = os.path.join(rec, name)
    if not os.path.exists(p): continue
    a, b = open(os.path.join(ref, name), "rb").read(), open(p, "rb").read()
    parts = []
    for rn, off, ln in REG:
        d = sum(1 for i in range(off, off + ln) if a[i] != b[i])
        parts.append(f"{rn}:{d}")
    w = " ".join(f"{x:04X}={word(a, x):04X}/{word(b, x):04X}" for x in watch)
    print(name[4:10], " ".join(parts), w)
