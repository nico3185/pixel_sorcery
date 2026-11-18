#!/bin/zsh
# sh3_disc.sh OUTPREFIX: SH3 (USA) for the Mega CD compilation disc as one image per zone or act (0.7.3),
# sh2_disc.sh's method on s3engine_disc.sh's layout. Writes OUTPREFIX.front (card 3: SEGA, title, menus,
# level select, the special and bonus stages), OUTPREFIX.<zone or act> and OUTPREFIX.args (build_disc.py's
# --game 3:... --image G:... arguments, groups 36-47).
# Evidence: the level-select starts (games/sh3/recordings/start-*.polls) and front paths (front-*.polls) replayed on the tracer, split by
# reader (PS_ROMUSE_CPUONLY / DMAONLY / Z80ONLY): keyed by zone/act ($FFFE10) while a level runs
# (PS_ROMUSE_LEVEL), and by game mode ($FFF600) for the front. SH3 reads no checksum at boot: from frame 0.
# Layout, every image:
#   word RAM $200B00 DMA queue helper (games/dmaqueue_map.s, queue at $FFFB00; images from $200D00), $200500 the image's descriptor
#   (VBlank, HBlank, main loop, stack), $200510 the switch (games/sh3_zone.s + swap.s), $200700-$23F000
#   code/data around the sound driver's banks (z80plan_s3.py: four 32 KB windows, $40-$47);
#   program-RAM bank 1 ($020000-$03FFFF) code/data; banks 2/3 the player art ($100000-$140000, fixed).
# S3_KEEP_EV=1 keeps the last captures; S3_ONLY="hcz1 front" rebuilds those images; S3_SIZES=1 only measures.
set -eu
root=${0:A:h}/../../..; here=${0:A:h}
out=${1:A}; rom=${ROM:-$root/../roms/sh3.bin}
py=$root/engine/.venv/bin/python; g=$root/games/sh3
work=${out:h}/s3work; [[ -n ${S3_KEEP_EV:-} ]] || rm -rf $work; mkdir -p $work/cpu $work/dma $work/z80 $work/mcpu $work/mdma
ninja -C $g/build-trace >/dev/null
tr=$g/build-trace/genrecomp_build/genrecomp_trace
# + the front's own paths the level select skips (recordings/front-*.polls, made on the cartridge): the attract
# demos (title left alone: AIZ's intro, HCZ) and data select -> No save -> AIZ1 - without them data select hung
# (black screen) and the demos showed a blank screen on the disc. Made here when missing: a movie (one pad byte a
# frame) played on the tracer, its polls recorded
mkdir -p $g/recordings
mv_front() {  # NAME FRAMES PRESSES: "frame:value[:frames]" each
  [[ -f $g/recordings/front-$1.polls ]] && return
  python3 -c "
import sys
m = {}
for p in sys.argv[2:]:
    f = p.split(':'); a, v, n = int(f[0]), int(f[1], 16), int(f[2]) if len(f) > 2 else 4
    for i in range(a, a + n): m[i] = v
open('$work/front-$1.mv', 'w').write(''.join('%x\n' % m.get(i, 0) for i in range(int(sys.argv[1]))))" ${@:2}
  GENRECOMP_MOVIE=$work/front-$1.mv PS_POLL_RECORD=$g/recordings/front-$1.polls $tr --headless --frames $2 \
    --trace-out /dev/null $rom >/dev/null 2>&1
}
mv_front demo 6000                                 # the title left alone: the AIZ and HCZ demos
ds=(600:80 900:80); for f in $(seq 2600 4199); do (( (f % 90) < 6 )) && ds+=($f:48:1) || ds+=($f:08:1); done
mv_front dataselect 4400 $ds                       # Start, Start (No save), right with a jump every 90 frames
print "1/4 evidence: $(ls $g/recordings/start-*.polls | wc -l | tr -d ' ') level-select starts, ${#${(f)"$(print -l $g/recordings/front-*.polls(N))"}} front paths"
i=0
for p in $g/recordings/start-*.polls $g/recordings/front-*.polls(N); do
  b=${p:t:r}
  [[ -n ${S3_KEEP_EV:-} && -n "$(print -l $work/cpu/$b/*.bin(N))" ]] && continue
  n=$(wc -l < $p); fr=$((n * 2 + 600))
  mkdir -p $work/cpu/$b $work/dma/$b $work/z80/$b $work/mcpu/$b $work/mdma/$b
  PS_ROMUSE_CPUONLY=1 PS_ROMUSE_LEVEL=1 PS_ROMUSE=$work/cpu/$b PS_ROMUSE_KEY=FFFE10 PS_PTRS=$work/cpu/$b/ptrs.txt PS_PTRS_NEAR=0x80 \
    PS_POLL_REPLAY=$p $tr --headless --frames $fr --trace-out /dev/null $rom >/dev/null 2>&1 &
  PS_ROMUSE_DMAONLY=1 PS_ROMUSE_LEVEL=1 PS_ROMUSE=$work/dma/$b PS_ROMUSE_KEY=FFFE10 \
    PS_POLL_REPLAY=$p $tr --headless --frames $fr --trace-out /dev/null $rom >/dev/null 2>&1 &
  PS_ROMUSE_Z80ONLY=1 PS_ROMUSE=$work/z80/$b PS_ROMUSE_KEY=FFFE10 \
    PS_POLL_REPLAY=$p $tr --headless --frames $fr --trace-out /dev/null $rom >/dev/null 2>&1 &
  PS_ROMUSE_CPUONLY=1 PS_ROMUSE=$work/mcpu/$b PS_ROMUSE_KEY=FFF600 PS_PTRS=$work/mcpu/$b/ptrs.txt PS_PTRS_NEAR=0x80 \
    PS_POLL_REPLAY=$p $tr --headless --frames $fr --trace-out /dev/null $rom >/dev/null 2>&1 &
  PS_ROMUSE_DMAONLY=1 PS_ROMUSE=$work/mdma/$b PS_ROMUSE_KEY=FFF600 \
    PS_POLL_REPLAY=$p $tr --headless --frames $fr --trace-out /dev/null $rom >/dev/null 2>&1 &
  i=$((i + 1)); (( i % ${PS_JOBS:-2} == 0 )) && wait
