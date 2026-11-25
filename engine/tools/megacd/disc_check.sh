#!/bin/zsh
# disc_check.sh GAME CARD DISC.cue OUTDIR - a game on the compilation disc against its cartridge: every
# level-select start of games/GAME/level_select.json (recordings/start-NAME.polls, level_starts.py) played on the
# reference runner from both, the disc's own boot spliced in front (BIOS, CD-ROM, the menu's card CARD, up to the
# game's title). Compared at every poll from the level's load: game mode, zone/act, the player's x ($FFF600,
# $FFFE10, the player's x - $FFD008, SH2 $FFB008; PX=ADDR another). One line per start: identical, the first differing poll, or where the disc
# stopped; and the picture (the frame, palette and video RAM hashed at each poll) until RAM differs. PS_REF (default build/ci-mac/genrecomp_ref), ROM (default ../roms/GAME.bin). sh1_disc_check.sh is
# this for SH1 (card 1).
set -eu
game=${1:?usage: disc_check.sh GAME CARD DISC.cue OUTDIR}; card=${2:?}; cue=${3:?}; o=${4:?}
cue=${cue:A}; mkdir -p $o; o=${o:A}
root=${0:A:h}/../../..; cd $root
R=${PS_REF:-build/ci-mac/genrecomp_ref}; rom=${ROM:-$root/../roms/$game.bin}; rec=games/$game/recordings
# the player's x: SH1 keeps the player object at $FFD000, SH2 at $FFB000 (its $D000 is another object)
px=${PX:-$(case $game in sh2) print B008 ;; sh3|shk|sh3k) print B010 ;; *) print D008 ;; esac)}   # SH3 engine: x at +$10
# the game's palettes in RAM (above water, under water): what it means to show. The VDP's colour RAM at a poll
# depends on where in the frame the poll falls (a water level swaps palettes mid-frame), so it is only a hint
pal=${PAL:-$(case $game in sh3|shk|sh3k) print FC00+64,FC40+64,F080+64,F0C0+64 ;; sh2|shk2) print FB00+64,FB40+64,F080+64,F0C0+64 ;; *) print FB00+64,FB40+64,FA80+64,FAC0+64 ;; esac)}   # 64 bytes a field at most
starts=(${(f)"$(python3 -c "
import json; d = json.load(open('games/$game/level_select.json'))   # items (a level select), or stages (SHK: RAM starts)
print('\n'.join(d['items'].values()) if 'items' in d else '\n'.join(n for n, _ in d['stages']))")"})
[[ -n ${ONLY:-} ]] && starts=(${=ONLY})                     # ONLY="EHZ1 MTZ1": those starts only
title() {  # the first poll of the title screen (game mode $04) in a watch file at or after poll $2
  awk -v from=$2 '$1 >= from && $3 == "04" { print $1; exit }' $1; }
