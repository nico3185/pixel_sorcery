#!/usr/bin/env python3
"""tracecmp.py DIR — align the recompiled (DIR/r.it) and oracle (DIR/o.it) instruction traces
(pc, cycle, registers) and print the first difference with a few lines of context."""
import sys
S=sys.argv[1] if len(sys.argv) > 1 else 'build/sidebyside'
r=[l.split() for l in open(f'{S}/r.it')]; o=[l.split() for l in open(f'{S}/o.it')]
i=j=0
while i<len(r) and j<len(o):
    if r[i][0]!=o[j][0]:
        if i+1<len(r) and r[i+1][0]==o[j][0]: i+=1; continue
        if j+1<len(o) and o[j+1][0]==r[i][0]: j+=1; continue
        print('path diverges: recomp', r[i][0], 'oracle', o[j][0])
        for k in range(5, 0, -1):
            if i-k >= 0 and j-k >= 0: print('   ', r[i-k][0], r[i-k][1], '|', o[j-k][0], o[j-k][1])
        break
    if r[i][1]!=o[j][1] or r[i][3:]!=o[j][3:]:
        print('first difference at pc', r[i][0], 'line', i)
        for k in range(4, 0, -1):
            if i-k >= 0 and j-k >= 0: print('   ', r[i-k][0], r[i-k][1], '|', o[j-k][0], o[j-k][1])
        print('  r', ' '.join(r[i][1:])); print('  o', ' '.join(o[j][1:])); break
    i+=1; j+=1
else: print('identical (pc, cycles, registers) over', i, 'instructions')
