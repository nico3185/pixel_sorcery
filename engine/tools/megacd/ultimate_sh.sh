#!/bin/zsh
# ultimate_sh.sh DISC.cue MENU.png [OUTDIR]: the compilation Mega CD disc of the sh titles.
# For your own use only: the disc holds code and data from your games - never share it.
#
# Inputs, all yours (nothing of them goes into the repository):
#   DISC.cue     your Mega CD disc of the series (Europe) - its data track is the disc's base
#                (files, security block) and its audio tracks are the disc's audio tracks
#   MENU.png     the game-select picture (4x2 cards; MENU_UNFRAME=x0,y0,x1,y1 removes a
#                highlight baked into the picture, default 92,176,432,504)
#   ROM          sh1 (REV00), default ../roms/sh1.bin
#   ROM2         sh2 (REV01), default ../roms/sh2.bin (card 2: its first level for now)
#   ROM3         sh3 (USA), default ../roms/sh3.bin (card 3: title to its first act)
#   ROM4         shk, default ../roms/shk.bin (card 4: title to its first act)
#   ROM5         shk + sh1 (lock-on dump, evidence only), default ../roms/shk_sh1.bin
#                (card 5: shbs; the disc image is shk's code with sh1's header, from
#                ROM, where shk reads the locked-on cartridge)
#   ROM7         sh3k (lock-on dump), default ../roms/sh3k.bin (card 7: its first act)
# Steps: evidence runs of sh1 on the tracer (level select, every zone, running right and
# jumping; plus your recorded sessions in games/sh1/sessions), relocation (relocate.py),
# the BuildSprites helper, menu art, disc (build_disc.py). Output: OUTDIR/ultimate.iso +
# ultimate.cue (about 7 minutes; default ~/Library/Application Support/PixelSorcery/megacd/ultimate).
# Play: GENRECOMP_REGION=2 games/sh1/build-trace/genrecomp_build/genrecomp_ref OUTDIR/ultimate.cue
set -eu
root=${0:A:h}/../../..
here=${0:A:h}
[[ $# -ge 2 ]] || { sed -n 2,16p $0; exit 2; }
scd=${1:A}; pic=${2:A}
out=${3:-$HOME/Library/Application Support/PixelSorcery/megacd/ultimate}
rom=${ROM:-$root/../roms/sh1.bin}
py=$root/engine/.venv/bin/python
mkdir -p $out; work=$out/work; rm -rf $work; mkdir -p $work
g=$root/games/sh1
cmake -S $g -B $g/build-trace -G Ninja -DCMAKE_BUILD_TYPE=Release -DPS_TRACE=ON >/dev/null 2>&1 || true
ninja -C $g/build-trace >/dev/null
tr=$g/build-trace/genrecomp_build/genrecomp_trace

print "1/5 evidence: level select items 0-19 (running right, jumping), recorded sessions"
jumps=(); for t in $(seq 1300 250 5800); do jumps+=(--press $t:C:10); done
run() {  # item: level select ($FFE0 = 1, cursor $FF83), A+START at the title, START on the item
  d=$work/ev/item$1; mkdir -p $d
  PS_ROMUSE=$d PS_ROMUSE_FROM=60 PS_PTRS=$d/ptrs.txt PS_POKE=FFE0=01@750,FF83=$(printf %02X $1)@900 \
    $tr --headless --frames 6000 --press 800:A+START:20 --press 1000:START --press 1150:RIGHT:4800 $jumps \
    --trace-out /dev/null $rom >/dev/null 2>&1
}
for i in $(seq 0 19); do run $i & if (( (i + 1) % 4 == 0 )); then wait; fi; done; wait
for p in $g/sessions/*.polls(N); do
  d=$work/ev/$(basename $p .polls); mkdir -p $d; n=$(wc -l < $p)
  PS_ROMUSE=$d PS_ROMUSE_FROM=60 PS_PTRS=$d/ptrs.txt PS_POLL_REPLAY=$p \
    $tr --headless --frames $((n * 2 + 600)) --trace-out /dev/null $rom >/dev/null 2>&1 &
done; wait
cat $work/ev/*/ptrs.txt | sort -u > $work/ptrs.txt
ex=(); for f in $work/ev/*/*.bin; do ex+=(--exec $f); done

print "2/5 BuildSprites helper"
m68k-elf-as -m68000 -o $work/bs.o $here/games/sh1_buildsprites.s
m68k-elf-ld -Ttext=0x23E000 --oformat=binary -o $work/s1_bs.bin $work/bs.o

print "3/5 relocation"
( cd $g && $py $here/relocate.py $rom recomp.json $work/s1cd --ptrs $work/ptrs.txt $ex --all-code \
    --map 000000-03DCF2=200000,060000-07C000=020000,03DCF2-041800=03C000 \
    --patch 33A=4E714E71 --patch 356=0080 --patch 36E=3F3F --patch D922=003F \
    --patch D700=4EB90023E000 --blob 23E000=$work/s1_bs.bin | head -1 )

print "4/5 menu art"
$py $here/menu_art.py $pic $work/menu_art.bin --unframe ${MENU_UNFRAME:-92,176,432,504}

print "5/5 SH2 (games/sh2: ../roms/sh2.bin, REV01) and the disc"
s2=(); if [[ -f ${ROM2:-$root/../roms/sh2.bin} ]]; then
  ROM=${ROM2:-$root/../roms/sh2.bin} $here/sh2_disc.sh $work/s2 | tail -1; s2=(--game 2:$work/s2); fi
s3=(); if [[ -f ${ROM3:-$root/../roms/sh3.bin} ]]; then        # SH3: front end + Angel Island
  ROM=${ROM3:-$root/../roms/sh3.bin} $here/s3engine_disc.sh sh3 $work/s3 | tail -1
  s3=(--game 3:$work/s3.front --image 10:$work/s3.zone1); fi
sk=(); if [[ -f ${ROM4:-$root/../roms/shk.bin} ]]; then         # SHK: front + Mushroom Hill
  ROM=${ROM4:-$root/../roms/shk.bin} $here/s3engine_disc.sh shk $work/sk | tail -1
  sk=(--game 4:$work/sk.front --image 11:$work/sk.zone1); fi
k3=(); if [[ -f ${ROM7:-$root/../roms/sh3k.bin} ]]; then       # SH3K: front + Angel Island
  ROM=${ROM7:-$root/../roms/sh3k.bin} $here/s3engine_disc.sh sh3k $work/k3 | tail -1
  k3=(--game 7:$work/k3.front --image 12:$work/k3.zone1); fi
bs=(); if [[ -f ${ROM5:-$root/../roms/shk_sh1.bin} && -f ${ROM4:-$root/../roms/shk.bin} ]]; then   # SHBS
  ROM=${ROM4:-$root/../roms/shk.bin} ROM1=$rom EVROM=${ROM5:-$root/../roms/shk_sh1.bin} \
    $here/s3engine_disc.sh shbs $work/bs | tail -1
  bs=(--game 5:$work/bs.front); fi
python3 $here/build_disc.py $scd $out/ultimate.iso --region E --menu $work/menu_art.bin \
  --game 1:$work/s1cd $s2 $s3 $sk $bs $k3 --scd $scd | tail -2
print "play: GENRECOMP_REGION=2 $g/build-trace/genrecomp_build/genrecomp_ref '$out/ultimate.cue'"
