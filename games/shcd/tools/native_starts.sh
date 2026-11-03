#!/bin/zsh
# native_starts.sh DISC.cue [FIRST LAST] [FRAMES]: every stage-select entry (stage_movie.py) on the native
# build against the reference build (the same recompiled main CPU on GPGX's Mega CD hardware: drive, gate
# array, sub CPU, PCM, graphics ASIC): main RAM at every controller poll (PS_CHECKPOINTS, ckcmp.py). One
# line per entry in build/native_starts/: polls compared, how many differ, the first one. PS_JOBS entries
# at once (default 4). PS_TRACER=1 compares with the full GPGX tracer instead (main-CPU timing too).
g=${0:A:h:h}; cue=${1:?usage: native_starts.sh DISC.cue [FIRST LAST] [FRAMES]}; cue=${cue:A}
first=${2:-0}; last=${3:-73}; frames=${4:-4000}
out=$g/build/native_starts; mkdir -p $out
tr=$g/build-trace/genrecomp_build/genrecomp_trace
one() {
  local k=$1 m=$out/stage$1
  python3 $g/tools/stage_movie.py $k $frames $m.txt
  if [[ -n ${PS_TRACER:-} ]]; then
    GENRECOMP_MOVIE=$m.txt PS_CHECKPOINTS=$m.ref PS_CHECKPOINT_MASK=FC00-10000 \
      $tr --headless --frames $frames --trace-out /dev/null $cue > /dev/null 2>&1
  else
    GENRECOMP_MOVIE=$m.txt PS_CHECKPOINTS=$m.ref PS_CHECKPOINT_MASK=FC00-10000 PS_RENDERER=native PS_POLL_RECORD=$m.polls \
      $g/build/shcd --headless --frames $frames $cue > /dev/null 2>&1
  fi
  # the reference's input at each poll, replayed per poll: a load ending a frame sooner does not shift the input
  GENRECOMP_MOVIE=$m.txt PS_POLL_REPLAY=$m.polls PS_CHECKPOINTS=$m.nat PS_CHECKPOINT_MASK=FC00-10000 \
    $g/build/shcd_native --headless --frames $frames $cue > /dev/null 2>&1
  print "stage $k: $(python3 $g/tools/ckcmp.py $m.ref $m.nat)" | tee $m.result
}
for k in {$first..$last}; do
  one $k &
  while (( $(jobs -r | wc -l) >= ${PS_JOBS:-4} )); do sleep 1; done
done
wait
