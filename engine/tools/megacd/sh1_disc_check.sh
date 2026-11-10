#!/bin/zsh
# sh1_disc_check.sh DISC.cue OUTDIR - SH1 on the compilation disc against the cartridge: the 20 level-select
# starts (games/sh1/recordings/start-*.polls, level_starts.py) played on the reference runner from both, the
# disc's own boot spliced in front (BIOS, CD-ROM, the menu's card 1, up to SH1's title at poll 350; the
# cartridge's title is at poll 128). Compared at every poll from the level's load: game mode, zone/act, Sonic's x
# ($FFF600, $FFFE10, $FFD008). One line per start: identical, or the first differing poll, or where the disc
# stopped. Needs the reference runner (PS_REF, default build/ci-mac/genrecomp_ref) and ROM (../roms/sh1.bin).
set -eu
cue=${1:?usage: sh1_disc_check.sh DISC.cue OUTDIR}; o=${2:?usage: sh1_disc_check.sh DISC.cue OUTDIR}
cue=${cue:A}; mkdir -p $o; o=${o:A}
root=${0:A:h}/../../..; cd $root
R=${PS_REF:-build/ci-mac/genrecomp_ref}; rom=${ROM:-$root/../roms/sh1.bin}; rec=games/sh1/recordings
starts=(GHZ1 GHZ2 GHZ3 MZ1 MZ2 MZ3 SYZ1 SYZ2 SYZ3 LZ1 LZ2 LZ3 SLZ1 SLZ2 SLZ3 SBZ1 SBZ2 SBZ3 FZ SS1)
# the disc's boot: Start on the BIOS screen (300), CD-ROM (420), Start on the menu's card 1 (800)
python3 -c "
m = {i: 0x80 for a in (300, 420, 800) for i in range(a, a + 4)}
open('$o/boot.mv', 'w').write(''.join('%x\n' % m.get(i, 0) for i in range(1300)))"
GENRECOMP_REGION=2 GENRECOMP_MOVIE=$o/boot.mv PS_POLL_RECORD=$o/boot.polls $R --headless --frames 1300 $cue >/dev/null 2>&1
i=0
for n in $starts; do
  ( r=$rec/start-$n.polls; L=$(wc -l < $r)
    { head -350 $o/boot.polls; tail -n +129 $r; } > $o/$n.rp
    PS_POLL_REPLAY=$r PS_WATCH=$o/$n.cart:F600,FE10+2,D008+2 $R --headless --frames $((L * 2 + 300)) $rom >/dev/null 2>&1
    GENRECOMP_REGION=2 PS_POLL_REPLAY=$o/$n.rp PS_WATCH=$o/$n.disc:F600,FE10+2,D008+2 \
      $R --headless --frames $((L * 2 + 3000)) $cue >/dev/null 2>&1 ) &
  i=$((i + 1)); (( i % 6 == 0 )) && wait
done; wait
python3 - $o $starts <<'P'
import sys
o, starts = sys.argv[1], sys.argv[2:]
rd = lambda p: [l.split() for l in open(p) if len(l.split()) == 5]
ok = 0
for n in starts:
    res = []
    for f, base in ((f'{o}/{n}.cart', 128), (f'{o}/{n}.disc', 350)):
        w = rd(f); want = '10' if n.startswith('SS') else '0C'
        k = next((j for j, x in enumerate(w) if int(x[0]) > base and x[2] == want), None)
        res.append(None if k is None else [x[2:] for x in w[k:]])
    c, d = res
    if d is None: print(n, 'disc: never loads'); continue
    m = min(len(c), len(d)); diff = next((j for j in range(m) if c[j] != d[j]), None)
    if diff is None and len(d) >= len(c) - 2: ok += 1; print(n, 'identical at all %d polls' % m)
    elif diff is None: print(n, 'identical for %d polls, then the disc stops (of %d)' % (len(d), len(c)))
    else: print(n, 'differs at poll %d of %d: cart %s disc %s' % (diff, m, c[diff], d[diff]))
print(f'{ok}/{len(starts)} identical to the cartridge')
P
