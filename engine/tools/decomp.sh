#!/bin/zsh
# decomp.sh ROM GAMEDIR [--movie FILE] [--offset N] [--padtype 6] [--frames N]
# Find the game's code: trace it on the Genesis Plus GX interpreter (driven by an input
# movie, or by GAMEDIR/selfcheck.args), merge every executed entry point into
# GAMEDIR/recomp.json, and write a labelled disassembly + function map to GAMEDIR/disasm/
# (ROM-derived: git-ignored).
set -eu
eng=${0:A:h}/..; rom=${1:A}; game=${2:A}; shift 2
movie=""; offset=0; pad=3; frames=""
while (( $# )); do case $1 in
  --movie) movie=${2:A}; shift 2;; --offset) offset=$2; shift 2;;
  --padtype) pad=$2; shift 2;; --frames) frames=$2; shift 2;;
  *) print -u2 "unknown option $1"; exit 2;; esac; done
py=$eng/.venv/bin/python
[[ -x $py ]] || { python3 -m venv $eng/.venv; $eng/.venv/bin/pip -q install capstone; }
cd $game; mkdir -p disasm
if [[ ! -x build-trace/genrecomp_build/genrecomp_trace ]]; then
  cmake -S . -B build-trace -G Ninja -DCMAKE_BUILD_TYPE=Release -DGENRECOMP_TRACE=ON >/dev/null
  ninja -C build-trace genrecomp_trace >/dev/null
fi
args=(--headless --trace-out disasm/coverage.seeds)
if [[ -n $movie ]]; then
  export GENRECOMP_MOVIE=$movie GENRECOMP_MOVIE_OFFSET=$offset
  [[ -n $frames ]] || frames=$(( $(wc -l < $movie 2>/dev/null || echo 0) + 0 ))
  [[ $frames -gt 0 ]] || frames=200000
else
  [[ -f selfcheck.args ]] && args+=(${(z)"$(<selfcheck.args)"})
  [[ -n $frames ]] || frames=6000
fi
GENRECOMP_PADTYPE=$pad build-trace/genrecomp_build/genrecomp_trace $args --frames $frames $rom >disasm/trace.log 2>&1 || true
$py $eng/tools/merge_seeds.py recomp.json disasm/coverage.seeds
$py $eng/tools/recompiler/analyze.py $rom -c recomp.json -o disasm/functions.json --stats >disasm/stats.txt
$py $eng/tools/recompiler/analyze.py $rom -c recomp.json --disasm >disasm/listing.asm
print "coverage: $(wc -l < disasm/coverage.seeds) entry points; listing: disasm/listing.asm"
grep -iE "functions|instructions|coverage" disasm/stats.txt | head -5