# the cartridge's title poll
PS_POLL_REPLAY=$rec/start-$starts[1].polls PS_WATCH=$o/t.cart:F600 $R --headless --frames 2500 $rom >/dev/null 2>&1
ct=$(title $o/t.cart 0)
# the disc's boot: Start on the BIOS screen, CD-ROM, the card (cards left to right, 4 a row), Start. The BIOS is
# the disc's region's: Europe takes Start at 300, CD-ROM at 420, the menu reads the pad from about frame 800 (a
# RIGHT at 700 or 760 was lost: card 1 started); Japan's title runs longer - 500, 700, the menu from about 1200
reg=$(python3 -c "
import sys; sys.path.insert(0, '$root/engine/tools/megacd'); import build_disc as b
print({0xA1: 'J', 0x64: 'E'}.get(b.read_data(sys.argv[1], 0x800)[0x20B], 'U'))" $cue)
python3 -c "
m = {}
def press(a, b):
    for i in range(a, a + 4): m[i] = b
s1, s2, t = {'E': (300, 420, 900)}.get('$reg', (500, 700, 1400))
press(s1, 0x80); press(s2, 0x80); c = $card - 1
for _ in range(c % 4): press(t, 0x08); t += 20
for _ in range(c // 4): press(t, 0x02); t += 20
press(t + 60, 0x80)
open('$o/boot.mv', 'w').write(''.join('%x\n' % m.get(i, 0) for i in range(3000)))"
GENRECOMP_MOVIE=$o/boot.mv PS_POLL_RECORD=$o/boot.polls PS_WATCH=$o/t.disc:F600 \
  $R --headless --frames 3000 $cue >/dev/null 2>&1
dt=$(title $o/t.disc 200)
print "title: cartridge poll $ct, disc poll $dt"
i=0
for n in $starts; do
  ( r=$rec/start-$n.polls; L=$(wc -l < $r)
    { head -$dt $o/boot.polls; tail -n +$((ct + 1)) $r; } > $o/$n.rp
    PS_WATCH_PIC=1 PS_POLL_REPLAY=$r PS_WATCH=$o/$n.cart:F600,FE10+2,$px+2,$pal $R --headless --frames $((L * 2 + 300)) $rom >/dev/null 2>&1
    pk=(); [[ -f $rec/start-$n.poke ]] && pk=(GENRECOMP_POKE="$(< $rec/start-$n.poke)")   # a RAM start (SHK: no level select of its own)
    env $pk PS_WATCH_PIC=1 PS_POLL_REPLAY=$o/$n.rp PS_WATCH=$o/$n.disc:F600,FE10+2,$px+2,$pal \
      $R --headless --frames $((L * 2 + 3600)) $cue >/dev/null 2>&1 ) &
  i=$((i + 1)); (( i % 6 == 0 )) && wait
done; wait
python3 - $o $ct $dt $starts <<'P'
import sys
o, ct, dt, starts = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4:]
rd = lambda p: [l.split() for l in open(p) if len(l.split()) == 12]   # poll frame mode zone x, 2 palettes in halves, P/V/F (PS_WATCH_PIC)
ok = 0
for n in starts:
    res = []
    for f, base in ((f'{o}/{n}.cart', ct), (f'{o}/{n}.disc', dt)):
        w = rd(f); want = '10' if n[:2] == 'SS' and n[2:3].isdigit() else '0C'   # a special stage (SS1); Sky Sanctuary (SSZ) is a level
        k = next((j for j, x in enumerate(w) if int(x[0]) > base and x[2] == want), None)
        res.append(None if k is None else w[k:])
    c, d = res
    if c and d:   # the picture apart, until RAM differs: the game's palettes in RAM are the verdict (Emerald Hill's
        # cycle and Chemical Plant's colours, relocated as pointers, showed there); the VDP's colour RAM, video RAM
        # and the frame are hints (a mid-frame palette swap or a colour cycle lands either side of a poll)
        pm = min(len(c), len(d)); pd = next((j for j in range(pm) if c[j][2:5] != d[j][2:5]), pm)
        first = lambda i: next((j for j in range(pd) if c[j][i] != d[j][i]), None)
        jp = next((j for j in range(pd) if c[j][5:9] != d[j][5:9]), None)
        hint = ', '.join(f'{nm} from poll {j}' for nm, j in (('colour RAM', first(9)), ('frame', first(11))) if j is not None)
        print(n, 'palette:', 'identical while RAM is (%d polls)' % pd if jp is None else 'differs at poll %d' % jp,
              '- hints: ' + hint if hint else '')
    c = c and [x[2:5] for x in c]; d = d and [x[2:5] for x in d]
    if c is None: print(n, 'cartridge: never loads (recording)'); continue
    if d is None: print(n, 'disc: never loads'); continue
    m = min(len(c), len(d)); diff = next((j for j in range(m) if c[j] != d[j]), None)
    if diff is None and len(d) >= len(c) - 2: ok += 1; print(n, 'identical at all %d polls' % m)
    elif diff is None: print(n, 'identical for %d polls, then the disc stops (of %d)' % (len(d), len(c)))
    else: print(n, 'differs at poll %d of %d: cart %s disc %s' % (diff, m, c[diff], d[diff]))
print(f'{ok}/{len(starts)} identical to the cartridge')
P
