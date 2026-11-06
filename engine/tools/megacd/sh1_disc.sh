#!/bin/zsh
# sh1_disc.sh WORK OUTPREFIX: SH1 for the Mega CD compilation disc as one image per zone (0.7.1).
# WORK is ultimate_sh.sh's work folder: ev/*/ZZAA.bin (the tracer's PS_ROMUSE captures, keyed by zone
# and act), ptrs.txt, s1_bs.bin (the BuildSprites helper). Writes OUTPREFIX.front (card 1: title, Green
# Hill, the special stage, the ending - what fits today) and OUTPREFIX.zone1..5 (Labyrinth, Marble, Star
# Light, Spring Yard, Scrap Brain + Final Zone), and prints build_disc.py's arguments (groups 13-17).
# Every image keeps ROM $000000-$03DCF2 (the code and shared data) at $200000 + address, so code
# pointers stay valid across a switch; the zone's own data from ROM $03DCF2 up is packed into the
# program-RAM window $020000-$03FFFF (zones need 78-98 KB of its 128). games/sh1_zone.s switches:
# the main loop's dispatch sends the modes a zone image lacks to the front, the Level entry sends
# another zone to its image.
set -eu
here=${0:A:h}; root=$here/../../..
work=${1:?usage: sh1_disc.sh WORK OUTPREFIX}; out=${2:?usage: sh1_disc.sh WORK OUTPREFIX}
rom=${ROM:-$root/../roms/sh1.bin}; py=$root/engine/.venv/bin/python; g=$root/games/sh1
t=$work/s1z; mkdir -p $t
m68k-elf-as -m68000 --defsym DESC=0x23FF00 -o $t/sw.o $here/swap.s
m68k-elf-ld -Ttext=0xFFFD20 --oformat=binary -o $t/swap.bin $t/sw.o
python3 -c "import sys; sys.stdout.buffer.write(bytes.fromhex('00200B10' '00201126' '00200388' '00FFFE00'))" > $t/desc.bin
common=(--data 071990-0719CC --data 071A9C-071AE8 --data 078B44-078C08
        --patch 33A=4E714E71 --patch 356=0080 --patch 36E=3F3F --patch D922=003F
        --patch D700=4EB90023E000 --blob 23E000=$work/s1_bs.bin
        --patch 388=4EB90023F0004E714E714E71 --patch 37A2=4EF90023F004 --blob 23FF00=$t/desc.bin)
helper() {  # NAME ZONE ZONE2 MODES [--defsym ACTMIN=.. / ACTMAX=..]
  m68k-elf-as -m68000 -I $t --defsym ZONE=$2 --defsym ZONE2=$3 --defsym MODES=$4 ${@:5} -o $t/h$1.o $here/games/sh1_zone.s
  m68k-elf-ld -Ttext=0x23F000 --oformat=binary -o $t/h$1.bin $t/h$1.o; }
# the sound tables' pointers, played or not (MusicIndex $071A9C: 19 songs, the effects $078B44: 49, the PSG
# envelopes $071990: 15): an effect no capture heard would keep its cartridge address and send the driver
# into a loop on the disc
python3 -c "
for t, n in ((0x071A9C, 19), (0x078B44, 49), (0x071990, 15)):
    print('\n'.join('%06X' % (t + 4 * i) for i in range(n)))" | cat - $work/ptrs.txt | sort -u |
  python3 -c "
import sys; rom = open('$rom', 'rb').read()   # LevelSizeArray (\$5F1C, 28 entries of 12 bytes): limits read as longs (left:right = \$00001800)
for l in sys.stdin:   # look like ROM addresses to the tracer - never pointers
    a = int(l, 16); v = int.from_bytes(rom[a:a + 4], 'big')
    if 0x5F1C <= a < 0x5F1C + 12 * 28: continue
    if a >= 0x20000 and v < 0x10000: continue   # art and collision maps (\$2222, \$EEEE ...): data, not low-ROM pointers
    sys.stdout.write(l)" > $t/ptrs.txt
