#!/usr/bin/env python3
"""cover_util.py: PS_PCCOVER bitmaps (bit (a >> 1) & 7 of byte a >> 4 = PC a executed), for cover_loop.sh.

  cover_util.py union OUT FILE...          OR of the bitmaps (missing files skipped)
  cover_util.py gain NEW BASE [ROMSIZE]    instructions set in NEW and not in BASE (below ROMSIZE)
  cover_util.py routes TITLE               recordings to explore from, one per line, longest first
"""
import glob
import os
import sys

N = 0x400000 // 16


def load(p):
    try:
        b = open(p, 'rb').read()
    except OSError:
        return bytearray(N)
    return bytearray(b[:N].ljust(N, b'\0'))


def union(out, files):
    acc = int.from_bytes(bytes(N), 'big')
    for f in files:
        acc |= int.from_bytes(load(f), 'big')
    tmp = out + '.tmp'
    open(tmp, 'wb').write(acc.to_bytes(N, 'big'))
    os.replace(tmp, out)


def gain(new, base, romsize=0x400000):
    a, b = load(new), load(base)
    n = int.from_bytes(a[:(romsize + 15) // 16], 'big') & ~int.from_bytes(b[:(romsize + 15) // 16], 'big')
    return bin(n).count('1')


def routes(title):
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    g = os.path.join(root, 'games', title)
    ps = glob.glob(os.path.join(g, 'recordings', '*.polls')) + glob.glob(os.path.join(g, 'sessions', '*.polls')) \
        + glob.glob(os.path.expanduser(f'~/Library/Application Support/PixelSorcery/{title}/sessions/*.polls'))
    ps = [p for p in ps if os.path.getsize(p) > 120 * 10]
    return sorted(ps, key=os.path.getsize, reverse=True)


if __name__ == '__main__':
    cmd = sys.argv[1]
    if cmd == 'union':
        union(sys.argv[2], sys.argv[3:])
    elif cmd == 'gain':
        print(gain(sys.argv[2], sys.argv[3], int(sys.argv[4], 0) if len(sys.argv) > 4 else 0x400000))
    elif cmd == 'routes':
        print('\n'.join(routes(sys.argv[2])))