done; wait
print "2/4 pointers, references, sound banks"
# the level-load table ($5AB0C: 24-byte entries of tagged pointers, by zone/act and special cases): every
# entry's pointers, read or not
python3 -c "
r=open('$rom','rb').read(); t=0x5AB0C
print('\n'.join('%06X' % a for a in range(t, t + 24 * 64, 4) if 0x200 <= int.from_bytes(r[a+1:a+4],'big') < len(r)))" > $work/llb_ptrs.txt
# a pointer seen read extends to its table's neighbours (stride 4 or 8, even, within 32 KB) - not in the
# player art ($100000-$140000: pixels), the sound driver's data ($E6000-$100000: the Z80's), the level
# limits ($1A1F4-$1A374: 48 acts x min/max x/y words, read as longs - the camera limits came out wrong and
# 7 starts drifted early), the palettes ($8C134-$8CC00: colour pairs, white read as an address), nor the PLC
# lists (a VRAM word and the next list's count read as a long: 4 lists' counts were rewritten)
# the pattern load cues ($5AF8C: word offsets to lists of "count - 1, (art.l, VRAM.w) x count"), every list whole
# and every art pointer in them: a list is read from its start, so its unread entries were holes other data
# filled (the title queued 17 entries and ran over the queue on the disc)
python3 $here/plc.py $rom 5AF8C all $work/plc_ptrs.txt $work/plc_force.txt >/dev/null
cat $work/cpu/*/ptrs.txt $work/mcpu/*/ptrs.txt $work/llb_ptrs.txt $work/plc_ptrs.txt | sort -u | python3 -c "
import sys
rom = open(sys.argv[1], 'rb').read(); n = len(rom)
L = lambda a: int.from_bytes(rom[a:a + 4], 'big') if 0 <= a <= n - 4 else -1
bad = lambda a: 0x100000 <= a < 0x140000 or 0xE6000 <= a < 0x100000 or 0x1A1F4 <= a < 0x1A374 or 0x8C134 <= a < 0x8CE74
plc = lambda a: 0x5AF8C <= a < 0x5B520           # the PLC lists: 6-byte entries, plc.py's pointers only
seen = [int(l, 16) for l in sys.stdin]; out = set(seen)
for p in seen:
    v = L(p) & 0xFFFFFF
    for st in (4, 8, -4, -8):
        q = p + st
        for _ in range(64):
            w = L(q)
            if not (0x200 <= w < n and not w & 1 and abs(w - v) < 0x8000) or bad(q) or plc(q): break
            out.add(q); q += st
