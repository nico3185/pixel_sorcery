#!/bin/zsh
# nightly.sh [--commit] [TITLE...] — the no-LLM robustness CI: every recording of every title (default:
# all titles with a sidebyside.json) replayed against the original, compiled and mixed
# (robust.sh: PS_INTERP=random:30, 2 seeds; entry points added on the way), titles in parallel
# through heavy.sh, then kpi.py (coverage KPIs, a row per title in docs/kpi/history.csv).
# Reports: docs/kpi/nightly.md (one robust.sh table per title + the KPI table).
# --commit: commit docs/kpi (numbers only), never seeds - new seeds need a recomp.sh self-check.
# Schedule it with launchd or cron, e.g. 0 2 * * * cd ~/dev/recompilation/PixelSorcery && engine/tools/nightly.sh
set -u
root=${0:A:h:h:h}; tools=$root/engine/tools; commit=0; titles=()
for a in "$@"; do [[ $a == --commit ]] && commit=1 || titles+=($a); done
(( $#titles )) || titles=(${(f)"$(cd $root/games && for d in */; do [[ -f $d/sidebyside.json && -f $d/recomp.json ]] && print ${d%/}; done)"})
out=$(mktemp -d); cd $root
for t in $titles; do
  ( ROBUST_REPORT=$out/$t.md $tools/heavy.sh $tools/robust.sh $t --mixed 30 --seeds 2 > $out/$t.log 2>&1 ) &
done
wait
report=$root/docs/kpi/nightly.md; mkdir -p ${report:h}
{ print "# Nightly robustness $(date '+%Y-%m-%d %H:%M') ($(git rev-parse --short HEAD))\n"
  for t in $titles; do [[ -f $out/$t.md ]] && sed -n '3,$p' $out/$t.md | grep "^| $t " ; done | sed '1i\
| title | recordings | compiled MATCH | mixed MATCH | entry points added | open divergences |\
|---|---|---|---|---|---|'
  print "\n## Coverage KPI\n\n\`\`\`"
  $root/engine/.venv/bin/python -I $tools/kpi.py 2>&1 | tail -20
  print "\`\`\`" } > $report
print "report: $report"
if (( commit )); then
  git add docs/kpi && git commit -q -m "Nightly robustness and coverage KPI $(date +%Y-%m-%d)" && print "committed docs/kpi"
fi
rm -rf $out
