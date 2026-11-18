#!/bin/zsh
# sh2_disc.sh OUTPREFIX: SH2 (REV01) for the Mega CD compilation disc as one image per zone (0.7.2).
# Writes OUTPREFIX.front (card 2: SEGA, title, options, level select), OUTPREFIX.<zone> for each zone
# (Metropolis with its act 3; with Emerald Hill the front is 284 KB of the 286 an image has) and OUTPREFIX.ss (the special stage), and OUTPREFIX.args:
# build_disc.py's arguments (--game 2:... --image G:... for groups 24-35).
# Evidence: the level-select starts (games/sh2/recordings/start-*.polls, level_starts.py) replayed on
# the tracer twice, PS_ROMUSE keyed by zone ($FFFE10: what a zone needs) and by game mode ($FFF600: the
# front end, Emerald Hill, the special stage), on the ROM without its checksum loop (it reads everything).
# Layout, every image (zonepack.py packs each on its own, as s3engine_disc.sh does):
#   word RAM $200500 the image's descriptor (VBlank, HBlank, main loop, stack), $200510 the switch
#   (games/sh2_zone.s + swap.s), $200700 helpers (the sound driver's loader, the DMA queue),
#   $200900 Casino Night's bumper lists (cnz), $200C00-$226800 code/data, $226800 the object layouts (ROM $E6800-$E8000: ObjPos lists found by offsets
#   from $E6800, some past $E8000 - Death Egg's at $EB230 - so they keep their distance to the Z80's banks),
#   $228000 the Z80's banks (ROM $E8000-$100000);
#   program-RAM bank 1 ($020000-$03FFFF) code/data; banks 2/3 Sonic's/Tails' art (virtual $0A0000/$0E0000).
# The main loop's dispatch and the Level entry go through the switch: a mode or a zone another image
# holds loads that image, which starts at its own main loop.
set -eu
root=${0:A:h}/../../..; here=${0:A:h}
out=${1:A}; rom=${ROM:-$root/../roms/sh2.bin}
py=$root/engine/.venv/bin/python; g=$root/games/sh2
work=${out:h}/s2work; [[ -n ${S2_KEEP_EV:-} ]] || rm -rf $work; mkdir -p $work/evz $work/evm   # S2_KEEP_EV=1: the captures of the last run
ninja -C $g/build-trace >/dev/null
tr=$g/build-trace/genrecomp_build/genrecomp_trace
# on the original ROM, recorded from frame 112 (the checksum loop, before it, reads the whole ROM; the first
# pad read is at frame ~117): a ROM without the loop starts the game at another frame and the replay drifts
# from the recording (Metropolis 1: the capture never rode the spiral the recording rides at poll 300). The
# boot before it: a capture of its executed half only (front image).
mkdir -p $work/boot
[[ -n "$(print -l $work/boot/*.bin(N))" ]] || PS_ROMUSE=$work/boot PS_ROMUSE_KEY=FFF600 $tr --headless --frames 112 --trace-out /dev/null $rom >/dev/null 2>&1
print "1/4 evidence: $(ls $g/recordings/start-*.polls | wc -l | tr -d ' ') level-select starts, by zone and by game mode"
i=0
for p in $g/recordings/start-*.polls; do
  [[ -n ${S2_KEEP_EV:-} && -n "$(print -l $work/evz/${p:t:r}/*.bin(N))" ]] && continue
  n=$(wc -l < $p); b=${p:t:r}; mkdir -p $work/evz/$b $work/evm/$b
  PS_ROMUSE=$work/evz/$b PS_ROMUSE_FROM=112 PS_ROMUSE_KEY=FFFE10 PS_PTRS=$work/evz/$b/ptrs.txt PS_PTRS_FROM=112 PS_PTRS_NEAR=0x80 PS_POLL_REPLAY=$p \
    $tr --headless --frames $((n * 2 + 600)) --trace-out /dev/null $rom >/dev/null 2>&1 &
  PS_ROMUSE=$work/evm/$b PS_ROMUSE_FROM=112 PS_ROMUSE_KEY=FFF600 PS_POLL_REPLAY=$p \
    $tr --headless --frames $((n * 2 + 600)) --trace-out /dev/null $rom >/dev/null 2>&1 &
  i=$((i + 1)); (( i % ${PS_JOBS:-3} == 0 )) && wait
