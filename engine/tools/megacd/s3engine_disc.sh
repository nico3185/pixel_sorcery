#!/bin/zsh
# s3engine_disc.sh GAME OUTPREFIX: a SH3-engine game (GAME = sh3 | shk) relocated for the
# Mega CD compilation disc as two images: the front end (OUTPREFIX.front, loaded as the card's
# game) and its first zone (OUTPREFIX.zone1, an --image group). build_disc.py arguments are
# printed at the end.
#
# Evidence (tracer, three readers: PS_ROMUSE_CPUONLY code + 68K reads, DMAONLY DMA sources,
# Z80ONLY the sound driver's bank reads): runs through the title (START presses, per game: more
# presses pause the game), randomised play, and long straight runs past the act's art switch.
# Layout:
#   word RAM $200000-$200400  free (SH3's save RAM writes land at $200001+, odd bytes)
#   $200400  DMA queue helper (games/dmaqueue.s, queue at $FFFB00)   $200500  image descriptor
#   $200510  switch thunk + stub (thunk.s, swap.s)                    $200600+ code/data
#   the Z80's banks in word RAM windows $44-$47; program-RAM bank 1 code/data; banks 2/3 the
#   game's DMA'd art window(s), unchanged (art is base + offset)
# Per game: patches (region lock, save-RAM enable, version register NTSC, boot RAM clear count),
# Process_DMA_Queue, the game-mode table, the level-load table, the sound driver (in ROM: bank
# selects patched there; unpacked by the game: patched in Z80 RAM after its loader).
set -eu
root=${0:A:h}/../../..; here=${0:A:h}
game=$1; out=${2:A}
case $game in
  sh3)
    rom=${ROM:-$root/../roms/sh3.bin}; starts=(400 700 1000)
    patches=(314=70024E71 B63C=4E714E714E714E71 6F8=0080 714=3F3F)
    dmaq=1812; mainloop=734; modetab=744; llb=5AB0C
    artfix=(--fixed 100000-120000=0A0000 --fixed 120000-140000=0E0000); rigid=(--rigid 149000-14B400)
    vbl=vec; hbl=vec; nearsz=0x100; gapsz=0x200
    driver=rom:E6000; front=0+4+4C; frontsw=8,C; zone=C+8C; zonesw=0,4,4C; card=3; zgroup=10 ;;
  shk)
    rom=${ROM:-$root/../roms/shk.bin}; starts=(400 700)
    patches=(344=72FF6000014C4E71 C32A=4E714E714E714E71 C342=4E714E714E714E71 C38A=4E714E714E714E71 C3AE=4E714E714E714E71 484=0080 33C=3F3F
             4CEB6=4E714E714E71)                # NO WAY's A+B+C: skip its tst.b $0 (the cartridge's
    dmaq=1588; mainloop=4B6; modetab=4C6; llb=91F0C   # first byte, the BIOS's on the disc)
    artfix=(--fixed 100000-120000=0A0000); rigid=(--rigid BA100-BDA00 --rigid 166500-166B00 --rigid 18BB00-18C800)
    vbl=584; hbl=FFFFF6                            # vectors point to RAM jump stubs: VBlank's
    driver=ram:12D0:D6:48:B65:33; front=0+4; frontsw=8,C; zone=C+8C; zonesw=0,4; card=4; zgroup=11 ;;
  sh3k)                                       # SHK's code + SH3 locked on ($200000-$3FFFFF)
    rom=${ROM:-$root/../roms/sh3k.bin}; starts=(400 700 1000)
    patches=(344=72006000014C4E71 C32A=4E714E714E714E71 C342=4E714E714E714E71 C38A=4E714E714E714E71 C3AE=4E714E714E714E71 20B63C=4E714E714E714E71 484=0080 33C=3F3F)
    dmaq=1588; mainloop=4B6; modetab=4C6; llb=91F0C
    artfix=(--fixed 100000-120000=0A0000 --fixed 320000-338000=0E0000      # art only DMA reads: banks 2, 3
            --fixed 18BB00-18C800=0F8000 --fixed 2A2900-2A3600=0F8D00 --fixed 383700-387400=0F9A00)
    rigid=(--rigid 367300-367E00)
    vbl=584; hbl=FFFFF6; zwin=47,46,45; nearsz=0x100; gapsz=0x200   # 4 MB: tighter packing
    driver=ram:12D0:D6:48:B65:33; front=0+4+4C; frontsw=8,C; zone=C+8C; zonesw=0,4,4C; card=7; zgroup=12 ;;
  shbs)                                    # SH1 + SHK: SHBS (SHK's code alone)
    rom=${ROM:-$root/../roms/shk.bin}; s1=${ROM1:-$root/../roms/sh1.bin}; gdir=shk
    evrom=${EVROM:-$root/../roms/shk_sh1.bin}; nopoke=1   # lives/timer RAM differ in SHBS
    starts=(400 700); extra=(--press 1000:A+B+C:30 --press 1300:START); goafter=1500
    lockhdr=$s1                                   # the locked-on cartridge's header, at $200100
    patches=(344=7801600000D04E71 C32A=4E714E714E714E71 C342=4E714E714E714E71 C38A=4E714E714E714E71 C3AE=4E714E714E714E71 484=0080 33C=3F3F
             4CEB6=4E714E714E71)                # NO WAY's A+B+C: skip its tst.b $0 (the cartridge's
    dmaq=1588; mainloop=4B6; modetab=4C6; llb=91F0C   # first byte, the BIOS's on the disc)
    artfix=(--fixed 100000-120000=0A0000); rigid=(--rigid BA100-BDA00 --rigid 166500-166B00 --rigid 18BB00-18C800)
    vbl=584; hbl=FFFFF6
    driver=ram:12D0:D6:48:B65:33; front=0+2C+34+38+48; frontsw=; zone=; zonesw=; card=5; zgroup= ;;
  *) print -u2 "unknown game $game"; exit 2 ;;
