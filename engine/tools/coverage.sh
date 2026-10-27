#!/bin/zsh
# coverage.sh ROM GAMEDIR — close a title's code coverage from everything known about it:
#   1. static: dispatcher tables (jumptables.py, dispatchers found automatically) and
#      seeds starting inside another instruction dropped (check_seeds.py)
#   2. play: every recorded session (GAMEDIR/sessions and the app's sessions folder) is
#      replayed side by side with the original (sidebyside.py): functions the original
#      ran are seeded, the build is regenerated, until each session MATCHes
#   3. bots: GAMEDIR/tools/coverage_sweep.sh when the title has one (all stages)
# Ends with recomp.sh's self-check. Output: GAMEDIR/build/coverage.txt
set -u
eng=${0:A:h}/..; rom=${1:A}; game=${2:A}; title=${game:t}
py=$eng/.venv/bin/python
cd $game; out=build/coverage.txt; mkdir -p build; : > $out
$py -I $eng/tools/recompiler/jumptables.py $rom recomp.json 2>&1 | tee -a $out
$py -I $eng/tools/recompiler/check_seeds.py $rom recomp.json --fix | tee -a $out
for d in sessions "$HOME/Library/Application Support/PixelSorcery/$title/sessions"; do
  [[ -d $d ]] || continue
  for p in $d/*.polls(N); do
    r=$($py $eng/tools/sidebyside.py $rom . $p --max-iter 10 2>&1 | grep -E "MATCH|STOP|comparable" | tail -1)
    print "${p:t:r}: $r" | tee -a $out
  done
done
[[ -x tools/coverage_sweep.sh ]] && tools/coverage_sweep.sh $rom | tee -a $out
$eng/tools/recomp.sh $rom $game 2>&1 | tail -1 | tee -a $out
