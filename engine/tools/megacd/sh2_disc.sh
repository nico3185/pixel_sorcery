#!/bin/zsh
# sh2_disc.sh OUTPREFIX: SH2 (REV01) relocated for the Mega CD compilation disc (game 2:
# build_disc.py --game 2:OUTPREFIX). For now Emerald Hill only: the SEGA screen leads straight
# into the level (the title and the other zones are their own images, not built yet).
# Evidence: direct-start runs of Emerald Hill acts 1 and 2 on the tracer (title skipped, $FE11
# poked for act 2), randomised run/jump/spin-dash/backtrack inputs, lives topped up.
# Layout (zonepack.py): word RAM $200000-$227F00 code/data, $227F00 helpers, $228000 the Z80's
# banks; program-RAM bank 1 code/data; banks 2/3 Sonic's/Tails' art (virtual $0A0000/$0E0000).
set -eu
root=${0:A:h}/../../..; here=${0:A:h}
out=${1:A}; rom=${ROM:-$root/../roms/sh2.bin}
py=$root/engine/.venv/bin/python; g=$root/games/sh2
work=${out:h}/s2work; rm -rf $work; mkdir -p $work/ev
ninja -C $g/build-trace >/dev/null
tr=$g/build-trace/genrecomp_build/genrecomp_trace
python3 -c "
r=bytearray(open('$rom','rb').read()); r[0x1A4:0x1A8]=(0x200).to_bytes(4,'big')
r[0x346:0x34A]=bytes.fromhex('4E714E71'); r[0x3968:0x396A]=bytes.fromhex('000C')
open('$work/measure.bin','wb').write(r)
r[0x3968:0x396A]=bytes.fromhex('0004'); open('$work/measure_title.bin','wb').write(r)"   # no checksum loop; title skipped / kept
print "1/4 evidence (24 direct + 6 through the title)"
run() { i=$1; act=$(( i % 2 )); d=$work/ev/r$i; mkdir -p $d; pr=(); t=600; k=$((i * 7919 + 1))
  while [ $t -lt 16000 ]; do k=$(( (k * 1103515245 + 12345) % 2147483648 )); r=$(( k % 100 ))
    if [ $r -lt 45 ]; then pr+=(--press $t:C:$((6 + r % 14))); t=$((t + 60 + r * 3))
    elif [ $r -lt 60 ]; then pr+=(--press $t:DOWN:40 --press $((t + 10)):C:4 --press $((t + 18)):C:4); t=$((t + 120))
    elif [ $r -lt 70 ]; then pr+=(--press $t:LEFT:$((60 + r * 4))); t=$((t + 120 + r * 4))
    else t=$((t + 90)); fi; done
  pk=""; for f in $(seq 300 600 16000); do pk="$pk,FE12=09@$f"; done
  [ $act = 1 ] && for f in $(seq 150 2 180); do pk="$pk,FE11=01@$f"; done
  PS_ROMUSE=$d PS_ROMUSE_KEY=FFF600 PS_PTRS=$d/ptrs.txt PS_POKE=${pk#,} $tr --headless --frames 16000 \
    --press 500:RIGHT:15500 $pr --trace-out /dev/null $work/measure.bin >/dev/null 2>&1; }
for i in $(seq 0 23); do run $i & if (( (i + 1) % 8 == 0 )); then wait; fi; done; wait
runt() { i=$1; d=$work/ev/t$i; mkdir -p $d; pr=()   # through the title (START, then the zone poked
  for t in $(seq $((1400 + i * 37)) $((150 + i * 20)) 9000); do pr+=(--press $t:C:$((8 + i * 3))); done   # in the fade)
  pk=""; for f in $(seq 1005 5 1080); do pk="$pk,FE10=00@$f,FE11=$((i % 2))@$f"; done
  for f in $(seq 1200 600 9000); do pk="$pk,FE12=09@$f"; done
  PS_ROMUSE=$d PS_ROMUSE_KEY=FFF600 PS_PTRS=$d/ptrs.txt PS_POKE=${pk#,} $tr --headless --frames 9000 \
    --press 700:START --press 1000:START --press 1400:RIGHT:$((5000 + i * 300)) $pr --trace-out /dev/null $work/measure_title.bin >/dev/null 2>&1; }
for i in 1 2 3 4 5 6; do runt $i & done; wait
# art the image's code can queue (moveq #id,d0 + LoadPLC: game over, sign post, ...), whole
python3 $here/plc.py $rom 42660 0,1,3,27,3C,3D,42 $work/plc_ptrs.txt $work/plc_force.txt
cat $work/ev/*/ptrs.txt $work/plc_ptrs.txt | sort -u > $work/ptrs.txt
force=(); for r in $(cat $work/plc_force.txt); do force+=(--force $r); done
ex=(); for f in $work/ev/*/*.bin; do ex+=(--exec $f); done
print "2/4 references (relative, fall-through) and named tables"
$py $here/code_refs.py $rom $work/refs.txt $work/ev/*/*.bin
( cd $g && $py $here/relocate.py $rom recomp.json $work/probe $ex --all-code \
    --map 000000-100000=200000 --targets-out $work/targets.txt >/dev/null 2>&1 )
print "3/4 packing + helpers"
python3 $here/zonepack.py 0x100000 --rom $rom --use "$work/ev/*:0+C+88+8C" --near $work/targets.txt:0x200 \
  $force --fixed E8000-100000=228000 --fixed 50000-64320=0A0000 --fixed 64320-6FBE0=0E0000 \
  --refs $work/refs.txt --place 200000-227F00,020000-040000 > $work/map.txt
snd=$(python3 -c "print('%X' % (0x228000 + 0xEC000 - 0xE8000))")
m68k-elf-as -m68000 --defsym SNDLOAD=0x$snd --defsym ZB1D=0x45 --defsym ZB1E=0x46 --defsym ZB1F=0x47 \
  -o $work/h.o $here/games/sh2_helpers.s
m68k-elf-ld -Ttext=0x227F00 -o $work/h.elf $work/h.o; m68k-elf-objcopy -O binary $work/h.elf $work/h.bin
sl=$(nm $work/h.elf | awk '/ sndload$/ {print toupper($1)}'); dq=$(nm $work/h.elf | awk '/ dmaqueue$/ {print toupper($1)}')
print "4/4 relocation"
( cd $g && $py $here/relocate.py $rom recomp.json $out --ptrs $work/ptrs.txt $ex --all-code --map $(cat $work/map.txt) \
    --patch 346=4E714E71 --patch 362=0080 --patch 37A=3F3F --patch 3968=000C --patch 17B0A=003E \
    --patch 130C=4EF9$sl --patch 14AC=4EF9$dq --blob 227F00=$work/h.bin | head -1 )
print "$out.trapcart.md: build_disc.py --game 2:$out"
