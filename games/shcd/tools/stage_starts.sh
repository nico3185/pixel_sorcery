#!/bin/zsh
# stage_starts.sh DISC.cue [FIRST LAST] [FRAMES]: every entry of the stage select (title screen: Up,
# Down, Down, Left, Right, B; 74 entries on the Japanese disc), started from a scripted input movie
# and played for FRAMES frames (default 4000: run right, jump now and then). For each entry, in
# build/starts/: the movie, the overlay misses (tools/seeds_REGION.tsv material), the instructions
# still interpreted (PS_INTERP_COVER bitmap), and frames every 500 compared with a run under
# PS_INTERP=all (the compiled code must not change anything). PS_JOBS entries at once.
g=${0:A:h:h}; cue=${1:?usage: stage_starts.sh DISC.cue [FIRST LAST] [FRAMES]}; cue=${cue:A}
first=${2:-0}; last=${3:-73}; frames=${4:-4000}
out=$g/build/starts; mkdir -p $out
one() {
  local k=$1 m=$out/stage$1
  python3 -c "
import sys
k, n = int(sys.argv[1]), int(sys.argv[2]); m = {}
def press(at, b, d=4):
    for i in range(at, at + d): m[i] = b
press(300, 0x80); press(420, 0x80)                     # BIOS menu: Start, then CD-ROM
for j, b in enumerate([1, 2, 2, 4, 8, 0x10]): press(1200 + j * 20, b, 6)    # the code
for j in range(2 * k): press(1400 + j * 12, 2, 3)      # Down: the cursor moves every second press
s = 1400 + 24 * k + 30; press(s, 0x80)                 # Start: the entry
for i in range(s + 200, n):                            # then right, C every 90 frames
    m[i] = 0x08 | (0x20 if (i - s) % 90 < 10 else 0)
open(sys.argv[3], 'w').write(''.join('%x\n' % m.get(i, 0) for i in range(n)))" $k $frames $m.txt
  rm -rf $m.c $m.i; mkdir -p $m.c $m.i
  GENRECOMP_MOVIE=$m.txt PS_OVERLAY_LOG=1 PS_INTERP_COVER=$m.cov PS_RENDERER=native PS_FRAMES_DIR=$m.c PS_FRAMES_EVERY=500 \
    $g/build/shcd --headless --frames $frames $cue 2>&1 | grep 'overlay miss' > $m.miss
  GENRECOMP_MOVIE=$m.txt PS_INTERP=all PS_RENDERER=native PS_FRAMES_DIR=$m.i PS_FRAMES_EVERY=500 \
    $g/build/shcd --headless --frames $frames $cue > /dev/null 2>&1
  local d=0 f; for f in $m.i/*.png(N); do cmp -s $f $m.c/${f:t} || d=$((d+1)); done
  print "stage $k: misses $(wc -l < $m.miss | tr -d ' ') differ $d"
}
for k in {$first..$last}; do
  one $k &
  while (( $(jobs -r | wc -l) >= ${PS_JOBS:-4} )); do sleep 1; done
done
wait
