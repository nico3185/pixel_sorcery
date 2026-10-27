#!/bin/zsh
# Discovery loop: run the recompiled game headless (fast) with scripted button
# presses, add every dispatch miss to recomp.json seeds, regenerate + rebuild,
# repeat until a run produces no new misses.
# Usage: tools/discover.sh [frames-per-run] [max-iterations]
set -u
cd "${0:A:h}/.."
frames=${1:-3000}; max=${2:-20}
py=../genrecomp/.venv/bin/python
rom=../roms/coi.md
presses=(--press 400:START --press 700:START --press 900:START --press 1100:START --press 1300:START
         --press 1500:RIGHT:200 --press 1750:C:10 --press 1800:RIGHT:200 --press 2050:C:10 --press 2100:RIGHT:300)
for i in $(seq 1 $max); do
  ./build/coi --headless --frames $frames $presses "$rom" > run.log 2>&1
  new=$($py - <<'PY'
import json, re
cfg = json.load(open("recomp.json"))
seeds = cfg.setdefault("seeds", {})
added = 0
for addr in sorted(set(re.findall(r"no function at \$([0-9A-F]{6}) ", open("run.log").read()))):
    a = int(addr, 16)
    if a & 1 or a < 0x200 or a >= 0x80000:   # odd / RAM / out of ROM: not a code entry
        continue
    key = f"0x{addr}"
    if key not in seeds:
        seeds[key] = f"sub_{addr}"
        added += 1
json.dump(cfg, open("recomp.json", "w"), indent=2)
print(added)
PY
)
  print "iteration $i: new_seeds=$new total=$($py -c 'import json;print(len(json.load(open("recomp.json"))["seeds"]))') misses=$(grep -c 'no function' run.log)"
  [[ $new == 0 ]] && break
  $py ../genrecomp/tools/recompiler/generate.py "$rom" -o src/recomp -c recomp.json >/dev/null 2>&1
  cmake -S . -B build >/dev/null 2>&1 && ninja -C build >/dev/null 2>&1 || { print "build failed"; exit 1; }
done
