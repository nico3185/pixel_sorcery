#!/bin/zsh
# generate.sh DISC.cue: SHCD's generated C, from the player's disc and BIOS. The data track's
# files go to disc/REGION (cd_extract.py: each program as an image at its load address), the BIOS
# (PS_SCD_BIOS or Application Support/PixelSorcery/bios, scd_REGION.bin) joins them, every image
# is recompiled into src/recomp/NAME, and overlay_wrap.py makes them overlays (ov_*.c), each used
# while its program is the one in memory. Nothing is redone while src/recomp/.stamp is newer than
# the disc, the BIOS, tools/seeds_REGION.tsv and the recompiler. PS_JOBS: programs generated at once (default: the cores).
set -e; setopt pipefail
g=${0:A:h:h}; root=${g:h:h}; py=$root/engine/.venv/bin/python
cue=${1:?usage: generate.sh DISC.cue}; cue=${cue:A}; [[ -f $cue ]] || { print -u2 "no disc '$cue'"; exit 2; }
track=${cue:h}/$(sed -n 's/^FILE "\(.*\)".*/\1/p' $cue | head -1)
r=$($py -I -c "import sys; d=open(sys.argv[1],'rb').read(0x210); print({'J':'jp','E':'eu'}.get(chr(d[0x10+0x1F0]),'us'))" $track)
bios=${PS_SCD_BIOS:-$HOME/Library/Application Support/PixelSorcery/bios}/scd_$r.bin
[[ -f $bios ]] || { print -u2 "no BIOS: $bios (dumped from your own console)"; exit 2; }
out=$g/src/recomp; stamp=$out/.stamp; disc=$g/disc/$r; img=$disc/img
if [[ -f $stamp ]] && [[ $(<$stamp) == $cue ]] && \
   [[ -z $(find $track $bios $root/engine/tools/recompiler/*.py $root/engine/tools/cd_extract.py \
            $root/engine/tools/megacd/overlay_wrap.py $g/tools/seeds_$r.tsv(N) $g/tools/volatile_$r.tsv(N) -newer $stamp) ]]; then
  print "SHCD: generated C up to date"; exit 0
fi
print "SHCD ($r): extracting the disc"
$py -I $root/engine/tools/cd_extract.py $track $disc > /dev/null
R=${(U)r}; cp $bios $img/BIOS_$R.bin
print '{"title": "BIOS_'$R'", "overlap_ties": "inner", "filler": "4AFC"}' > $img/BIOS_$R.json
# tools/seeds_REGION.tsv: entry points the analysis does not find (computed jumps), per program, from
# replays (PS_OVERLAY_LOG's "overlay miss NAME $ADDR")
[[ -f $g/tools/seeds_$r.tsv ]] && $py -I -c '
import json, sys
img, seeds = sys.argv[1], {}
for l in open(sys.argv[2]):
    f = l.split()
    if len(f) >= 2 and not l.startswith("#"): seeds.setdefault(f[0], []).append(int(f[1].lstrip("$"), 16))
for n, aa in seeds.items():
    try: c = json.load(open(f"{img}/{n}.json"))
    except OSError: continue
    for a in aa: c.setdefault("seeds", {}).setdefault(f"0x{a:06X}", f"sub_{a:06X}")
    json.dump(c, open(f"{img}/{n}.json", "w"))' $img $g/tools/seeds_$r.tsv
mkdir -p $out
# the lock holds its owner's pid: one left by a run that died (killed, or the script edited under it) is taken over
lock=$out.lock
if ! mkdir $lock 2>/dev/null; then
  pid=$(<$lock/pid 2>/dev/null); [[ -n $pid ]] && kill -0 $pid 2>/dev/null && { print -u2 "SHCD: another generate.sh is running ($lock, pid $pid)"; exit 1; }
  print -u2 "SHCD: stale lock from a run that died, taken over"
fi
print $$ > $lock/pid
trap 'rm -rf $lock' EXIT
rm -rf $out/*(N/) $out/*.log(N) $out/ov_*.c(N)
names=(${${(f)"$(cd $img && print -l *.json)"}%.json})
print "SHCD: $#names programs"
# one generator per program, PS_JOBS at once ($5: the program's number, $6: its name)
print -l $names | nl -ba -w1 -s' ' | xargs -P ${PS_JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc)} -L1 zsh -c '
  $0 $1/engine/tools/recompiler/generate.py $2/$6.bin -o $3/$6 -c $2/$6.json > $3/$6.log 2>&1 \
    && { rm $3/$6.log; print "[$5/$4] $6"; } || print "$6 FAILED (see $3/$6.log)"' $py $root $img $out $#names \
  | $root/engine/tools/progress.zsh "SHCD: generating"
[[ -z $(print $out/*.log(N)) ]] || exit 1
(cd $out && $py -I $root/engine/tools/megacd/overlay_wrap.py . $img $g/tools/volatile_$r.tsv)
print -r -- $cue > $stamp
