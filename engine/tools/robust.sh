#!/bin/zsh
# robust.sh [TITLE...] [--mixed PCT] [--seeds N] — the no-play robustness harness: every recording
# of each title (default: all titles) replayed against the original, the compiled build and then
# a mixed build (a share of functions interpreted, several seeds), with entry points added on the
# way; one line per title in robust_report.md, details in games/TITLE/build/robust.json.
#
# Recordings: build/selfcheck.polls, the player's sessions (games/TITLE/sessions and
# Application Support/PixelSorcery/TITLE/sessions), and games/TITLE/recordings/*.polls (long
# plays, TAS conversions - local only). Sessions are compared with logic settings off (no
# .meta): cheats and widescreen enemies change the game on purpose.
# Steps per title: check_seeds.py (seeds inside instructions), sidebyside.py on every recording
# (adds missing entry points, regenerates, until MATCH or a real divergence), the same with
# PS_INTERP=random:PCT:SEED (the compiled/interpreted hand-over), then the report line.
# Reading the report is the only manual part: a STOP names the recording and its first
# divergent poll; divergence.sh localises it to an instruction.
set -u
eng=${0:A:h}/..; root=$eng/..; py=$eng/.venv/bin/python
pct=30; seeds=2; titles=()
while (( $# )); do
  case $1 in
    --mixed) pct=$2; shift ;;
    --seeds) seeds=$2; shift ;;
    *) titles+=($1) ;;
  esac
  shift
done
(( $#titles )) || titles=(${(f)"$(cd $root/games && for d in */; do [[ -f $d/sidebyside.json ]] && print ${d%/}; done)"})
report=${ROBUST_REPORT:-$root/robust_report.md}     # ROBUST_REPORT=file: one per parallel run
tmp=$(mktemp -d)
print "# Robustness report $(date '+%Y-%m-%d %H:%M')  (mixed: ${pct}% interpreted, $seeds seeds)\n" > $report
print "| title | recordings | compiled MATCH | mixed MATCH | entry points added | open divergences |" >> $report
print "|---|---|---|---|---|---|" >> $report
for t in $titles; do
  g=$root/games/$t
  [[ -f $g/sidebyside.json ]] || { print -u2 "$t: no sidebyside.json"; continue; }
  romname=$(python3 -c "import json;print(json.load(open('$g/sidebyside.json')).get('rom',''))")
  rom=$root/../roms/$romname
  [[ -f $rom ]] || continue                          # no ROM on this machine: not a result
  # the recordings, copied without their .meta (logic settings off)
  recs=(); i=0
  for p in $g/build/selfcheck.polls(N) $g/sessions/*.polls(N) "$HOME/Library/Application Support/PixelSorcery/$t/sessions/"*.polls(N) $g/recordings/*.polls(N); do
    (( $(wc -l < "$p") > 120 )) || continue
    i=$((i + 1)); cp "$p" $tmp/$t-$i.polls; recs+=($tmp/$t-$i.polls)
    print "$t-$i ${p:t}" >> $tmp/$t.names
  done
  before=$(python3 -c "import json;print(len(json.load(open('$g/recomp.json'))['seeds']))")
  $py -I $eng/tools/recompiler/check_seeds.py $rom $g/recomp.json --fix >/dev/null 2>&1
  okc=0; okm=0; open=(); results=()
  for r in $recs; do
    name=$(grep "^${r:t:r} " $tmp/$t.names | cut -d' ' -f2)
    c=$($py $eng/tools/sidebyside.py $rom $g $r --max-iter 6 2>&1 | grep -E "^MATCH|^STOP" | tail -1)
    [[ $c == MATCH* ]] && okc=$((okc + 1)) || open+=("$name compiled: ${c:-no result}")
    m="MATCH"
    for s in $(seq 1 $seeds); do
      x=$(PS_INTERP=random:$pct:$s $py $eng/tools/sidebyside.py $rom $g $r --max-iter 1 2>&1 | grep -E "^MATCH|^STOP" | tail -1)
      [[ $x == MATCH* ]] || { m="seed $s: ${x:-no result}"; break; }
    done
    [[ $m == MATCH ]] && okm=$((okm + 1)) || open+=("$name mixed $m")
    results+=("{\"recording\": \"$name\", \"compiled\": \"${c//\"/}\", \"mixed\": \"${m//\"/}\"}")
  done
  after=$(python3 -c "import json;print(len(json.load(open('$g/recomp.json'))['seeds']))")
  print "{\"title\": \"$t\", \"recordings\": [${(j:, :)results}], \"entry_points_added\": $((after - before))}" > $g/build/robust.json
  print "| $t | $#recs | $okc/$#recs | $okm/$#recs | $((after - before)) | ${(j:<br>:)open:-none} |" >> $report
  print "$t: compiled $okc/$#recs, mixed $okm/$#recs, +$((after - before)) entry points"
done
rm -rf $tmp
print "report: $report"
