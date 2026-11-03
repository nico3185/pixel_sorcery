#!/usr/bin/env python3
"""ckcmp.py A B: compare two PS_CHECKPOINTS files (main RAM hashed in 256-byte blocks at each controller
poll). Prints the polls compared, how many differ, and the first differing poll with its frame on each
side and the RAM blocks that differ ($FFxx00)."""
import sys

def load(p):
    out = []
    for line in open(p):
        w = line.split()
        if len(w) == 258: out.append((int(w[0]), w[1][2:], w[2:]))
    return out

a, b = load(sys.argv[1]), load(sys.argv[2])
n = min(len(a), len(b)); bad = [i for i in range(n) if a[i][2] != b[i][2]]
msg = f'polls {n} ({len(a)}/{len(b)}) differ {len(bad)}' + (' POLL COUNTS DIFFER' if abs(len(a) - len(b)) > 2 else '')
if bad:
    i = bad[0]; blocks = [f'{0xFF0000 + j * 256:06X}' for j in range(256) if a[i][2][j] != b[i][2][j]]
    msg += f' first poll {a[i][0]} frame {a[i][1]}/{b[i][1]} blocks {" ".join(blocks[:8])}{" ..." if len(blocks) > 8 else ""}'
print(msg)
