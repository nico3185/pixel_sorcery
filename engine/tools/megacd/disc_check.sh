#!/bin/zsh
# disc_check.sh GAME CARD DISC.cue OUTDIR - a game on the compilation disc against its cartridge: every
# level-select start of games/GAME/level_select.json (recordings/start-NAME.polls, level_starts.py) played on the
# reference runner from both, the disc's own boot spliced in front (BIOS, CD-ROM, the menu's card CARD, up to the
# game's title). Compared at every poll from the level's load: game mode, zone/act, the player's x ($FFF600,
# $FFFE10, $FFD008: the SH games). One line per start: identical, the first differing poll, or where the disc
# stopped. PS_REF (default build/ci-mac/genrecomp_ref), ROM (default ../roms/GAME.bin). sh1_disc_check.sh is
# this for SH1 (card 1).
set -eu
game=${1:?usage: disc_check.sh GAME CARD DISC.cue OUTDIR}; card=${2:?}; cue=${3:?}; o=${4:?}
cue=${cue:A}; mkdir -p $o; o=${o:A}
root=${0:A:h}/../../..; cd $root
R=${PS_REF:-build/ci-mac/genrecomp_ref}; rom=${ROM:-$root/../roms/$game.bin}; rec=games/$game/recordings
starts=(${(f)"$(python3 -c "import json; print('\n'.join(json.load(open('games/$game/level_select.json'))['items'].values()))")"})
title() {  # the first poll of the title screen (game mode $04) in a watch file at or after poll $2
  awk -v from=$2 '$1 >= from && $3 == "04" { print $1; exit }' $1; }
# the cartridge's title poll
PS_POLL_REPLAY=$rec/start-$starts[1].polls PS_WATCH=$o/t.cart:F600 $R --headless --frames 2500 $rom >/dev/null 2>&1
ct=$(title $o/t.cart 0)
# the disc's boot: Start on the BIOS screen (300), CD-ROM (420), the card (cards left to right, 4 a row), Start
python3 -c "
m = {}
def press(a, b):
    for i in range(a, a + 4): m[i] = b
press(300, 0x80); press(420, 0x80); c = $card - 1; t = 700
for _ in range(c % 4): press(t, 0x08); t += 20
for _ in range(c // 4): press(t, 0x02); t += 20
press(max(t, 800), 0x80)
open('$o/boot.mv', 'w').write(''.join('%x\n' % m.get(i, 0) for i in range(1600)))"
GENRECOMP_REGION=2 GENRECOMP_MOVIE=$o/boot.mv PS_POLL_RECORD=$o/boot.polls PS_WATCH=$o/t.disc:F600 \
  $R --headless --frames 1600 $cue >/dev/null 2>&1
dt=$(title $o/t.disc 200)
print "title: cartridge poll $ct, disc poll $dt"
i=0
for n in $starts; do
  ( r=$rec/start-$n.polls; L=$(wc -l < $r)
    { head -$dt $o/boot.polls; tail -n +$((ct + 1)) $r; } > $o/$n.rp
    PS_POLL_REPLAY=$r PS_WATCH=$o/$n.cart:F600,FE10+2,D008+2 $R --headless --frames $((L * 2 + 300)) $rom >/dev/null 2>&1
    GENRECOMP_REGION=2 PS_POLL_REPLAY=$o/$n.rp PS_WATCH=$o/$n.disc:F600,FE10+2,D008+2 \
      $R --headless --frames $((L * 2 + 3000)) $cue >/dev/null 2>&1 ) &
  i=$((i + 1)); (( i % 6 == 0 )) && wait
done; wait
python3 - $o $ct $dt $starts <<'P'
import sys
o, ct, dt, starts = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4:]
rd = lambda p: [l.split() for l in open(p) if len(l.split()) == 5]
ok = 0
for n in starts:
    res = []
    for f, base in ((f'{o}/{n}.cart', ct), (f'{o}/{n}.disc', dt)):
        w = rd(f); want = '10' if n.startswith('SS') else '0C'
        k = next((j for j, x in enumerate(w) if int(x[0]) > base and x[2] == want), None)
        res.append(None if k is None else [x[2:] for x in w[k:]])
    c, d = res
    if c is None: print(n, 'cartridge: never loads (recording)'); continue
    if d is None: print(n, 'disc: never loads'); continue
    m = min(len(c), len(d)); diff = next((j for j in range(m) if c[j] != d[j]), None)
    if diff is None and len(d) >= len(c) - 2: ok += 1; print(n, 'identical at all %d polls' % m)
    elif diff is None: print(n, 'identical for %d polls, then the disc stops (of %d)' % (len(d), len(c)))
    else: print(n, 'differs at poll %d of %d: cart %s disc %s' % (diff, m, c[diff], d[diff]))
print(f'{ok}/{len(starts)} identical to the cartridge')
P
