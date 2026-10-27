#!/usr/bin/env python3
"""sync_seeds.py - after k1.s changes: drop recomp.json seeds in the shim ($80000-$81FFF) and add
every code symbol of k1.sym (the shim's functions move whenever k1.s grows)."""
import json, os
g = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
p = os.path.join(g, 'recomp.json')
d = json.load(open(p))
d['seeds'] = {k: v for k, v in d['seeds'].items() if not 0x80000 <= int(k, 16) < 0x82000}
for line in open(os.path.join(g, 'k1.sym')):
    v, t, n = line.split()
    if t in 'tT':
        d['seeds'][f'0x{int(v, 16):06X}'] = f'sub_{int(v, 16):06X}'
d['seeds'] = dict(sorted(d['seeds'].items()))
json.dump(d, open(p, 'w'), indent=2)
print(len(d['seeds']), 'seeds')
