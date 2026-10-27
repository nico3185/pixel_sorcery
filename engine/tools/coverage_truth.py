#!/usr/bin/env python3
"""coverage_truth.py GAMEDIR TRUTH.toml [...]: how much of a decompilation's known code the
title's generated C covers. TRUTH files list routine addresses (any 0x... after "addr ="
or in a list). A routine counts as covered when its address is an instruction the
recompiler decoded (an ITRACE in src/recomp): a function entry or inside one.
Used to measure discovery, never as input to it."""
import glob
import re
import sys

game, truths = sys.argv[1], sys.argv[2:]
truth = set()
for t in truths:
    for m in re.finditer(r'(?:addr\s*=\s*|^\s*)0x([0-9A-Fa-f]{4,6})', open(t).read(), re.M):
        a = int(m.group(1), 16)
        if a >= 0x200 and not a & 1:
            truth.add(a)
insns, entries = set(), set()
for f in glob.glob(f'{game}/src/recomp/recomp_*.c'):
    s = open(f).read()
    insns.update(int(x, 16) for x in re.findall(r'ITRACE\(0x([0-9A-F]+)\)', s))
    entries.update(int(x, 16) for x in re.findall(r'^void \w+_([0-9A-F]{6})\(void\) \{', s, re.M))
cov = truth & insns
print(f'{len(cov)}/{len(truth)} known routines covered ({100 * len(cov) / max(1, len(truth)):.1f}%), '
      f'{len(truth & entries)} as function entries; {len(insns)} instructions, {len(entries)} functions generated')
missing = sorted(truth - insns)
if '-v' in sys.argv or len(missing) <= 20:
    print('missing:', ' '.join(f'${a:06X}' for a in missing[:200]))
