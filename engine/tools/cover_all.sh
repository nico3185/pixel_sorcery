#!/bin/zsh
# cover_all.sh [MINUTES] [TITLE...] — raise every focus title's instruction coverage, no person and
# no LLM in the loop: per title (in parallel, CPU through heavy.sh)
#   1. level starts: one recording per act through the game's own level select
#      (level_starts.py, games/T/level_select.json; titles without a recipe skip it);
#   2. each start replayed by sidebyside.py, compiled and mixed (PS_INTERP=random:30:1): a start
#      that does not MATCH is reported (FINDING) and its coverage bitmap set aside, so KPI A only
#      counts verified play; new entry points are merged into recomp.json on the way;
#   3. cover_loop.sh T MINUTES: the novelty explorer from every recording (starts included);
#   4. kpi.py, then cover_table.py: before/after per title.
# Default 30 minutes per title; titles default to cover_table.py's focus list.
set -u
eng=${0:A:h}/..; root=$eng/..; py=$eng/.venv/bin/python
minutes=${1:-30}; (( $# )) && shift
titles=($@); (( $#titles )) || titles=(sh1 sh2 sh3 shk sh3k shk2 coi qs woi-us)
H=$eng/tools/heavy.sh
log=$root/cover_all_report.md
print "# cover_all $(date '+%Y-%m-%d %H:%M'), $minutes min per title\n" > $log
one() {
  local t=$1 g=$root/games/$1
  local rom=$root/../roms/$(python3 -c "import json;print(json.load(open('$g/sidebyside.json'))['rom'])")
  if [[ -f $g/level_select.json ]]; then
    $H $py $eng/tools/level_starts.py $t | sed 's/^/  /'
    for p in $g/recordings/start-*.polls(N); do
      local n=${p:t:r}
      local c=$($H $py $eng/tools/sidebyside.py $rom $g $p --max-iter 4 2>&1 | grep -E "^MATCH|^STOP" | tail -1)
      local m=$(PS_INTERP=random:30:1 $H $py $eng/tools/sidebyside.py $rom $g $p --max-iter 1 2>&1 | grep -E "^MATCH|^STOP" | tail -1)
      print "$n | c: ${c:-no result} | m1: ${m:-no result}" >> $g/build/starts/results.txt
      if [[ $c != MATCH* || $m != MATCH* ]]; then
        print "FINDING $t $n: compiled ${c:-none} / mixed ${m:-none}"
        mkdir -p $g/build/cover-unverified; mv -f $g/build/cover/$n.bin $g/build/cover-unverified/ 2>/dev/null
      fi
    done
    $py $eng/tools/kpi.py --results $t=$g/build/starts/results.txt >/dev/null
  fi
  COVER_HEAVY=$H $eng/tools/cover_loop.sh $t $minutes | tail -1
}
for t in $titles; do one $t >> $log 2>&1 & done
wait
$py $eng/tools/kpi.py $titles >/dev/null 2>&1
$py $eng/tools/cover_table.py $titles >> $log
cat $log