# code ties and the addresses code names (tables come whole with them, offset tables to their last entry):
# a probe relocation over every capture, then code_refs.py
exall=(); for f in $work/ev/*/*.bin(N); do exall+=(--exec $f); done
( cd $g && $py $here/relocate.py $rom recomp.json $t/probe --ptrs $t/ptrs.txt $exall --all-code \
    --map 000000-080000=200000 --targets-out $t/targets.txt --funcs-out $t/funcs.txt >/dev/null 2>&1 )
$py $here/code_refs.py $rom $t/refs.txt $work/ev/*/*.bin(N) --funcs $t/funcs.txt >/dev/null
# palette data (what PalPointers $2168 names, 20 entries of pointer, RAM, length): its word pairs ($0000 $0ACE)
# read like low-ROM pointers to the tracer - not pointers (demo and other low-ROM data pointers are)
$py - $t/ptrs.txt $rom <<'P'
import sys
ptrs, rom = sys.argv[1], open(sys.argv[2], 'rb').read()
W = lambda a: int.from_bytes(rom[a:a + 2], 'big'); L = lambda a: int.from_bytes(rom[a:a + 4], 'big')
pal = [(L(0x2168 + 8 * i), L(0x2168 + 8 * i) + 4 * (W(0x2168 + 8 * i + 6) + 1)) for i in range(20)]   # length: longs - 1
lo, hi = min(a for a, _ in pal), max(b for _, b in pal)
keep = [l for l in open(ptrs) if not lo <= int(l, 16) < hi]
open(ptrs, 'w').writelines(keep)

P
# data two zones read (common tables, read sparsely): forced into every zone image
$py - $work/ev > $t/shared.txt <<'P'
import glob, sys, numpy as np
zones = {}
for f in glob.glob(sys.argv[1] + '/*/[0-9A-F][0-9A-F]??.bin'):
    d = np.frombuffer(open(f, 'rb').read()[2:], dtype=np.uint8); k = int(f.split('/')[-1][:2], 16)
    if d.size == 131072 and k < 6: zones[k] = zones.get(k, 0) | np.unpackbits(d[:65536] | d[65536:])
