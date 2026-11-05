#!/bin/zsh
# starts_frames.sh DISC.cue [FIRST LAST]: native_starts.sh's entries (run it first: the movies and the
# reference's input per poll) drawn on both builds; frames 3000, 3500 and 3990 compared pixel by pixel.
# One line per entry: the differing pixels at each frame. PS_JOBS entries at once (default 4).
g=${0:A:h:h}; cue=${1:?usage: starts_frames.sh DISC.cue [FIRST LAST]}; [[ $OSTYPE == (msys|cygwin)* ]] && cue=$(cygpath -u $cue); cue=${cue:A}   # MSYS2: C:/ to /c/ first
first=${2:-0}; last=${3:-73}; out=$g/build/native_starts
one() {
  local k=$1 m=$out/stage$1
  rm -rf $m.fr $m.fn; mkdir -p $m.fr $m.fn
  GENRECOMP_MOVIE=$m.txt PS_RENDERER=native PS_FRAMES_DIR=$m.fr PS_FRAMES_BMP=1 PS_FRAMES_FROM=3000 PS_FRAMES_EVERY=500 \
    $g/build/shcd --headless --frames 3991 $cue > /dev/null 2>&1
  GENRECOMP_MOVIE=$m.txt PS_POLL_REPLAY=$m.polls PS_FRAMES_DIR=$m.fn PS_FRAMES_BMP=1 PS_FRAMES_FROM=3000 PS_FRAMES_EVERY=500 \
    $g/build/shcd_native --headless --frames 3991 $cue > /dev/null 2>&1
  print "stage $k: $(python3 $g/tools/bmpdiff.py $m.fr $m.fn)"
}
for k in {$first..$last}; do
  one $k &
  while (( $(jobs -r | wc -l) >= ${PS_JOBS:-4} )); do sleep 1; done
done
wait
