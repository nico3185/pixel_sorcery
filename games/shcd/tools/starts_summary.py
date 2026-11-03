#!/usr/bin/env python3
"""starts_summary.py DIR: native_starts.sh's results - entries whose main RAM equals the reference's at
every poll after the boot (BOOT_POLLS, the backup RAM checksum work) with as many polls (2 either way: the
run's last frame), and for the others the first
differing poll, its frame on each side and the RAM blocks that differ."""
import sys
BOOT_POLLS = 394
def load(p): return [l.split() for l in open(p) if len(l.split()) == 258]
d = sys.argv[1]; ok = []; rest = []; boot = set()
for k in range(74):
    try: a = load(f'{d}/stage{k}.ref'); b = load(f'{d}/stage{k}.nat')
    except OSError: continue
    allbad = [x[0] for x, y in zip(a, b) if x[2:] != y[2:]]
    boot.add(len([p for p in allbad if int(p) <= BOOT_POLLS]))
    bad = [(x, y) for x, y in zip(a, b) if x[2:] != y[2:] and int(x[0]) > BOOT_POLLS]
    if abs(len(a) - len(b)) > 2:                    # one side stopped polling (hung): a failure
        rest.append((k, 'polls', len(a), len(b), '', '')); continue
    if not bad: ok.append(k); continue
    x, y = bad[0]; bl = [f'{0xFF0000 + j * 256:06X}' for j in range(256) if x[2 + j] != y[2 + j]]
    rest.append((k, len(bad), x[0], x[1], y[1], ' '.join(bl[:6])))
print(f'match after boot: {len(ok)}/{len(ok) + len(rest)}; boot polls differing: {sorted(boot)}')
for r in rest: print(*r)
