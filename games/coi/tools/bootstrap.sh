#!/bin/zsh
# Seed bootstrap: run the ROM once under Genesis Plus GX's 68000 interpreter
# (genrecomp_trace, built with -DGENRECOMP_TRACE=ON), record every executed
# JSR/BSR/JMP target, interrupt entry and RTS landing PC, and merge them all
# into recomp.json seeds in one pass. Then regenerate + rebuild once; use
# tools/discover.sh only for whatever is left.
#
# Reusable for other genrecomp titles: run from the title's repo root.
# Usage: tools/bootstrap.sh ROM [frames]      (env PRESSES overrides the input script)
set -eu
cd "${0:A:h}/.."
rom=${1:?usage: tools/bootstrap.sh ROM [frames]}
frames=${2:-6000}
py=../genrecomp/.venv/bin/python
exe=${PWD:t}

# Scripted input: Start/A on a timer through boot + menus, then hold right and jump.
default_presses="400:START 700:START 900:START 1100:START 1300:START 1500:A 1700:START
1900:RIGHT:240 2150:C:12 2200:RIGHT:240 2450:C:12 2500:RIGHT:300 2820:C:12 2850:RIGHT:300
3200:C:12 3250:RIGHT:400 3700:C:12 3750:RIGHT:400 4200:C:12 4250:RIGHT:600 4900:C:12 4950:RIGHT:600"
presses=()
for p in ${=${PRESSES:-$default_presses}}; do presses+=(--press $p); done

print "== building tracer (build-trace/, GENRECOMP_TRACE=ON)"
cmake -S . -B build-trace -G Ninja -DCMAKE_BUILD_TYPE=Release -DGENRECOMP_TRACE=ON >/dev/null
ninja -C build-trace genrecomp_build/genrecomp_trace >/dev/null

print "== tracing $frames frames under the interpreter"
./build-trace/genrecomp_build/genrecomp_trace --headless --frames $frames $presses \
    --trace-out trace_seeds.txt "$rom" >/dev/null 2>&1 || true
print "   $(wc -l < trace_seeds.txt | tr -d ' ') executed entry points"

$py - <<'PY'
import json
cfg = json.load(open("recomp.json"))
seeds = cfg.setdefault("seeds", {})
added = 0
for line in open("trace_seeds.txt"):
    a = int(line.strip(), 16)
    key = f"0x{a:06X}"
    if key not in seeds:
        seeds[key] = f"sub_{a:06X}"
        added += 1
json.dump(cfg, open("recomp.json", "w"), indent=2)
print(f"   added {added} seeds, {len(seeds)} total")
PY

print "== regenerating + rebuilding"
$py ../genrecomp/tools/recompiler/generate.py "$rom" -o src/recomp -c recomp.json | grep -E "Generated|functions" | tail -2
cmake -S . -B build >/dev/null && ninja -C build >/dev/null

print "== native check run"
./build/$exe --headless --frames $frames $presses "$rom" > run.log 2>&1 || true
print "   dispatch misses: $(grep -c 'no function' run.log)  bad returns: $(grep -c 'RTS to' run.log)"
print "   leftovers: tools/discover.sh"
