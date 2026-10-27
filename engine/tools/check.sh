#!/bin/zsh
# check.sh TITLE RECORDING [--max-iter N]: does the native port play RECORDING exactly like the
# original? RECORDING is any of
#   .polls  our sessions (play.sh), the Windows agent's synced TAS conversions
#   .bk2    BizHawk movie (its Genplus-gx core is our oracle: syncs frame for frame)
#   .gmv    Gens movie (played on the oracle; may drift from Gens, the run is still checked)
# Movies are first played on the original (the GPGX tracer) to record the inputs per
# controller poll; then sidebyside.py replays those polls on both and compares 68K RAM at
# every poll. Prints MATCH, or the first divergence with the command that localises it to
# the instruction (divergence.sh). TITLE: a games/ directory or a unique prefix; ROM: $ROM,
# else ../roms/<"rom" in sidebyside.json>.
set -eu
eng=${0:A:h}/..; root=$eng/..
title=${1:?usage: check.sh TITLE RECORDING [--max-iter N]}; rec=${2:?usage: check.sh TITLE RECORDING}; rec=${rec:A}; shift 2
iters=1; [[ ${1:-} == --max-iter ]] && iters=$2
titles=(${(f)"$(cd $root/games && for d in */; do [[ -f $d/sidebyside.json ]] && print ${d%/}; done)"})
if (( ! ${titles[(Ie)$title]} )); then
  m=(${(M)titles:#$title*})
  (( $#m == 1 )) || { print -u2 "unknown or ambiguous title '$title' (${m:-none})"; exit 2; }
  title=$m[1]
fi
game=$root/games/$title
romdef=$(python3 -c "import json;print(json.load(open('$game/sidebyside.json')).get('rom',''))")
rom=${ROM:-$root/../roms/$romdef}
[[ -f $rom ]] || { print -u2 "ROM not found: $rom (set ROM=path)"; exit 1; }
py=$eng/.venv/bin/python
cd $game
tr=build-trace/genrecomp_build/genrecomp_trace
[[ -x $tr ]] || { cmake -S . -B build-trace -G Ninja -DCMAKE_BUILD_TYPE=Release -DGENRECOMP_TRACE=ON >/dev/null; ninja -C build-trace genrecomp_trace >/dev/null; }
mkdir -p build/check
polls=$rec
case $rec in
  *.polls) ;;
  *.bk2|*.gmv)
    movie=$rec
    if [[ $rec == *.bk2 ]]; then movie=build/check/${rec:t:r}.txt; $py $eng/tools/bk2_to_movie.py $rec $movie >/dev/null; fi
    n=$(( $(wc -l < $movie) + 600 )); [[ $rec == *.gmv ]] && n=400000
    polls=build/check/${rec:t:r}.polls
    print "playing ${rec:t} on the original..."
    GENRECOMP_MOVIE=${movie:A} PS_POLL_RECORD=$polls $tr --headless --frames $n --trace-out build/check/seeds.txt $rom >/dev/null 2>&1 || true
    [[ -s $polls ]] || { print -u2 "the movie produced no controller polls"; exit 1; }
    ;;
  *) print -u2 "unknown recording type: $rec (.polls, .bk2, .gmv)"; exit 2;;
esac
print "$(wc -l < $polls | tr -d ' ') polls; comparing native port and original..."
out=$($py $eng/tools/sidebyside.py $rom . $polls --max-iter $iters 2>&1 | grep -E '^\[|STOP|MATCH' | tail -3)
print -- "$out"
if [[ $out == *"RAM diverges at poll"* ]]; then
  k=$(print -- "$out" | sed -nE 's/.*RAM diverges at poll ([0-9]+).*/\1/p' | tail -1)
  f=$(sed -n "$((k + 1))p" build/sidebyside/orig.ck | awk '{print $2}' | cut -c3-)
  print "localise: DIV_FROM=$((f - 1)) $eng/tools/divergence.sh $rom $game $polls $k"
  exit 1
fi
[[ $out == *MATCH* ]]
