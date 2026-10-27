#!/usr/bin/env python3
"""Merge tracer coverage files (one 0xADDRESS per line) into a title's recomp.json seeds.
Usage: merge_seeds.py path/to/recomp.json coverage.seeds [...]"""
import json, re, sys, pathlib
cfg_path = pathlib.Path(sys.argv[1])
cfg = json.loads(cfg_path.read_text())
seeds = cfg.setdefault("seeds", {})
added = 0
for f in sys.argv[2:]:
    for line in open(f):
        m = re.match(r"\s*0x([0-9A-Fa-f]+)", line)
        if not m:
            continue
        a = int(m.group(1), 16)
        if a & 1 or a < 0x200 or a >= 0x400000:   # 4 MB: the largest unmapped cartridge
            continue
        key = f"0x{a:06X}"
        if key not in seeds:
            seeds[key] = f"sub_{a:06X}"
            added += 1
if added:                                     # untouched otherwise: sidebyside.py compares mtimes
    cfg_path.write_text(json.dumps(cfg, indent=2) + "\n")
print(f"added {added} seeds, total {len(seeds)}")
