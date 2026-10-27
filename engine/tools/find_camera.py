#!/usr/bin/env python3
"""find_camera.py DUMPDIR [--from FRAME]: candidate widescreen hooks for a title, from RAM
dumps (tracer PS_RAMDUMP_EVERY=N:DIR: work RAM, VRAM, CRAM, VDP registers per sample).

The camera is the RAM word whose value follows plane A's scroll position across samples:
  x: horizontal scroll of plane A at line 0 (VRAM hscroll table, register 13), negated;
  y: vertical scroll of plane A (the game's VSRAM copy is not in the dump, so y candidates
     are RAM words that change with x's samples' frames less tightly - listed by spread).
A candidate matches when camera = scroll + constant on (almost) every sample where the
scroll moved; ranked by how many samples agree. Also lists bytes that stay constant while
the camera moves and change between rooms (map id candidates) and the most common values
of a word at the game-mode candidates. Use on a run that plays (moves the camera) a while.
"""
import glob
import os
import sys
from collections import Counter


def load(path):
    r = open(path, 'rb').read()
    ram, vram, reg = r[:0x10000], r[0x10000:0x20000], r[0x20080:0x20098]
    hs = (reg[13] & 0x3F) << 10
    mode = reg[11] & 3                                  # 0 full, 2 per cell, 3 per line
    w = {0: 256, 1: 512, 3: 1024}.get(reg[16] & 3, 256)   # plane width (register 16)
    a = int.from_bytes(vram[hs:hs + 2], 'big') & (w - 1)  # plane A, line 0
    hgt = {0: 256, 1: 512, 3: 1024}.get(reg[16] >> 4 & 3, 256)   # plane height
    vs = r[0x200A0:0x20120]                              # VSRAM (host byte order: little end)
    y = int.from_bytes(vs[0:2], 'little') & (hgt - 1) if len(vs) >= 2 else None
    return ram, (-a) & (w - 1), mode, w, y, hgt


def main():
    d = sys.argv[1]
    lo = int(sys.argv[sys.argv.index('--from') + 1]) if '--from' in sys.argv else 0
    th = float(sys.argv[sys.argv.index('--min') + 1]) if '--min' in sys.argv else 0.8   # agreement
    fs = sorted(f for f in glob.glob(os.path.join(d, 'f*.ram')) if int(os.path.basename(f)[1:7]) >= lo)
    samples = [load(f) for f in fs]
    w = Counter(s[3] for s in samples).most_common(1)[0][0]
    print(f'{len(samples)} samples; hscroll modes {Counter(s[2] for s in samples)}; plane {w} px wide')
    xs = [s[1] for s in samples]
    moved = [i for i in range(1, len(xs)) if xs[i] != xs[i - 1]]
    print(f'plane A scroll moved on {len(moved)} samples')
    if not moved:
        sys.exit('the camera never moved: record a run that plays')
    res = []
    for addr in range(0, 0x10000, 2):
        vals = [int.from_bytes(s[0][addr:addr + 2], 'big') for s in samples]
        diffs = Counter(((v - x) & (w - 1)) for v, x in zip(vals, xs))
        k, c = diffs.most_common(1)[0]
        moving = sum(1 for i in moved if vals[i] != vals[i - 1])
        if moving >= len(moved) * th and c >= len(samples) * th:
            res.append((c, moving, addr, k, len(set(vals))))
    res.sort(reverse=True)
    ys = [s[4] for s in samples]; hgt = samples[0][5]
    ymoved = [i for i in range(1, len(ys)) if ys[0] is not None and ys[i] != ys[i - 1]]
    resy = []
    for addr in range(0, 0x10000, 2) if ymoved else []:
        vals = [int.from_bytes(s[0][addr:addr + 2], 'big') for s in samples]
        diffs = Counter(((v - y) & (hgt - 1)) for v, y in zip(vals, ys))
        k, c = diffs.most_common(1)[0]
        moving = sum(1 for i in ymoved if vals[i] != vals[i - 1])
        if moving >= len(ymoved) * th and c >= len(samples) * th:
            resy.append((c, moving, addr, k, len(set(vals))))
    resy.sort(reverse=True)
    print(f'plane A vscroll moved on {len(ymoved)} samples; camera Y candidates:')
    for c, m, a, k, nv in resy[:8]:
        print(f'  {c:4d} {m:4d}  $FF{a:04X}  +{k:3d}  {nv}')
    print('camera X candidates (agreeing samples, moving, $FFxxxx, offset mod plane width, distinct values):')
    for c, m, a, k, nv in res[:12]:
        print(f'  {c:4d} {m:4d}  $FF{a:04X}  +{k:3d}  {nv}')
    # game state: bytes with few values, constant across most moving samples
    print('few-valued bytes (game mode / map candidates: value counts):')
    out = []
    for addr in range(0xF000, 0x10000):
        vals = [s[0][addr] for s in samples]
        cnt = Counter(vals)
        if 2 <= len(cnt) <= 6 and cnt.most_common(1)[0][1] < len(vals):
            out.append((addr, dict(cnt.most_common(4))))
    for a, c in out[:40]:
        print(f'  $FF{a:04X} {c}')


if __name__ == '__main__':
    main()