plcp = {int(l, 16) for l in open(sys.argv[2])}   # in the PLC lists only plc.py's art pointers (a count and the
# next pointer's high word, 0x00030015, read as one long by the tracer, rewrote 4 lists' counts)
# a target below 0x7700 (the boot and engine code) is never a data pointer in SH3 - of 178 such entries only
# the vectors were real (relocate.py moves those anyway): water levels (0x5324: level, x word pairs), palette
# words and x/y pairs whose low word is an executed instruction start had been rewritten (Hydrocity's water)
# except the SEGA intro's frame table (0x3BF0, 11 entries of art, art, palette): its palettes (0x3D28-0x3E88) lie
# among the engine's code, and unrelocated they were read from the Mega CD's BIOS ROM there (the intro's colours garbled)
intro = {0x3BF8 + 12 * k for k in range(11)}
low = lambda a: a >= 0x100 and 0x200 <= (L(a) & 0xFFFFFF) < 0x7700 and a not in intro
out |= intro
sys.stdout.writelines('%06X\n' % a for a in sorted(out) if not bad(a) and not low(a) and (not plc(a) or a in plcp))" $rom $work/plc_ptrs.txt > $work/ptrs.txt
ex=(); for f in $work/cpu/*/*.bin(N) $work/mcpu/*/*.bin(N); do ex+=(--exec $f); done
( cd $g && $py $here/relocate.py $rom recomp.json $work/probe $ex --all-code \
    --map 000000-200000=400000 --targets-out $work/targets.txt >/dev/null 2>&1 )
print "3/4 helpers"
m68k-elf-as -m68000 --defsym QBASE=0xFB00 --defsym QEND=0xFBFC --defsym ART2LO=0x100000 --defsym ART2HI=0x120000 --defsym ART3LO=0x120000 --defsym ART3HI=0x140000 -o $work/dq.o $here/games/dmaqueue_map.s
m68k-elf-as -m68000 -o $work/home.o $here/games/home.s && m68k-elf-ld -Ttext=0x200300 --oformat=binary -o $work/home.bin $work/home.o   # A+B+C+Start: the menu ($200300: at $200200 SH3 faulted on its first instruction, not understood yet)
m68k-elf-ld -Ttext=0x200B00 -o $work/dq.elf $work/dq.o; m68k-elf-objcopy -O binary $work/dq.elf $work/dq.bin
(( $(wc -c < $work/dq.bin) <= 0x200 )) || { print -u2 "dmaqueue_map.s: over 512 bytes (images start at \$200D00)"; exit 1; }
m68k-elf-as -m68000 --defsym ABSORB=1 --defsym LOADSCREEN=1 -o $work/sw.o $here/swap.s
m68k-elf-ld -Ttext=0xFFFD20 --oformat=binary -o $work/swap.bin $work/sw.o
# the images and their groups; zone x 2 + act -> image (ACTS table); captures by zone/act (cpu/dma, keys)
names=(front aiz1 aiz2 hcz1 hcz2 mgz1 mgz2 cnz icz lbz comp)
typeset -A grp=(front 3 aiz1 36 aiz2 37 hcz1 38 hcz2 39 mgz1 40 mgz2 41 cnz 42 icz 43 lbz 44 comp 45)
typeset -A keys=(aiz1 0000 aiz2 0001 hcz1 0100 hcz2 0101 mgz1 0200 mgz2 0201 cnz "0300 0301" icz "0500 0501"
  lbz "0600 0601" comp "0E00 0E01 0F00 0F01 1000 1001 1100 1101 1200 1201 1300 1301")
