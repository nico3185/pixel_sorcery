#!/bin/zsh
# cover_loop.sh TITLE [MINUTES] — raise a title's instruction coverage (kpi.py KPI A) with no
# person and no LLM in the loop. Each round:
#   1. explore: COVER_JOBS Go-Explore bots (genrecomp_explore --cover-base) on the ORIGINAL (GPGX,
#      headless, no picture), each for COVER_ROUND seconds, from power-on or from a cell every few
#      hundred frames along a recording (the player's sessions, games/T/recordings/*.polls, and the
#      bot recordings of earlier rounds: depth compounds). Novelty = a 68000 instruction no
#      recording ever executed (the union of games/T/build/cover/*.bin); plus camera / zone cells
#      for diversity. Each bot writes the routes (per poll, from power-on) reaching the most new code.
#   2. keep: every route is replayed on the GPGX tracer (PS_PCCOVER): one that runs instructions
#      no recording ran is kept as games/T/recordings/bot-*.polls (git-ignored) and its bitmap
#      joins build/cover/ (KPI A counts it at once: it is what the original executed).
#   3. verify, every COVER_SBS_EVERY kept recordings and at the end: the new functions they reach
#      are merged into recomp.json (one regeneration), then sidebyside.py replays each on the
#      compiled build and a mixed one (PS_INTERP=random:30:1). MATCH -> robust.json (KPI C);
#      a STOP is printed as "FINDING" (and build/coverloop/findings.txt) for a person to look at.
#   4. one line per round: KPI A, new instructions, recordings kept, findings.
# At the end kpi.py TITLE (appends docs/kpi/history.csv). MINUTES (default 30) bounds the
# exploration; the last verification follows it.
# Env: COVER_JOBS (2) COVER_ROUND (300 s) COVER_SBS_EVERY (8) COVER_ROUTES (12 per bot)
#      COVER_NO_VERIFY=1 (skip 3)  COVER_HEAVY (a slot runner, e.g. heavy.sh; default none)
#      PS_ROMS (../roms)
set -u
eng=${0:A:h}/..; root=$eng/..; py=$eng/.venv/bin/python; [[ -x $py ]] || py=python3
t=${1:?usage: cover_loop.sh TITLE [MINUTES]}; minutes=${2:-30}
g=$root/games/$t
[[ -f $g/sidebyside.json ]] || { print -u2 "$t: no games/$t/sidebyside.json"; exit 2; }
roms=${PS_ROMS:-$root/../roms}
rom=$roms/$(python3 -c "import json;print(json.load(open('$g/sidebyside.json'))['rom'])")
[[ -f $rom ]] || { print -u2 "$t: ROM $rom missing"; exit 2; }
jobs_n=${COVER_JOBS:-2}; round_s=${COVER_ROUND:-300}; every=${COVER_SBS_EVERY:-8}; nroutes=${COVER_ROUTES:-12}
H=(${=COVER_HEAVY:-})
tracer=$g/build-trace/genrecomp_build/genrecomp_trace
explore=$eng/build-cover/genrecomp_explore
work=$g/build/coverloop; mkdir -p $work $g/recordings $g/build/cover; touch $work/pending $work/findings.txt
romsize=$(stat -L -f %z $rom 2>/dev/null || stat -L -c %s $rom)   # -L: ROMs are often symlinks

# the bot (an explore build with the CPU hook; engine/build-cover is git-ignored)
if [[ ! -x $explore || $eng/tools/explore/explore.c -nt $explore ]]; then
  ( cmake -S $eng -B $eng/build-cover -G Ninja -DCMAKE_BUILD_TYPE=Release -DGENRECOMP_TRACE=ON >/dev/null &&
    $H cmake --build $eng/build-cover --target genrecomp_explore >/dev/null ) || { print -u2 "explore build failed"; exit 4; }
fi
[[ -x $tracer ]] || { print -u2 "$t: no tracer $tracer (recomp.sh builds it)"; exit 4; }

# diversity cells per title (RAM, hex): zone/act, game state, level; the camera for all
case $t in
  sh1|sh2|sh3|sh3k|shk|shk2) cells=(--cell FE10:2:0) ;;
  qs|qs-r1) cells=(--cell FF18:2:0) ;;
  coi) cells=(--cell FF42:1:0) ;;
  *) cells=() ;;
esac

kpi_a() {   # KPI A from a fresh kpi.json (dynamic part only)
  $H $py $eng/tools/kpi.py $t --no-history --no-static >/dev/null 2>&1
  python3 -c "import json;k=json.load(open('$g/build/kpi.json'));print('%.2f %d %d' % (k['coverage_pct'], k['executed_converted'], k['interpreted_only']))"
}