done; wait
# art the image's code can queue (moveq #id,d0 + LoadPLC: game over, sign post, ...), whole
python3 $here/plc.py $rom 42660 0,1,3,27,3C,3D,42 $work/plc_ptrs.txt $work/plc_force.txt
# the level sizes (LevelSize $C054: per zone and act, left/right/top/bottom limits as words) read as longwords
# look like ROM addresses to the tracer ($02000600: "$000600", VBlank code the CPU runs anyway) - never pointers
# (Aquatic Ruin 1's bottom limit became $B100 on the disc and Sonic died as he spawned)
# A pointer seen read is one entry of a table whose other entries a play may never read (the animals a
# badnik frees, per zone: Oil Ocean 1's kept its cartridge mappings and the sprite builder took an address
# error). Its neighbours at a stride of 4 or 8 bytes that point near it (even, within 32 KB) are taken too, up
# to 64 a side - not in the level sizes, the PLC lists ($42660-$43000: 6-byte entries, plc.py's) or Sonic's and
# Tails' art ($50000-$6FC00: pixels). Nothing in that art is a pointer: a pixel run read as one was
# rewritten per image (15 bytes of Sonic's sprites differed between images, and the art bank was loaded again).
cat $work/evz/*/ptrs.txt $work/plc_ptrs.txt | sort -u | python3 -c "
import sys
rom = open(sys.argv[1], 'rb').read(); n = len(rom)
L = lambda a: int.from_bytes(rom[a:a + 4], 'big') if 0 <= a <= n - 4 else -1
seen = [int(l, 16) for l in sys.stdin]; out = set(seen)
for p in seen:
    v = L(p)
    for st in (4, 8, -4, -8):
        q = p + st
        for _ in range(64):
            w = L(q)
            if not (0x200 <= w < n and not w & 1 and abs(w - v) < 0x8000) or 0x42660 <= q < 0x43000 or 0x50000 <= q < 0x6FC00: break
            out.add(q); q += st
sys.stdout.writelines('%06X\\n' % a for a in sorted(out) if not (0xC054 <= a < 0xC164 or 0x50000 <= a < 0x6FBE0))" $rom > $work/ptrs.txt
force=(); for r in $(cat $work/plc_force.txt); do force+=(--force $r); done
ex=(); for f in $work/evz/*/*.bin; do ex+=(--exec $f); done
print "2/4 references (relative, fall-through) and named tables"
$py $here/code_refs.py $rom $work/refs.txt $work/evz/*/*.bin
( cd $g && $py $here/relocate.py $rom recomp.json $work/probe $ex --all-code \
    --map 000000-100000=200000 --targets-out $work/targets.txt >/dev/null 2>&1 )
print "3/4 helpers"
m68k-elf-as -m68000 -o $work/home.o $here/games/home.s && m68k-elf-ld -Ttext=0x200200 --oformat=binary -o $work/home.bin $work/home.o   # A+B+C+Start: the menu
m68k-elf-as -m68000 --defsym ABSORB=${S2_ABSORB:-1} --defsym LOADSCREEN=1 -o $work/sw.o $here/swap.s; m68k-elf-ld -Ttext=0xFFFD20 --oformat=binary -o $work/swap.bin $work/sw.o
# the images: NAME GROUP USE... (zonepack --use values); zones and modes -> group, for the switch tables
names=(front ehz mtz wfz htz ooz mcz cnz cpz dez arz scz ss)
typeset -A grp=(front 2 ehz 35 mtz 24 wfz 25 htz 26 ooz 27 mcz 28 cnz 29 cpz 30 dez 31 arz 32 scz 33 ss 34)
typeset -A zof=(0 ehz 1 ehz 2 ehz 3 ehz 4 mtz 5 mtz 6 wfz 7 htz 8 ehz 9 ehz 10 ooz 11 mcz 12 cnz 13 cpz 14 dez 15 arz 16 scz)
typeset -A use=(   # captures, under $work: GLOB:KEYS (zonepack.py --use; the key's high byte: zone or game mode)
  front "evm/*:0+4+24+28 x:boot:0" ehz "evm/start-EHZ*:8+C+88+8C"
  mtz "evz/*:4+5" wfz "evz/*:6" htz "evz/*:7" ooz "evz/*:A" mcz "evz/*:B" cnz "evz/*:C" cpz "evz/*:D"
  dez "evz/*:E" arz "evz/*:F" scz "evz/*:10" ss "evm/*:10+90")
print "4/4 images"
args=()
for name in $names; do
  [[ -n ${S2_ONLY:-} && ${${=S2_ONLY}[(Ie)$name]} -eq 0 ]] && { [[ $name == front ]] && args+=(--game 2:$out.front) || args+=(--image $grp[$name]:$out.$name); continue; }   # S2_ONLY="arz ss": only those again
  $py - $name > $work/tables.inc <<P
import sys
me = sys.argv[1]
grp = dict(zip("${(k)grp}".split(), map(int, "${(v)grp}".split())))
zof = dict(zip(map(int, "${(k)zof}".split()), "${(v)zof}".split()))
G = lambda n: 0 if n == me else 0x80 + grp[n]
# the front holds every mode but the special stage (continue, 2P, the ending: no evidence yet, they stay
# where the front can be extended); a zone image the level modes; the special stage's image only its own
if me == 'front': modes = [0, 0, 0xFF, 0xFF, G('ss'), 0, 0, 0, 0, 0, 0]   # a level mode: the zone's image
elif me == 'ss':  modes = [G('front')] * 2 + [0xFF, 0xFF, 0] + [G('front')] * 6
else:             modes = [G('front')] * 2 + [0, 0, G('ss')] + [G('front')] * 6
print('modes:\t.byte\t' + ', '.join('0x%02X' % m for m in modes))
print('zones:\t.byte\t' + ', '.join('0x%02X' % G(zof[z]) for z in range(17)))
P
  uargs=(); for u in ${=use[$name]}; do [[ $u == x:* ]] && uargs+=(--exec-only $work/${${u#x:}%%:*}:${u##*:}) || uargs+=(--use $work/$u); done
  exm=(${(f)"$(python3 -c "
import glob, os, sys   # the captures this image is made of: --use GLOB:KEYS (the key's high byte)
a = sys.argv[1:]
for g, keys in (x.rsplit(':', 1) for x in a if x not in ('--use', '--exec-only')):
    ks = {int(k, 16) for k in keys.split('+')}
    for f in sorted(glob.glob(g + '/*.bin')):
        if int(os.path.basename(f)[:2], 16) in ks: print('--exec'); print(f)" $uargs)"})
  ( cd $g && $py $here/relocate.py $rom recomp.json $work/probe_$name $exm \
      --map 000000-100000=200000 --funcs-out $work/funcs_$name.txt >/dev/null 2>&1 )
  # the ties of this image's own code only (all the zones' together chain the whole code area into one
  # 269 KB piece, more than word RAM's 157 KB or the window's 128)
  $py $here/code_refs.py $rom $work/refs_$name.txt ${exm:#--exec} --funcs $work/funcs_$name.txt --tables-out $work/tables_$name.txt >/dev/null
  grep -v -i -E '^0*(411A|411E) ' $work/refs_$name.txt > $work/refs_$name.cut; mv $work/refs_$name.cut $work/refs_$name.txt   # called through the helper (games/sh2_zone.s +8)
  # collision (curve map $42D50, height arrays $42E50/$43E50, the zone's own block indexes): read as
  # base + index, so a play's captures touch a few entries only - an untouched one (a slope taken another way)
  # read whatever the image put there and Sonic left the floor (EHZ1 on the disc, poll 37). Whole, in every level image
  typeset -A zids=(ehz "0" mtz "4 5" wfz "6" htz "7" ooz "A" mcz "B" cnz "C" cpz "D" dez "E" arz "F" scz "10")
  coll=(); [[ -n ${zids[$name]:-} ]] && coll=($(python3 - $rom ${=zids[$name]} <<'C'
import sys
r = open(sys.argv[1], 'rb').read(); L = lambda a: int.from_bytes(r[a:a + 4], 'big')
tabs = sorted({L(0x49E8 + 4 * i) for i in range(34)} | {0x45B80})   # Off_ColP $49E8 / Off_ColS $4A2C, 17 zones each
out = ['--force', '042D00-044E50']                                   # curve map + both height arrays
for z in sys.argv[2:]:
    for t in (0x49E8, 0x4A2C):
        a = L(t + 4 * int(z, 16)); out += ['--force', '%06X-%06X' % (a, next(x for x in tabs if x > a))]
print(' '.join(out))
C
))
  tabf=(--force-file $work/tables_$name.txt); [[ ${S2_TABLES:-1} == 0 ]] && tabf=()   # S2_TABLES=0: indexed tables not forced
  pack() {  # NEAR GAP: table margin, data merge gap; an image that does not fit is packed again tighter
    python3 $here/zonepack.py 0x100000 --rom $rom $uargs --near $work/targets.txt:$1 --gap $2 \
      $force $coll --fixed E6800-E8000=226800 --fixed E8000-100000=228000 --fixed 50000-64320=0A0000 --fixed 64320-6FBE0=0E0000 \
      --force 000200-000410 --refs $work/refs_$name.txt --force-file $work/funcs_$name.txt $tabf \
      --place ${S2_SIZES:+400000-600000,}200C00-226800,020000-040000 > $work/map_$name.txt 2> $work/pack_$name.txt; }
  pack 0x200 0x800 || pack 0x100 0x200 || { print -u2 "$name: does not fit ($(tail -1 $work/pack_$name.txt))"; exit 1; }   # tighter: s3engine_disc.sh's values for 4 MB
  [[ -n ${S2_SIZES:-} ]] && { print "  $name: $(tail -1 $work/pack_$name.txt)"; continue; }   # S2_SIZES=1: what each image needs, no limit
  # where the map put an address
  at() { python3 -c "
import sys
m=[(int(a.split('-')[0],16),int(a.split('-')[1].split('=')[0],16),int(a.split('=')[1],16)) for a in open('$work/map_$name.txt').read().strip().split(',')]
v=int(sys.argv[1],16); print('%X' % next((new+v-lo for lo,hi,new in m if lo<=v<hi), 0x200510))" $1; }   # not in this image: never reached
  v() { python3 -c "import sys; print('%X' % int.from_bytes(open('$rom','rb').read()[$1*4:$1*4+4],'big'))"; }
  python3 -c "import sys; sys.stdout.buffer.write(b''.join(int(x,16).to_bytes(4,'big') for x in sys.argv[1:]))" \
    $(at $(v 30)) $(at $(v 28)) $(at 394) FFFFFE00 > $work/desc_$name.bin
  m68k-elf-as -m68000 -I $work --defsym TABLE=0x$(at 3A2) --defsym LEVELREST=0x$(at 3ECA) --defsym FAR1=0x$(at BFBC) --defsym FAR2=0x$(at 4F52) -o $work/z.o $here/games/sh2_zone.s
  m68k-elf-ld -Ttext=0x200510 --oformat=binary -o $work/zone_$name.bin $work/z.o
  snd=$(at EC000)
  m68k-elf-as -m68000 -I $here/games --defsym SNDLOAD=0x$snd --defsym ZB1D=0x45 --defsym ZB1E=0x46 --defsym ZB1F=0x47 \
    -o $work/h.o $here/games/sh2_helpers.s
  m68k-elf-ld -Ttext=0x200700 -o $work/h.elf $work/h.o; m68k-elf-objcopy -O binary $work/h.elf $work/h_$name.bin
  (( $(wc -c < $work/h_$name.bin) <= 0x200 )) || { print -u2 "sh2_helpers.s: over 512 bytes (the cnz lists start at \$200900)"; exit 1; }
  sl=$(nm $work/h.elf | awk '/ sndload$/ {print toupper($1)}'); dq=$(nm $work/h.elf | awk '/ dmaqueue$/ {print toupper($1)}')
  # Casino Night's special bumpers ($1781A act 1, $1795E act 2: 6-byte entries scanned both ways from the camera):
  # the backward scan stops on the word 4 bytes before a list being 0 - before act 1's it is the high word of the
  # "jmp $1370.l" that ends the previous routine, nonzero once relocated, so near the act's start the scan ran
  # back into code and the handler jumped through its 6-entry table with index 6 (CNZ1 crashed at poll 4810).
  # Both lists go to $200904 behind a zero entry x (the scan reads the previous entry's x at -4), and the two
  # lea that name them are pointed there.
  cnzl=(); if [[ $name == cnz ]]; then
    python3 -c "import sys; r = open('$rom', 'rb').read(); sys.stdout.buffer.write(bytes(4) + r[0x1781A:0x17AA2])" > $work/cnzlists.bin
    cnzl=(--blob 200900=$work/cnzlists.bin --patch 173D2=43F900200904 --patch 173DE=43F900200A48); fi
  # the hit boxes of the touch code ($3FB28..$3FC34: move.l #$00040004,d1 - two sizes, not $40004) stay as they
  # are: relocated, Aquatic Ruin 2's arrow missed Sonic on the disc where it hits him on the cartridge
  ( cd $g && $py $here/relocate.py $rom recomp.json $out.$name --ptrs $work/ptrs.txt $ex --all-code --map $(cat $work/map_$name.txt) \
      --patch 3FB2A=00040004 --patch 3FBA0=00080010 --patch 3FC00=00060008 --patch 3FC36=0008000C \
      --patch 346=4E714E71 --patch 362=0080 --patch 37A=3F3F --patch 17B0A=003E \
      --patch 394=4EB9002005104E714E714E71 --patch 3EC4=4EF900200514 --patch 411A=4EB9002005184E71 \
      --patch 130C=4EF9$sl --patch 14AC=4EF9$dq --blob 200700=$work/h_$name.bin --patch 111C=4EF900200200 --blob 200200=$work/home.bin \
      --blob 200500=$work/desc_$name.bin --blob 200510=$work/zone_$name.bin $cnzl | head -1 )
  print "  $name: $(tail -1 $work/pack_$name.txt)"
  [[ $name == front ]] && args+=(--game 2:$out.front) || args+=(--image $grp[$name]:$out.$name)
done
print -rl -- $args > $out.args   # one argument per line (paths may hold spaces)
print "build_disc.py $args"
