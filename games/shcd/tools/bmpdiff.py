#!/usr/bin/env python3
"""bmpdiff.py DIR_A DIR_B: differing pixels between same-named BMP frames of two directories (24/32-bit)."""
import os, struct, sys

def rd(p):
    d = open(p, 'rb').read(); off, = struct.unpack_from('<I', d, 10); w, h = struct.unpack_from('<ii', d, 18)
    bp = struct.unpack_from('<H', d, 28)[0] // 8; stride = (w * bp + 3) & ~3
    return w, abs(h), [d[off + y * stride: off + y * stride + w * bp] for y in range(abs(h))], bp

out = []
for f in sorted(os.listdir(sys.argv[1])):
    b = os.path.join(sys.argv[2], f)
    if not f.endswith('.bmp') or not os.path.exists(b): continue
    wa, ha, ra, bp = rd(os.path.join(sys.argv[1], f)); wb, hb, rb, _ = rd(b)
    if (wa, ha) != (wb, hb): out.append(f'{f}:size'); continue
    n = sum(1 for y in range(ha) if ra[y] != rb[y] for x in range(wa) if ra[y][x * bp:x * bp + 3] != rb[y][x * bp:x * bp + 3])
    out.append(f'{f[:-4]}:{n}')
print(' '.join(out))
