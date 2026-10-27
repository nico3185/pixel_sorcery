#!/bin/zsh
# Play COI yourself; inputs and code coverage are recorded.
#   tools/play_and_record.sh /path/to/coi.md
# Writes movies/play-<date>.txt (replayable input movie) and movies/play-<date>.seeds
# (every code entry point the game executed). Merge coverage afterwards with:
#   tools/merge_seeds.py movies/play-*.seeds
# Runs on the Genesis Plus GX tracer, so recording and replay are frame-exact.
set -eu
here=${0:A:h}/..
rom=${1:?usage: play_and_record.sh ROM}
stamp=$(date +%Y%m%d-%H%M%S)
cd $here
[[ -x build-trace/genrecomp_build/genrecomp_trace ]] || {
  cmake -S . -B build-trace -G Ninja -DCMAKE_BUILD_TYPE=Release -DGENRECOMP_TRACE=ON >/dev/null
  ninja -C build-trace genrecomp_trace >/dev/null; }
mkdir -p movies movies/play-$stamp.states
print "Recording to movies/play-$stamp.txt  (close the window to stop)"
GENRECOMP_PADTYPE=${GENRECOMP_PADTYPE:-3} GENRECOMP_RECORD=movies/play-$stamp.txt \
  PS_POLL_RECORD=movies/play-$stamp.polls \
  PS_CHECKPOINTS=movies/play-$stamp.ck PS_AUDIO_LOG=movies/play-$stamp.audio \
  PS_STATE_DUMP=movies/play-$stamp.states:60 \
  build-trace/genrecomp_build/genrecomp_trace --trace-out movies/play-$stamp.seeds "$rom"
print "Saved $(wc -l < movies/play-$stamp.polls) polls (replayable on both sides), $(wc -l < movies/play-$stamp.txt) frames, $(wc -l < movies/play-$stamp.seeds) code entry points."
