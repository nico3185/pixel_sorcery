#!/usr/bin/env python3
"""Carry entry points found on one dump of a game over to another dump (other revision or
region), so a recording made on either one adds coverage to both.

Each entry point's first N code bytes in FROM_ROM are searched in TO_ROM; an entry point
whose bytes occur exactly once there is written at that address (one 0xADDRESS per line,
the format merge_seeds.py reads). Byte sequences found zero or several times are skipped.
Matches are candidates: merge them, then check_seeds.py and the self-check decide.

Usage: translate_seeds.py FROM_ROM TO_ROM coverage.seeds [-n 24] > translated.seeds
"""
import argparse, re, sys

ap = argparse.ArgumentParser()
ap.add_argument('src'); ap.add_argument('dst'); ap.add_argument('seeds')
ap.add_argument('-n', type=int, default=24, help='signature length in bytes (default 24)')
o = ap.parse_args()
a, b = open(o.src, 'rb').read(), open(o.dst, 'rb').read()
found = ambiguous = missing = 0
out = set()
for line in open(o.seeds):
    m = re.match(r'\s*0x([0-9A-Fa-f]+)', line)
    if not m:
        continue
    pat = a[int(m.group(1), 16):][:o.n]
    i = b.find(pat) if len(pat) == o.n else -1
    if i < 0:
        missing += 1
    elif b.find(pat, i + 1) >= 0:
        ambiguous += 1
    else:
        found += 1
        out.add(i)
for i in sorted(out):
    print(f'0x{i:06X}')
print(f'translate_seeds: {found} mapped, {ambiguous} ambiguous, {missing} not found', file=sys.stderr)