print "4/4 images"
args=()
for name in $names; do
  if [[ -n ${S3_ONLY:-} && ${${=S3_ONLY}[(Ie)$name]} -eq 0 ]]; then
    [[ $name == front ]] && args+=(--game 3:$out.front) || args+=(--image $grp[$name]:$out.$name); continue; fi
  # its captures: by game mode for the front (every mode but the levels), by zone/act for the others
  d=$work/use_$name; rm -rf $d; mkdir -p $d/cpu $d/dma
  if [[ $name == front ]]; then
    for f in $work/mcpu/*/*.bin(N); do k=${${f:t}[1,2]}; [[ $k != (08|0C|88|8C) ]] && ln -sf $f $d/cpu/${f:h:t}_${f:t}; done
    for f in $work/mdma/*/*.bin(N); do k=${${f:t}[1,2]}; [[ $k != (08|0C|88|8C) ]] && ln -sf $f $d/dma/${f:h:t}_${f:t}; done
  else
    for k in ${=keys[$name]}; do
      for f in $work/cpu/*/$k.bin(N); do ln -sf $f $d/cpu/${f:h:t}_${f:t}; done
      for f in $work/dma/*/$k.bin(N); do ln -sf $f $d/dma/${f:h:t}_${f:t}; done; done
  fi
  # the sound driver's banks this image's music, effects and samples use, into the four word-RAM windows
  # (z80plan_s3.py; all the zones' together need more banks than four windows hold)
  zk=(${=keys[$name]:-0000}); zf=(); for k in $zk; do zf+=($work/z80/*/$k.bin(N)); done
  python3 $here/z80plan_s3.py $rom E6000 47,46,45,44 $work/z80fixed_$name.txt $work/z80patch_$name.txt $zf
  zp=(); for p in $(cat $work/z80patch_$name.txt); do zp+=(--patch $p); done
  zfix=(); for r in $(cat $work/z80fixed_$name.txt); do zfix+=(--fixed $r); done
  exm=(); for f in $d/cpu/*.bin(N); do exm+=(--exec $f); done
  (( $#exm )) || { print -u2 "$name: no captures"; exit 1; }
  ( cd $g && $py $here/relocate.py $rom recomp.json $work/probe_$name $exm \
      --map 000000-200000=400000 --funcs-out $work/funcs_$name.txt >/dev/null 2>&1 )
  $py $here/code_refs.py $rom $work/refs_$name.txt ${exm:#--exec} --funcs $work/funcs_$name.txt --tables-out $work/tables_$name.txt >/dev/null
  # the tables: MODES (32: game mode / 4) and ACTS (48: zone x 2 + act, zones $00-$17), 0 = here, else $80 + group;
  # $FF = a level mode in an image without levels (the front): the zone decides
  $py - $name "${(@kv)keys}" > $work/tables.inc <<'P'
import sys
me = sys.argv[1]; kv = sys.argv[2:]
grp = {'front': 3, 'aiz1': 36, 'aiz2': 37, 'hcz1': 38, 'hcz2': 39, 'mgz1': 40, 'mgz2': 41, 'cnz': 42, 'icz': 43, 'lbz': 44, 'comp': 45}
acts = ['aiz1'] * 48                              # zones $00-$17; one no image holds (SHK's): Angel Island
for i in range(0, len(kv), 2):
    for k in kv[i + 1].split():
        z, a = int(k[:2], 16), int(k[2:], 16)
        if z < 24: acts[z * 2 + a] = kv[i]
G = lambda n: 0 if n == me else 0x80 + grp[n]
level = (2, 3)                                    # modes $08 (demo) and $0C (level), / 4
modes = [(0xFF if i in level else 0) if me == 'front' else (0 if i in level else G('front')) for i in range(32)]
print('modes:\t.byte\t' + ', '.join('0x%02X' % m for m in modes))
print('acts:\t.byte\t' + ', '.join('0x%02X' % G(a) for a in acts))
P
  # collision: the angle and height arrays ($704A0-$716A0, rotated $726A0-$736A0) read as base + index, and
  # each act's block-index pair (pointer at $746A0 + zone x 8 + act x 4; primary P, secondary P + $600 set by
  # code), whole and at their distances - only used pages had been placed, so P + $600 or an array entry
  # read other data (Sonic left the floor at once in Endless Mine; y off in Hydrocity 2, Launch Base 2)
  coll=(--force 704A0-716A0 --rigid 704A0-716A0 --force 726A0-736A0 --rigid 726A0-736A0)
  for k in ${=keys[$name]:-}; do
    cp_=$(python3 -c "
r = open('$rom', 'rb').read(); z, a = int('$k'[:2], 16), int('$k'[2:], 16); t = 0x746A0 + z * 8 + a * 4
p = int.from_bytes(r[t:t + 4], 'big') & 0xFFFFFF
print('%06X-%06X' % (p, p + 0xC00) if 0x200 <= p < len(r) else '')")
    [[ -n $cp_ ]] && coll+=(--force $cp_ --rigid $cp_)
  done
  pack() {  # NEAR GAP; an image that does not fit is packed again tighter
    python3 $here/zonepack.py 0x200000 --rom $rom --use "$d/cpu:*" --dma-use "$d/dma:*" \
      --near $work/targets.txt:$1 --gap $2 \
      --fixed 100000-120000=0A0000 --fixed 120000-140000=0E0000 $zfix --rigid 149000-14B400 \
      $coll --force 5AB0C-5B10C --force 5AF8C-5B520 --force 000000-000800 --refs $work/refs_$name.txt \
      --force-file $work/funcs_$name.txt --force-file $work/tables_$name.txt \
      --place ${S3_SIZES:+400000-600000,}200D00-23F000,020000-040000 > $work/map_$name.txt 2> $work/pack_$name.txt; }
  pack 0x200 0x800 || pack 0x100 0x200 || { print -u2 "$name: does not fit ($(tail -1 $work/pack_$name.txt))"; exit 1; }
  [[ -n ${S3_SIZES:-} ]] && { print "  $name: $(tail -1 $work/pack_$name.txt)"; continue; }
  at() { python3 -c "
import sys
m=[(int(a.split('-')[0],16),int(a.split('-')[1].split('=')[0],16),int(a.split('=')[1],16)) for a in open('$work/map_$name.txt').read().strip().split(',')]
v=int(sys.argv[1],16); print('%X' % next((new+v-lo for lo,hi,new in m if lo<=v<hi), 0x200510))" $1; }
  python3 -c "import sys; sys.stdout.buffer.write(b''.join(int(x,16).to_bytes(4,'big') for x in sys.argv[1:]))" \
    $(at 802) $(at F9E) 200700 FFFFFE00 > $work/desc_$name.bin   # entry: games/z80fix.s, then the main loop
  # the driver's bank tables for this image (z80patch_<image>.txt: ROM $E6000 + offset = HEX bytes) as records
  python3 - $work/z80patch_$name.txt $work/z80patch.bin <<'Z'
import sys
out = bytearray()
for l in open(sys.argv[1]):
    if '=' not in l: continue
    a, h = l.strip().split('='); b = bytes.fromhex(h); o = int(a, 16) - 0xE6000
    rec = o.to_bytes(2, 'big') + (len(b) - 1).to_bytes(2, 'big') + b
    out += rec + (b'\0' if len(rec) & 1 else b'')
open(sys.argv[2], 'wb').write(out + b'\xff\xff')
Z
  m68k-elf-as -m68000 -I $work --defsym MAINLOOP=0x$(at 734) -o $work/zf.o $here/games/z80fix.s
  m68k-elf-ld -Ttext=0x200700 --oformat=binary -o $work/zf_$name.bin $work/zf.o
  (( $(wc -c < $work/zf_$name.bin) <= 0x400 )) || { print -u2 "$name: z80fix over 1 KB"; exit 1; }
  m68k-elf-as -m68000 -I $work --defsym TABLE=0x$(at 744) --defsym LEVELREST=0x$(at 46B6) -o $work/z.o $here/games/sh3_zone.s
  m68k-elf-ld -Ttext=0x200510 --oformat=binary -o $work/zone_$name.bin $work/z.o
  # the object table ($5CC96: 212 code pointers, decoded as instructions by --all-code, so only the entries a
  # run read were rewritten - Hydrocity 1 and Marble Garden 1 jumped to raw ROM addresses): data, every entry
  ( cd $g && $py $here/relocate.py $rom recomp.json $out.$name --strict-imm --ptrs $work/ptrs.txt $ex --all-code --data 05CC96-05CFE6 \
      --map $(cat $work/map_$name.txt) \
      --patch 314=70024E71 --patch B63C=4E714E714E714E71 --patch 6F8=0080 --patch 714=3F3F \
      --patch 734=4EB9002005104E714E714E714E71 --patch 46B0=4EF900200514 --patch 1812=4EF900200B00 $zp \
      --blob 200B00=$work/dq.bin --blob 200500=$work/desc_$name.bin --blob 200510=$work/zone_$name.bin --patch 1372=4EF900200300 --blob 200300=$work/home.bin \
      --blob 200700=$work/zf_$name.bin | head -1 )
  print "  $name: $(tail -1 $work/pack_$name.txt)"
  [[ $name == front ]] && args+=(--game 3:$out.front) || args+=(--image $grp[$name]:$out.$name)
done
print -rl -- $args > $out.args
print "build_disc.py $args"