esac
zwin=${zwin:-47,46,45,44}                         # word RAM windows for the sound driver's banks
nearsz=${nearsz:-0x200}; gapsz=${gapsz:-0x800}     # table margin, data merge gap (zonepack.py)
py=$root/engine/.venv/bin/python; g=$root/games/${gdir:-$game}   # project (recomp.json, tracer)
evrom=${evrom:-$rom}                              # evidence ROM (SHBS: the lock-on dump)
romsize=$(python3 -c "import os;print(hex(os.path.getsize('$rom')))")
work=${out:h}/${game}work; rm -rf $work; mkdir -p $work/cpu $work/dma $work/z80
ninja -C $g/build-trace >/dev/null
tr=$g/build-trace/genrecomp_build/genrecomp_trace
title=(); for t in $starts; do title+=(--press $t:START); done
title+=(${extra[@]:-})                            # per game: more title presses (SHBS's A+B+C)
go=$(( ${goafter:-$(( ${starts[-1]} + 1000 ))} ))  # play starts after the title and the level load
inputs() { i=$1; pr=($title --press $go:RIGHT:$((10000 + i * 300)))
  (( i % 2 == 0 )) && pr+=(--press $((go + 900 + i * 50)):START --press $((go + 1100 + i * 50)):START)   # pause, resume
  t=$((go + 100)); k=$((i * 7919 + 1)); while [ $t -lt 13000 ]; do k=$(( (k * 1103515245 + 12345) % 2147483648 )); r=$(( k % 100 ))
    if [ $r -lt 50 ]; then pr+=(--press $t:C:$((6 + r % 14))); t=$((t + 60 + r * 3))
    elif [ $r -lt 62 ]; then pr+=(--press $t:DOWN:30 --press $((t + 8)):C:4 --press $((t + 16)):C:4); t=$((t + 110))
    elif [ $r -lt 70 ]; then pr+=(--press $t:LEFT:$((60 + r * 3))); t=$((t + 120 + r * 3))
    else t=$((t + 90)); fi; done
  pk=""; for f in $(seq $go 600 13000); do (( nopoke )) || pk="$pk,FE12=09@$f"; done; echo ${pk#,}; }
cap() { kind=$1; i=$2; d=$work/$kind/r$i; mkdir -p $d; inputs $i >/dev/null; pk=$(inputs $i)
  env PS_ROMUSE_${(U)kind}ONLY=1 PS_ROMUSE=$d PS_ROMUSE_KEY=FFF600 PS_PTRS=$d/ptrs.txt PS_POKE=$pk \
    $tr --headless --frames 13000 $pr --trace-out /dev/null $evrom >/dev/null 2>&1; }
offs=(0 400 800 1200 15 37 61 90)                 # start offsets: play is chaotic (15 frames
straight() { kind=$1; i=$2; d=$work/$kind/s$i; mkdir -p $d; st=$((go + offs[i + 1]))   # later meet other objects)
  pr=($title --press $st:RIGHT:18000)                                   # objects met at other times
  for t in $(seq $((st + 100 + i * 23)) $((60 + i * 15)) 20000); do pr+=(--press $t:C:$((10 + i * 4))); done
  pk=""; for f in $(seq $go 600 20000); do (( nopoke )) || pk="$pk,FE12=09@$f"; done
  env PS_ROMUSE_${(U)kind}ONLY=1 PS_ROMUSE=$d PS_ROMUSE_KEY=FFF600 PS_PTRS=$d/ptrs.txt PS_POKE=${pk#,} \
    $tr --headless --frames 20000 $pr --trace-out /dev/null $evrom >/dev/null 2>&1; }
print "1/5 evidence"
for i in 0 1 2 3 4 5; do cap cpu $i & done; wait
for i in 0 1 2; do cap dma $i & cap z80 $i & done; wait
for i in 0 1 2 3; do straight cpu $i & straight dma $i & straight z80 $i & done; wait
for i in 4 5 6 7; do straight cpu $i & straight dma $i & done; wait
die() { kind=$1; d=$work/$kind/die; mkdir -p $d                      # deaths and restarts: the timer
  pk="FE12=09@$go"; for f in $(seq $((go + 2000)) 4000 20000); do      # set to 9:59 (time over),
    pk="$pk,FE23=09@$f,FE24=3B@$f,FE12=09@$((f + 10))"; done           # lives kept up
  env PS_ROMUSE_${(U)kind}ONLY=1 PS_ROMUSE=$d PS_ROMUSE_KEY=FFF600 PS_PTRS=$d/ptrs.txt PS_POKE=$pk \
    $tr --headless --frames 20000 $title --press $go:RIGHT:18000 --trace-out /dev/null $evrom >/dev/null 2>&1; }
(( nopoke )) || { die cpu & die dma & die z80 & wait }
# the level-load table (24-byte entries of tagged pointers, indexed by zone/act and special cases
# such as an intro or a restart): every entry's pointers, read or not
python3 -c "
r=open('$rom','rb').read(); t=0x$llb
print('\n'.join('%06X' % a for a in range(t, t + 24 * 64, 4) if 0x200 <= int.from_bytes(r[a+1:a+4],'big') < len(r)))" > $work/llb_ptrs.txt
cat $work/cpu/*/ptrs.txt $work/llb_ptrs.txt | sort -u > $work/ptrs.txt
ex=(); for f in $work/cpu/*/*.bin $work/dma/*/*.bin; do ex+=(--exec $f); done
print "2/5 code ties, named tables"
( cd $g && $py $here/relocate.py $rom recomp.json $work/probe $ex --all-code \
    --map 000000-400000=400000 --targets-out $work/targets.txt --funcs-out $work/funcs.txt >/dev/null 2>&1 )
$py $here/code_refs.py $rom $work/refs.txt $work/cpu/*/*.bin --funcs $work/funcs.txt
print "3/5 sound driver banks"
zp=(); blobs=()
if [[ $driver == rom:* ]]; then                   # driver copied as it is: patch the ROM
  python3 $here/z80plan_s3.py $rom ${driver#rom:} $zwin $work/z80fixed.txt $work/z80patch.txt $work/z80/*/*.bin
  for p in $(cat $work/z80patch.txt); do zp+=(--patch $p); done
else                                              # unpacked into Z80 RAM: patch it there after the loader
  parts=(${(s.:.)driver}); loader=${parts[2]}
  PS_Z80_DUMP=$work/z80dump.bin $tr --headless --frames 600 --trace-out /dev/null $evrom >/dev/null 2>&1
  python3 $here/z80plan_ram.py $work/z80dump.bin $zwin $work/z80fixed.txt $work/z80ram.txt \
    --table ${parts[3]}:${parts[4]} --table ${parts[5]}:${parts[6]} $work/z80/*/*.bin
  python3 -c "
import sys
out=bytearray()
for l in open('$work/z80ram.txt'):
    o,v=l.split(); out+=int(o,16).to_bytes(2,'big')+int(v,16).to_bytes(2,'big')
open('$work/z80patch.bin','wb').write(out)"
fi
fixed=(); for r in $(cat $work/z80fixed.txt); do fixed+=(--fixed $r); done
print "4/5 helpers"
m68k-elf-as -m68000 --defsym QBASE=0xFB00 --defsym QEND=0xFBFC -o $work/dq.o $here/games/dmaqueue.s
m68k-elf-ld -Ttext=0x200400 -o $work/dq.elf $work/dq.o; m68k-elf-objcopy -O binary $work/dq.elf $work/dq.bin
m68k-elf-as -m68000 -o $work/sw.o $here/swap.s; m68k-elf-ld -Ttext=0xFFFD20 --oformat=binary -o $work/swap.bin $work/sw.o
# image NAME MODES TARGET_GROUP SWITCH_MODES
image() { name=$1; modes=$2; tg=$3; sw=$4
  mm=(); for x in ${(s:+:)modes}; do mm+=($(printf '%02X' $((0x$x)))); done
  exm=(); for f in $work/cpu/*/*.bin; do (( ${mm[(Ie)${${f:t}[1,2]}]} )) && exm+=(--exec $f); done
  ( cd $g && $py $here/relocate.py $rom recomp.json $work/probe_$name $exm \
      --map 000000-400000=400000 --funcs-out $work/funcs_$name.txt >/dev/null 2>&1 )
  python3 $here/zonepack.py $romsize --rom $rom \
    --use "$work/cpu/*:$modes" --use "$work/dma/*:$modes" $artfix $rigid \
    --near $work/targets.txt:$nearsz --gap $gapsz $fixed --refs $work/refs.txt --force $llb-$(printf '%X' $((0x$llb + 24 * 64))) \
    --force-file $work/funcs_$name.txt --place 200600-23F000,020000-040000 > $work/map_$name.txt
  python3 - $rom $work/map_$name.txt $work/desc_$name.bin $mainloop $vbl $hbl <<'PY'
import sys
rom = open(sys.argv[1], 'rb').read()
m = [(int(a.split('-')[0], 16), int(a.split('-')[1].split('=')[0], 16), int(a.split('=')[1], 16))
     for a in open(sys.argv[2]).read().strip().split(',')]
f = lambda x: x if x >= 0xFF0000 else next(new + x - lo for lo, hi, new in m if lo <= x < hi)
v = lambda i: int.from_bytes(rom[i * 4:i * 4 + 4], 'big')
vb = v(30) if sys.argv[5] == 'vec' else int(sys.argv[5], 16)
hb = v(28) if sys.argv[6] == 'vec' else int(sys.argv[6], 16)
d = f(vb).to_bytes(4, 'big') + f(hb).to_bytes(4, 'big') + f(int(sys.argv[4], 16)).to_bytes(4, 'big') + (0xFFFFFE00).to_bytes(4, 'big')
open(sys.argv[3], 'wb').write(d)
PY
  m68k-elf-as -m68000 -I $work --defsym GROUP=$tg -o $work/th.o $here/thunk.s
  m68k-elf-ld -Ttext=0x200510 --oformat=binary -o $work/thunk_$name.bin $work/th.o
  tp=(); for md in ${(s:,:)sw}; do tp+=(--patch $(printf '%X' $((0x$modetab + 0x$md)))=00200510); done
  zb=()
  if [[ $driver == ram:* ]]; then                 # Z80 RAM patcher at $23F000, after the loader
    ld=$(python3 -c "
m=[(int(a.split('-')[0],16),int(a.split('-')[1].split('=')[0],16),int(a.split('=')[1],16)) for a in open('$work/map_$name.txt').read().strip().split(',')]
v=0x$loader; print('%X'%next(new+v-lo for lo,hi,new in m if lo<=v<hi))")
    # the loader's first instruction (move.w #$100,$A11100, 8 bytes) becomes jmp helper + nop;
    # the helper takes the Z80 bus itself and calls the loader's rest
    ( cd $work && m68k-elf-as -m68000 -I $work --defsym SNDLOAD=0x$(printf '%X' $(( 0x$ld + 8 ))) -o z.o $here/games/z80ram.s \
      && m68k-elf-ld -Ttext=0x23F000 --oformat=binary -o z80ram_$name.bin z.o )
    zb=(--patch $loader=4EF90023F0004E71 --blob 23F000=$work/z80ram_$name.bin)
  fi
  pt=(); for p in $patches; do pt+=(--patch $p); done
  if [[ -n ${lockhdr:-} ]]; then                  # SHK reads the locked-on cartridge's header
    python3 -c "r=open('$lockhdr','rb').read(); open('$work/lockhdr.bin','wb').write(r[0x100:0x200])"   # ($200100 name,
    pt+=(--blob 200100=$work/lockhdr.bin)         # $200180 serial: SH1 = SHBS's full game)
  fi
  ( cd $g && $py $here/relocate.py $rom recomp.json $out.$name --strict-imm --scan-tables --ptrs $work/ptrs.txt $ex --all-code --map $(cat $work/map_$name.txt) \
      $pt --patch $dmaq=4EF900200400 $zp $tp $zb \
      --blob 200400=$work/dq.bin --blob 200500=$work/desc_$name.bin --blob 200510=$work/thunk_$name.bin | head -1 ) }
print "5/5 images"
if [[ -n $zone ]]; then
  image front $front $zgroup $frontsw
  image zone1 $zone $card $zonesw
  print "build_disc.py --game $card:$out.front --image $zgroup:$out.zone1"
else                                              # one image: nothing to switch to
  image front $front 0 ""
  print "build_disc.py --game $card:$out.front"
fi