b = sum(np.asarray(v).reshape(-1, 0x100).any(1).astype(int) for v in zones.values()) >= 2
b[:0x3DCF2 // 0x100 + 1] = False
b[0x079700 // 0x100:] = False      # past the sound effects (the SEGA voice): a zone that reads it has it in its own captures
idx = np.flatnonzero(b); s = p = idx[0]; runs = []
for i in idx[1:]:
    if i - p > 1: runs.append((s, p)); s = i
    p = i
runs.append((s, p)); print(' '.join('--force %06X-%06X' % (s * 0x100, (e + 1) * 0x100) for s, e in runs))
P
# songs and sound effects (MusicIndex $071A9C, SFX index $078B44): SMPS data addresses its parts by offsets
# from its start, so each comes whole and in one piece: every effect, and the songs the zone can play (the
# drowning countdown in the water zone) - an unheard one would otherwise send the 68000 driver into garbage
for z in 1 2 3 4 5 6; do $py - $work/ev $z $rom > $t/songs$z.txt <<'P'
import glob, sys, numpy as np
ev, z, rom = sys.argv[1], int(sys.argv[2]), open(sys.argv[3], 'rb').read()
keys = ['0502'] if z == 6 else ['0500', '0501'] if z == 5 else ['%02X??' % z]   # image 6: Final Zone
u = 0
for f in [f for k in keys for f in glob.glob(ev + '/*/%s.bin' % k)]:
    d = np.frombuffer(open(f, 'rb').read()[2:], dtype=np.uint8)
    if d.size == 131072: u = u | np.unpackbits(d[:65536] | d[65536:])
u = np.asarray(u)
def table(t, n): return [int.from_bytes(rom[t + 4 * i:t + 4 * i + 4], 'big') for i in range(n)]
songs, sfx = table(0x071A9C, 19), table(0x078B44, 49)
starts = sorted(set(songs + sfx)); ends = dict(zip(starts, starts[1:] + [0x079700]))
# every effect; the songs this zone's captures played, those any level can play (invincibility $87, extra
# life $88, act clear $8E), and the drowning countdown ($92) in the water zone (1: Labyrinth, SBZ act 3)
keep = set(sfx) | {songs[m - 0x81] for m in (0x87, 0x88, 0x8E)} | ({songs[0x92 - 0x81]} if z == 1 else set())
keep |= {lo for lo in songs if u[lo:ends[lo]].any()}
args = []
for lo in sorted(keep):
    args += ['--force', '%06X-%06X' % (lo, ends[lo]), '--rigid', '%06X-%06X' % (lo, ends[lo])]
print(' '.join(args))
P
done
# a zone's data from ROM $03DCF2 up, packed by zonepack.py into the program-RAM window. The collision data
# ($061F00-$0654FF: angle map, height arrays, the zones' indexes) comes whole, as one piece - read as base +
# index, its rarely read entries are where a lost ring bounces; the SEGA voice
# ($079700-$07FFFF, played by the title only) stays out (left where it was: unmapped in a zone image). ObjPos_Index
# ($06B000: word offsets up to $6360 from the table, past zonepack's offset-table limit) and the zone's object
# lists keep their distances (one rigid piece, other data in its holes): the loader adds the offsets to the
# table's new address
zmap() {  # ZONE ACTS IMAGE (ACTS: the evidence of those acts only, e.g. "0 1"; "" all)
  local use="$work/ev/*:$(printf %02X $1)"
  if [[ -n ${2:-} ]]; then                         # an act subset: its captures linked apart
    local d=$t/ev_z$1_${2// /}; rm -rf $d
    for a in ${=2}; do for f in $work/ev/*/$(printf %02X%02X $1 $a).bin(N); do mkdir -p $d/${f:h:t}; ln -sf $f $d/${f:h:t}/; done; done
    use="$d/*:$(printf %02X $1)"
  fi
  $py $here/zonepack.py 0x80000 --rom $rom --use "$use" ${=$(cat $t/shared.txt)} \
    --fixed 000000-03DCF2=200000 --fixed 079700-080000=079700 --refs $t/refs.txt --near $t/targets.txt:0x200 --scan-tables \
    --rigid 06B000-071400 --force 061F00-065500 --rigid 061F00-065500 ${=$(cat $t/songs$3.txt)} \
    --place 020000-040000 2> $t/zpack$1.txt | tail -1; tail -1 $t/zpack$1.txt >&2; }
helper front 0 6 0xFF                              # the card: every mode; Green Hill and the ending
ex=(); for f in $work/ev/*/*.bin(N); do ex+=(--exec $f); done
( cd $g && $py $here/relocate.py $rom recomp.json $out.front --ptrs $t/ptrs.txt $ex --all-code \
    --map 000000-03DCF2=200000,060000-07C000=020000,03DCF2-041800=03C000 $common --blob 23F000=$t/hfront.bin > $t/relocfront.txt; head -1 $t/relocfront.txt )
for z in 1 2 3 4 5 6; do                          # 6: Final Zone (zone 5 act 2), apart: Scrap Brain fills the window
  if (( z == 5 )); then helper z5 5 5 0x0C --defsym ACTMAX=1; acts="0 1"; zz=5
  elif (( z == 6 )); then helper z6 5 5 0x0C --defsym ACTMIN=2; acts="2"; zz=5
  else helper z$z $z $z 0x0C; acts=""; zz=$z; fi
  ex=(); for f in $work/ev/*/0$zz??.bin(N); do ex+=(--exec $f); done
  map=$(zmap $zz "$acts" $z)
  ( cd $g && $py $here/relocate.py $rom recomp.json $out.zone$z --ptrs $t/ptrs.txt $ex --all-code \
      --map $map $common --blob 23F000=$t/hz$z.bin > $t/reloc$z.txt; head -1 $t/reloc$z.txt )
done
print "build_disc.py --game 1:$out.front --image 13:$out.zone1 --image 14:$out.zone2 --image 15:$out.zone3 --image 16:$out.zone4 --image 17:$out.zone5 --image 18:$out.zone6"
