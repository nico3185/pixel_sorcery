#!/bin/zsh
# stage_starts.sh DISC.cue [FIRST LAST] [FRAMES]: every entry of the stage select (title screen: Up,
# Down, Down, Left, Right, B; 74 entries on the Japanese disc), started from a scripted input movie
# and played for FRAMES frames (default 4000: run right, jump now and then). For each entry, in
# build/starts/: the movie, the overlay misses (tools/seeds_REGION.tsv material), the instructions
# still interpreted (PS_INTERP_COVER bitmap), and frames every 500 compared with a run under
# PS_INTERP=all (the compiled code must not change anything). PS_JOBS entries at once.
g=${0:A:h:h}; cue=${1:?usage: stage_starts.sh DISC.cue [FIRST LAST] [FRAMES]}; [[ $OSTYPE == (msys|cygwin)* ]] && cue=$(cygpath -u $cue); cue=${cue:A}   # MSYS2: C:/ to /c/ first
first=${2:-0}; last=${3:-73}; frames=${4:-4000}
out=$g/build/starts; mkdir -p $out
one() {
  local k=$1 m=$out/stage$1
  python3 $g/tools/stage_movie.py $k $frames $m.txt
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
