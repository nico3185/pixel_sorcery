#!/usr/bin/env python3
"""funclut.py [TITLE ...]: the known-function lookup table (BACKLOG roadmap 0.6).
docs/lut/functions.jsonl (in git: one function per line, sorted, so an update changes only its lines)
and docs/lut/functions.db (SQLite, git-ignored, rebuilt from the .jsonl: functions(fp, insns, name),
placements(fp, title, entry, verified) - what the recompiler and --lookup query).

One entry per distinct function of at least MIN instructions across the titles' generated code
(games/T/src/recomp, read as kpi.py reads it): its fingerprint - SHA-1 of the first word of each of
its instructions in address order, so the same routine relocated elsewhere matches while operands
(addresses, constants) don't count - its instruction count, and per title the entry address and
whether a verified replay ran it (games/T/build/cover, kpi.py's executed()). A role name can be
added by hand ("name"); the generator never reads names from other projects' disassemblies.
No game bytes are stored: fingerprints, sizes and addresses only, like roms.json's hashes.

--lookup TITLE: for each function of TITLE, the other titles that have the same fingerprint (what
a new title's recompilation would get from the table: entry points, boundaries, names).
"""
import hashlib
import json
import os
import sqlite3
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kpi   # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LUT = os.path.join(REPO, 'docs', 'lut')
JSONL = os.path.join(LUT, 'functions.jsonl')
DB = os.path.join(LUT, 'functions.db')
MIN = 8


def scan(title, roms):
    g = os.path.join(REPO, 'games', title)
    cfg = json.load(open(os.path.join(g, 'sidebyside.json')))
    rom = open(os.path.join(roms, cfg['rom']), 'rb').read()
    bodies = {}
    kpi.generated(g, len(rom), bodies)
    ex, _ = kpi.executed(g, len(rom))
    out = {}
    for entry, body in bodies.items():
        if len(body) < MIN:
            continue
        ops = b''.join(rom[a:a + 2] for a in sorted(body))
        out[hashlib.sha1(ops).hexdigest()] = (entry, len(body), bool(body & ex))
    return out


def load():
    funcs = {}
    if os.path.exists(JSONL):
        for line in open(JSONL):
            f = json.loads(line)
            funcs[f.pop('fp')] = f
    return funcs


def build_db(funcs):
    if os.path.exists(DB):
        os.remove(DB)
    db = sqlite3.connect(DB)
    db.executescript("""create table functions(fp text primary key, insns int, name text);
                        create table placements(fp text, title text, entry text, verified int);
                        create index placements_title on placements(title);""")
    db.executemany('insert into functions values (?, ?, ?)', [(h, f['insns'], f.get('name')) for h, f in funcs.items()])
    db.executemany('insert into placements values (?, ?, ?, ?)',
                   [(h, t, p['entry'], int(p['verified'])) for h, f in funcs.items() for t, p in f['titles'].items()])
    db.commit()
    return db


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    roms = os.environ.get('PS_ROMS', os.path.join(os.path.dirname(REPO), 'roms'))
    funcs = load()
    if '--lookup' in sys.argv:
        t = args[0]
        mine = scan(t, roms)
        db = sqlite3.connect(DB) if os.path.exists(DB) else build_db(funcs)
        known = {h for (h,) in db.execute('select distinct fp from placements where title != ?', (t,))}
        print(f'{t}: {len(set(mine) & known)} of {len(mine)} functions known from other titles')
        return 0
    titles = args or kpi.TITLES
    for t in titles:
        for h, (entry, n, ran) in scan(t, roms).items():
            f = funcs.setdefault(h, {'insns': n, 'titles': {}})
            f['titles'][t] = {'entry': '$%06X' % entry, 'verified': ran}
    os.makedirs(LUT, exist_ok=True)
    with open(JSONL, 'w') as fh:
        for h in sorted(funcs):
            fh.write(json.dumps({'fp': h, **funcs[h]}, separators=(',', ':'), sort_keys=True) + '\n')
    build_db(funcs).close()
    shared = sum(1 for f in funcs.values() if len(f['titles']) > 1)
    print(f'{len(funcs)} functions, {shared} in more than one title -> {os.path.relpath(JSONL, REPO)} + functions.db')
    return 0


if __name__ == '__main__':
    sys.exit(main())
