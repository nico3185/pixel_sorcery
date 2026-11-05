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
helper() {  # NAME ZONE ZONE2 MODES
  m68k-elf-as -m68000 -I $t --defsym ZONE=$2 --defsym ZONE2=$3 --defsym MODES=$4 -o $t/h$1.o $here/games/sh1_zone.s
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
# from its start, so every song and effect comes whole and in one piece, in every image - the drowning
# countdown or an effect the captures never heard would otherwise send the 68000 sound driver into garbage
for z in 1 2 3 4 5; do $py - $work/ev $z $rom > $t/songs$z.txt <<'P'
import glob, sys, numpy as np
ev, z, rom = sys.argv[1], int(sys.argv[2]), open(sys.argv[3], 'rb').read()
u = 0
for f in glob.glob(ev + '/*/%02X??.bin' % z):
    d = np.frombuffer(open(f, 'rb').read()[2:], dtype=np.uint8)
    if d.size == 131072: u = u | np.unpackbits(d[:65536] | d[65536:])
u = np.asarray(u)
def table(t, n): return [int.from_bytes(rom[t + 4 * i:t + 4 * i + 4], 'big') for i in range(n)]
starts = sorted(set(table(0x071A9C, 19) + table(0x078B44, 49)))
ends = starts[1:] + [0x079700]
args = []
for lo, hi in zip(starts, ends):
    args += ['--force', '%06X-%06X' % (lo, hi), '--rigid', '%06X-%06X' % (lo, hi)]   # any image may play any of them
print(' '.join(args))
P
done
# a zone's data from ROM $03DCF2 up, packed by zonepack.py into the program-RAM window. ObjPos_Index
# ($06B000: word offsets up to $6360 from the table, past zonepack's offset-table limit) and the zone's object
# lists keep their distances (one rigid piece, other data in its holes): the loader adds the offsets to the
# table's new address
zmap() { $py $here/zonepack.py 0x80000 --rom $rom --use "$work/ev/*:$(printf %02X $1)" ${=$(cat $t/shared.txt)} \
    --fixed 000000-03DCF2=200000 --refs $t/refs.txt --near $t/targets.txt:0x200 --scan-tables \
    --rigid 06B000-071400 ${=$(cat $t/songs$1.txt)} \
    --place 020000-040000 2> $t/zpack$1.txt | tail -1; tail -1 $t/zpack$1.txt >&2; }
helper front 0 6 0xFF                              # the card: every mode; Green Hill and the ending
ex=(); for f in $work/ev/*/*.bin(N); do ex+=(--exec $f); done
( cd $g && $py $here/relocate.py $rom recomp.json $out.front --ptrs $t/ptrs.txt $ex --all-code \
    --map 000000-03DCF2=200000,060000-07C000=020000,03DCF2-041800=03C000 $common --blob 23F000=$t/hfront.bin > $t/relocfront.txt; head -1 $t/relocfront.txt )
for z in 1 2 3 4 5; do
  helper z$z $z $z 0x0C
  ex=(); for f in $work/ev/*/0$z??.bin(N); do ex+=(--exec $f); done
  map=$(zmap $z)
  ( cd $g && $py $here/relocate.py $rom recomp.json $out.zone$z --ptrs $t/ptrs.txt $ex --all-code \
      --map $map $common --blob 23F000=$t/hz$z.bin > $t/reloc$z.txt; head -1 $t/reloc$z.txt )
done
print "build_disc.py --game 1:$out.front --image 13:$out.zone1 --image 14:$out.zone2 --image 15:$out.zone3 --image 16:$out.zone4 --image 17:$out.zone5"