verify() {   # sidebyside on the pending recordings; STOP -> FINDING
  local pend=($(cat $work/pending 2>/dev/null)); (( $#pend )) || return 0
  [[ -n ${COVER_NO_VERIFY:-} ]] && return 0
  local s=(); for n in $pend; do [[ -f $work/seeds/$n.seeds ]] && s+=($work/seeds/$n.seeds); done
  (( $#s )) && $py $eng/tools/merge_seeds.py $g/recomp.json $s | sed "s/^/  $t: /"
  local res=$work/results-$(date +%Y%m%d-%H%M%S).txt
  for n in $pend; do
    local p=$g/recordings/$n.polls
    local c=$($H $py $eng/tools/sidebyside.py $rom $g $p --max-iter 4 2>&1 | grep -E "^MATCH|^STOP" | tail -1)
    local m=$(PS_INTERP=random:30:1 $H $py $eng/tools/sidebyside.py $rom $g $p --max-iter 1 2>&1 | grep -E "^MATCH|^STOP" | tail -1)
    print "$n | c: ${c:-no result} | m1: ${m:-no result}" >> $res
    [[ $c == MATCH* ]] || { print "FINDING $t $n compiled: ${c:-no result}" | tee -a $work/findings.txt; }
    [[ $m == MATCH* ]] || { print "FINDING $t $n mixed (PS_INTERP=random:30:1): ${m:-no result}" | tee -a $work/findings.txt; }
  done
  $py $eng/tools/kpi.py --results $t=$res >/dev/null
  : > $work/pending
}

end=$(( $(date +%s) + minutes * 60 ))
read a0 e0 b0 <<< "$(kpi_a)"
print "$t: start KPI A $a0% ($e0 executed converted, B $b0), ${minutes} min, $jobs_n bots x ${round_s}s rounds"
round=0; kept_total=0; rr=$(cat $work/rr 2>/dev/null || print 0)
while (( $(date +%s) < end )); do
  round=$((round + 1))
  left=$(( end - $(date +%s) )); secs=$(( left < round_s ? left : round_s )); (( secs < 20 )) && break
  $py $eng/tools/cover_util.py union $work/base.bin $g/build/cover/*.bin(N)
  srcs=("" ${(f)"$($py $eng/tools/cover_util.py routes $t)"})
  pids=()
  for j in $(seq 1 $jobs_n); do
    src=${srcs[$(( rr % $#srcs + 1 ))]}; rr=$((rr + 1))
    d=$work/r$round-$j; rm -rf $d; mkdir -p $d
    a=(--dir $d --cover-base $work/base.bin --seconds $secs --routes $nroutes --fast --buttons ABC --camera 6
       --seed $RANDOM $cells)
    if [[ -n $src ]]; then
      np=$(wc -l < $src); ae=$(( np / 100 )); (( ae < 300 )) && ae=300
      a+=(--polls $src --anchor-every $ae)
    fi
    print -r -- "${src:-power-on}" > $d/source
    $H $explore $rom $a > $d/explore.log 2>&1 &
    pids+=($!)
  done
  print $rr > $work/rr
  wait $pids
  # keep the routes that run instructions nobody ran (replayed on the tracer: exact by construction)
  kept=0; gained=0
  for r in $work/r$round-*/cover_*.polls(N); do
    n=$(wc -l < $r); (( n > 0 )) || continue
    rm -f $work/r.bin
    PS_POLL_REPLAY=$r PS_POLL_STOP=$((n - 1)) PS_PCCOVER=$work/r.bin $H $tracer --headless --frames $((n * 4 + 2000)) \
      --trace-out $work/r.seeds $rom >/dev/null 2>&1
    gn=$($py $eng/tools/cover_util.py gain $work/r.bin $work/base.bin $romsize)
    (( gn > 0 )) || continue
    name=bot-$(date +%Y%m%d-%H%M%S)-${${r:h}:t}-${${r:t:r}#cover_}
    cp $r $g/recordings/$name.polls
    cp $work/r.bin $g/build/cover/$name.bin
    mkdir -p $work/seeds; cp $work/r.seeds $work/seeds/$name.seeds
    print $name >> $work/pending
    $py $eng/tools/cover_util.py union $work/base.bin $work/base.bin $work/r.bin
    kept=$((kept + 1)); gained=$((gained + gn))
  done
  kept_total=$((kept_total + kept))
  (( $(wc -l < $work/pending 2>/dev/null || print 0) >= every )) && verify
  read a e b <<< "$(kpi_a)"
  nf=$(wc -l < $work/findings.txt 2>/dev/null || print 0)
  print "$t round $round: KPI A $a% (+$(( e - e0 )) converted executed), $gained new instructions, kept $kept recordings, B $b, findings ${nf// /}"
done
verify
$H $py $eng/tools/kpi.py $t >/dev/null 2>&1
read a e b <<< "$(python3 -c "import json;k=json.load(open('$g/build/kpi.json'));print('%.2f %d %d' % (k['coverage_pct'], k['executed_converted'], k['interpreted_only']))")"
print "$t: KPI A $a0% -> $a% ($e0 -> $e executed converted), B $b0 -> $b, $kept_total bot recordings kept, findings: $(cat $work/findings.txt 2>/dev/null | wc -l | tr -d ' ') (build/coverloop/findings.txt)"
