#!/bin/zsh
# divergence.sh ROM GAMEDIR POLLS POLL — localise the first difference before a divergent poll:
# per-frame clocks, then an instruction-level trace (pc, cycles, registers) of the first
# differing frame (or of the last poll interval), both sides. Needs build-itrace and build-trace.
set -u
eng=${0:A:h}/..; rom=${1:A}; game=${2:A}; polls=${3:A}; k=$4
cd $game; S=build/sidebyside; mkdir -p $S
tr=build-trace/genrecomp_build/genrecomp_trace
exe=$(python3 -c "import json;print(json.load(open('sidebyside.json')).get('exe','build/coi'))" 2>/dev/null || echo build/coi)
iexe=build-itrace/${exe:t}
# the session's codes on the original, as sidebyside.py applies them (sidebyside.json "oracle")
meta=${polls:r}.meta
if [[ -f $meta ]]; then
  eval "$(python3 - $meta <<'PY'
import sys, json
ev = [list(map(int, (l.split() + ['0'])[2:6])) for l in open(sys.argv[1]) if l.startswith('logic ')]
oc = json.load(open('sidebyside.json')).get('oracle', {}) if __import__('os').path.exists('sidebyside.json') else {}
if any(e[1] for e in ev) and oc.get('cheats'): print("export PS_ORACLE_PATCH='%s'" % ','.join(oc['cheats']))
if any(e[2] for e in ev) and oc.get('toggles'): print("export PS_ORACLE_TOGGLES='%s'" % ','.join(oc['toggles']))
PY
)"
fi
PS_FRAMELOG=$S/r.fl PS_POLL_REPLAY=$polls PS_POLL_STOP=$k ./$exe --headless --frames 400000 $rom >/dev/null 2>&1 &
PS_FRAMELOG=$S/o.fl PS_POLL_REPLAY=$polls PS_POLL_STOP=$k $tr --headless --frames 400000 --trace-out /dev/null $rom >/dev/null 2>&1; wait
read a b <<< $(python3 - $S $k <<'PY'
import sys
S,k=sys.argv[1],int(sys.argv[2])
r={l.split()[0]:l.split()[1] for l in open(f'{S}/r.fl')}; o={l.split()[0]:l.split()[1] for l in open(f'{S}/o.fl')}
d=[int(x) for x in sorted(set(r)&set(o), key=int) if r[x]!=o[x] and int(x)>int(__import__("os").environ.get("DIV_FROM","500"))]
ck=[l.split()[1] for l in open(f'{S}/orig.ck')]
lo=int(ck[k-1][2:]); hi=int(ck[k][2:])
print(*( (d[0]-1, d[0]) if d and d[0] <= hi else (lo, hi) ))
PY
)
print "frames $a..$b"
PS_POLL_REPLAY=$polls PS_POLL_STOP=$k PS_ITRACE=$S/r.it PS_FTRACE_FROM=$a PS_FTRACE_TO=$b ./$iexe --headless --frames 400000 $rom >/dev/null 2>&1 &
PS_POLL_REPLAY=$polls PS_POLL_STOP=$k PS_ITRACE=$S/o.it PS_FTRACE=$S/o.ft PS_FTRACE_FUNCS=$S/r.ft.funcs PS_FTRACE_FROM=$a PS_FTRACE_TO=$b $tr --headless --frames 400000 --trace-out /dev/null $rom >/dev/null 2>&1; wait
python3 $eng/tools/tracecmp.py $S
