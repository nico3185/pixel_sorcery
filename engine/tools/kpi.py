#!/usr/bin/env python3
"""kpi.py [TITLE ...]: robustness / coverage KPIs per title, from what is already on disk.

Dynamic (what the original executed, recorded by the GPGX tracer with PS_PCCOVER - sidebyside.py
does it on every replay - into games/T/build/cover/*.bin, one bit per even address < $400000):
  A  coverage         = executed instructions we converted / instructions we converted
  B  interpreted-only = instructions the original executed in ROM that we never converted (target 0)
  never entered       = generated functions no recording ever entered (the to-do list for bots/fuzzing)
Replay exactness (games/T/build/robust.json, see --results):
  C  recordings whose replay MATCHes the original: compiled build / every mixed seed; polls verified
Static (the ROM and the generated C alone):
  D  converted code bytes and their share of the ROM (instruction sizes from capstone)
  E  suspect gaps: unconverted stretches >= 64 bytes decoding as >= 16 valid instructions ending in
     rts/rte/jmp/bra - an upper bound on code never found (68000 decodes much data as code). Strong
     when an address inside is referenced by converted code (absolute, PC-relative, branch/call
     target, immediate) or by a pointer-sized ROM longword elsewhere (pointer/jump tables); else weak.
  F  reference coverage: routine addresses another project's disassembly labels as code
     (docs/kpi/references.json, read in place) that our generated C covers. MEASUREMENT ONLY - never
     an input to recomp.json. A reference built for another ROM revision is reported, not compared.

Writes games/T/build/kpi.json (address lists, git-ignored) and appends one line per title to
docs/kpi/history.csv (numbers only). --no-history: table only.
--results TITLE=FILE[,FILE...]: import sidebyside result lines ("NAME compiled: MATCH: ... at all N
polls", "NAME | c: ... | m1: ...", "NAME N compiled=[...] s1=MATCH(N)") into games/T/build/robust.json
(later lines win), then exit.
"""
import argparse
import bisect
import csv
import datetime
import glob
import json
import os
import re
import struct
import subprocess
import sys

ENG = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPO = os.path.dirname(ENG)
TITLES = ['sh1', 'sh2', 'sh3', 'sh3k', 'shk', 'shk2', 'coi', 'qs',
          'qs-r1', 'woi', 'woi-us', 'shk1']
HIST = os.path.join(REPO, 'docs', 'kpi', 'history.csv')
REFS = os.path.join(REPO, 'docs', 'kpi', 'references.json')
COLS = ['date', 'git_sha', 'title', 'converted', 'executed_converted', 'coverage_pct', 'interpreted_only',
        'recordings', 'match_compiled', 'match_mixed', 'polls_verified', 'functions', 'functions_never_entered',
        'code_bytes', 'code_pct_rom', 'suspects_strong', 'suspects_weak', 'ref_routines', 'ref_covered', 'ref_pct']


# ---------------------------------------------------------------- generated C, coverage bitmaps

def generated(game, rom_size):
    insns, entries = set(), set()
    for f in glob.glob(os.path.join(game, 'src', 'recomp', 'recomp_*.c')):
        s = open(f).read()
        insns.update(int(x, 16) for x in re.findall(r'ITRACE\(0x([0-9A-F]+)\)', s))
        entries.update(int(x, 16) for x in re.findall(r'^void \w+_([0-9A-F]{6})\(void\) \{', s, re.M))
    return {a for a in insns if a < rom_size}, {a for a in entries if a < rom_size}


def executed(game, rom_size):
    """Union of build/cover/*.bin: bit (a >> 1) & 7 of byte a >> 4 = PC a executed."""
    acc = bytearray(0x400000 // 16)
    files = sorted(glob.glob(os.path.join(game, 'build', 'cover', '*.bin')))
    for f in files:
        b = open(f, 'rb').read()
        acc = bytearray(x | y for x, y in zip(acc, b.ljust(len(acc), b'\0')))
    ex = set()
    for i, v in enumerate(acc):
        if v:
            for k in range(8):
                if v >> k & 1:
                    a = (i << 4) | (k << 1)
                    if a < rom_size:
                        ex.add(a)
    return ex, [os.path.splitext(os.path.basename(f))[0] for f in files]


# ---------------------------------------------------------------- replay results (KPI C)

MODE = r'(compiled|mixed\d|seed\d|s\d|c|m\d)'


def parse_results(path):
    """{recording: {"polls": N, "compiled": "MATCH"|"STOP ...", "mixed": {mode: result}}}"""
    out = {}
    for line in open(path, errors='replace'):
        segs = list(re.finditer(r'(?:^|\s|\||\])' + MODE + r'\s*(?::|=)\s*\[?', line))
        if not segs:
            continue
        head = line[:segs[0].start()].replace('|', ' ').split()
        head = [h for h in head if h not in TITLES and not h.isdigit()]
        if not head:
            continue
        name = norm_name(head[-1])
        r = out.setdefault(name, {'polls': 0, 'mixed': {}})
        for i, m in enumerate(segs):
            txt = line[m.end():segs[i + 1].start() if i + 1 < len(segs) else len(line)].strip(' |]\n')
            ok = txt.startswith('MATCH')
            n = re.search(r'at all (\d+) polls|MATCH\((\d+)\)', txt)
            if n:
                r['polls'] = max(r['polls'], int(n.group(1) or n.group(2)))
            res = 'MATCH' if ok else txt[:120]
            if m.group(1) in ('compiled', 'c'):
                r['compiled'] = res
            else:
                r['mixed'][m.group(1)] = res
    return out


def norm_name(n):
    n = re.sub(r'\.polls$', '', n)
    n = re.sub(r'^(eu|us)-', '', n)
    n = re.sub(r'^\d\d(?=sess|selfcheck|ses-)', '', n)
    n = re.sub(r'^(rec|sess|ses|app)-', '', n)
    return n


def import_results(spec):
    t, files = spec.split('=', 1)
    game = os.path.join(REPO, 'games', t)
    path = os.path.join(game, 'build', 'robust.json')
    rob = json.load(open(path)) if os.path.exists(path) else {'recordings': {}, 'sources': []}
    for f in files.split(','):
        for k, v in parse_results(f).items():
            old = rob['recordings'].get(k, {'polls': 0, 'mixed': {}})
            old['polls'] = max(old['polls'], v['polls'])
            if 'compiled' in v:
                old['compiled'] = v['compiled']
            old['mixed'].update(v['mixed'])
            rob['recordings'][k] = old
        rob['sources'].append(os.path.abspath(f))
    rob['updated'] = datetime.date.today().isoformat()
    os.makedirs(os.path.dirname(path), exist_ok=True)
    json.dump(rob, open(path, 'w'), indent=1)
    print(f'{t}: {len(rob["recordings"])} recordings in {path}')


def replay_kpi(game):
    path = os.path.join(game, 'build', 'robust.json')
    if not os.path.exists(path):
        return None
    recs = json.load(open(path))['recordings']
    if isinstance(recs, list):
        # robust.sh's format: [{"recording", "compiled": "MATCH: ... at all N polls", "mixed": "MATCH"|...}]
        conv = {}
        for r in recs:
            c, m = r.get('compiled', ''), r.get('mixed', '')
            n = re.search(r'at all (\d+) polls', c)
            conv[r['recording']] = {'compiled': 'MATCH' if c.startswith('MATCH') else c,
                                    'mixed': {'all': 'MATCH' if m.startswith('MATCH') else m},
                                    'polls': int(n.group(1)) if n else 0}
        recs = conv
    comp = [k for k, v in recs.items() if v.get('compiled') == 'MATCH']
    mixed = [k for k, v in recs.items() if v.get('compiled') == 'MATCH' and v['mixed']
             and all(x == 'MATCH' for x in v['mixed'].values())]
    fails = {k: v for k, v in recs.items() if k not in mixed}
    return {'recordings': len(recs), 'match_compiled': len(comp), 'match_mixed': len(mixed),
            'polls_verified': sum(recs[k]['polls'] for k in comp), 'not_matching': fails}


# ---------------------------------------------------------------- static (D, E)

def disasm():
    import capstone
    return capstone.Cs(capstone.CS_ARCH_M68K, capstone.CS_MODE_BIG_ENDIAN | capstone.CS_MODE_M68K_000)


def static_kpi(rom, conv):
    md = disasm()
    cov = bytearray(len(rom))
    refs = set()
    for a in sorted(conv):
        i = next(md.disasm(rom[a:a + 10], a), None)
        s = i.size if i else 2
        cov[a:a + s] = b'\1' * s
        if i:
            refs.update(int(x, 16) for x in re.findall(r'\$([0-9a-f]+)', i.op_str))
    code_bytes = sum(cov)
    refs = sorted(refs)
    # pointer-sized ROM longwords outside converted code: (value, location), top byte 0, even, in ROM
    ptrs = []
    for base in (0, 2):
        n = (len(rom) - base) // 4
        for k, v in enumerate(struct.unpack_from('>%dI' % n, rom, base)):
            if v < len(rom) and v >= 0x200 and not v & 1:
                loc = base + 4 * k
                if not cov[loc]:
                    ptrs.append((v, loc))
    ptrs.sort()
    strong, weak = [], []
    a = 0x200
    while a < len(rom):
        if cov[a]:
            a += 2
            continue
        b = a
        while b < len(rom) and not cov[b]:
            b += 1
        if b - a >= 64 and any(rom[a:b]):
            ok = 0
            for i in md.disasm(rom[a:b], a):
                ok += 1
                if ok > 400:
                    break
                if i.mnemonic in ('rts', 'rte', 'jmp') or i.mnemonic.startswith('bra'):
                    if ok >= 16:
                        end = i.address + i.size
                        j = bisect.bisect_left(refs, a)
                        ev = j < len(refs) and refs[j] < end
                        if not ev:
                            j = bisect.bisect_left(ptrs, (a, -1))
                            while j < len(ptrs) and ptrs[j][0] < end:
                                if not a <= ptrs[j][1] < end:
                                    ev = True
                                    break
                                j += 1
                        (strong if ev else weak).append([a, end])
                        break
        a = b
    return code_bytes, strong, weak


# ---------------------------------------------------------------- references (F)

def ref_addresses(src, rom):
    rom_size = len(rom)
    path = os.path.expanduser(src['path'])
    fmt, out = src['format'], set()
    for p in sorted(glob.glob(path)):
        s = open(p, errors='replace').read()
        if fmt == 'toml':
            vals = re.findall(r'^\s*0x([0-9A-Fa-f]{4,6})\b', s, re.M)
        elif fmt == 'decls':
            vals = re.findall(r'^void func_([0-9A-F]{6})\(void\);', s, re.M)
        elif fmt == 'csv':
            vals = [r[0] for r in csv.reader(s.splitlines()) if r and re.fullmatch(r'[0-9A-Fa-f]{6}', r[0])]
        elif fmt == 'notes':
            # RE notes in prose: instruction addresses at the start of disassembly lines, and $ADDR
            # right after a word that names code (RAM addresses share the low range, so no others)
            vals = re.findall(r'^\s*([0-9a-f]{5,6})\s{2}', s, re.M)
            # (not ranges "$70000-$77FFF" / "$7B000..": data, free space, Z80 banks)
            vals += re.findall(r'(?:jsr|jmp|bsr|bra|routine|handler|subroutine|function|calls?)\s*`?\(?\$([0-9A-Fa-f]{3,6})\b(?!\.[wb]|-|\.\.)', s)
        else:
            raise SystemExit(f'references.json: unknown format {fmt}')
        for v in vals:
            a = int(v, 16) + src.get('offset', 0)
            lo, hi = src.get('range', [0, 1 << 24])
            if lo <= a - src.get('offset', 0) < hi and 0x200 <= a < rom_size and not a & 1:
                if fmt == 'notes' and rom[a:a + 2] in (b'\xff\xff', b'\0\0'):
                    continue    # free space a port patches into (widescreen caves), not original code
                out.add(a)
    return out


def revision_ok(ref, rom):
    """None when the reference matches our ROM, else why not."""
    chk = ref.get('match', {})
    if 'rom' in chk:
        p = os.path.expanduser(chk['rom'])
        if not os.path.exists(p):
            return f'reference ROM {p} missing'
        theirs = open(p, 'rb').read()
        lo = chk.get('their_offset', 0)
        if theirs[lo:lo + len(rom)] != rom:
            return 'reference ROM differs from ours'
    if 'header' in chk and chk['header'] not in rom[0x180:0x190].decode('latin-1'):
        return f'reference is for "{chk["header"]}", our ROM header is "{rom[0x180:0x18E].decode("latin-1")}"'
    if 'checksum' in chk and rom[0x18E:0x190].hex().upper() != chk['checksum'].upper():
        return f'reference is for header checksum ${chk["checksum"]}'
    return None


def reference_kpi(title, rom, conv):
    refs = json.load(open(REFS)).get(title) if os.path.exists(REFS) else None
    if not refs:
        return None
    addrs, used, why = set(), [], []
    for r in refs:
        bad = revision_ok(r, rom)
        if bad:
            why.append(f'{r["name"]}: {bad}')
            continue
        got = ref_addresses(r, rom)
        if got:
            addrs |= got
            used.append(f'{r["name"]} ({len(got)})')
        else:
            why.append(f'{r["name"]}: no addresses read from {r["path"]}')
    if not addrs:
        return {'status': 'rev-mismatch' if why else 'n/a', 'notes': why}
    cov = addrs & conv
    return {'status': 'ok', 'routines': len(addrs), 'covered': len(cov), 'pct': 100 * len(cov) / len(addrs),
            'sources': used, 'notes': why, 'missing': ['$%06X' % a for a in sorted(addrs - conv)]}


# ---------------------------------------------------------------- main

def title_kpi(t, roms, do_static):
    game = os.path.join(REPO, 'games', t)
    cfg = json.load(open(os.path.join(game, 'sidebyside.json')))
    rom = open(os.path.join(roms, cfg['rom']), 'rb').read()
    conv, entries = generated(game, len(rom))
    ex, covers = executed(game, len(rom))
    both = ex & conv
    interp = sorted(ex - conv)
    never = sorted(entries - ex)
    k = {'title': t, 'converted': len(conv), 'executed': len(ex), 'executed_converted': len(both),
         'coverage_pct': 100 * len(both) / max(1, len(conv)), 'interpreted_only': len(interp),
         'interpreted_only_first': ['$%06X' % a for a in interp[:50]], 'cover_files': covers,
         'functions': len(entries), 'functions_never_entered': len(never),
         'never_entered': ['$%06X' % a for a in never], 'replay': replay_kpi(game)}
    if do_static:
        cb, strong, weak = static_kpi(rom, conv)
        k.update(code_bytes=cb, code_pct_rom=100 * cb / len(rom), suspects_strong=len(strong),
                 suspects_weak=len(weak), suspect_strong_list=['$%06X-%06X' % tuple(x) for x in strong],
                 suspect_weak_list=['$%06X-%06X' % tuple(x) for x in weak])
    k['reference'] = reference_kpi(t, rom, conv)
    json.dump(k, open(os.path.join(game, 'build', 'kpi.json'), 'w'), indent=1)
    return k


def row(k, date, sha):
    r, f = k['replay'] or {}, k['reference'] or {'status': 'n/a'}
    ok = f.get('status') == 'ok'
    v = {'date': date, 'git_sha': sha, 'title': k['title'], 'converted': k['converted'],
         'executed_converted': k['executed_converted'], 'coverage_pct': '%.2f' % k['coverage_pct'],
         'interpreted_only': k['interpreted_only'], 'recordings': r.get('recordings', ''),
         'match_compiled': r.get('match_compiled', ''), 'match_mixed': r.get('match_mixed', ''),
         'polls_verified': r.get('polls_verified', ''), 'functions': k['functions'],
         'functions_never_entered': k['functions_never_entered'], 'code_bytes': k.get('code_bytes', ''),
         'code_pct_rom': '%.1f' % k['code_pct_rom'] if 'code_pct_rom' in k else '',
         'suspects_strong': k.get('suspects_strong', ''), 'suspects_weak': k.get('suspects_weak', ''),
         'ref_routines': f['routines'] if ok else '', 'ref_covered': f['covered'] if ok else '',
         'ref_pct': '%.2f' % f['pct'] if ok else f['status']}
    return v


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('titles', nargs='*')
    ap.add_argument('--roms', default=os.environ.get('PS_ROMS', os.path.join(os.path.dirname(REPO), 'roms')))
    ap.add_argument('--no-history', action='store_true')
    ap.add_argument('--no-static', action='store_true', help='skip D/E (capstone)')
    ap.add_argument('--results', action='append', default=[], metavar='TITLE=FILE[,FILE]')
    o = ap.parse_args()
    if o.results:
        for s in o.results:
            import_results(s)
        return 0
    date = datetime.datetime.now().strftime('%Y-%m-%d %H:%M')
    sha = subprocess.run(['git', '-C', REPO, 'rev-parse', '--short', 'HEAD'], capture_output=True, text=True).stdout.strip()
    rows = []
    for t in o.titles or TITLES:
        k = title_kpi(t, o.roms, not o.no_static)
        rows.append(row(k, date, sha))
        f = k['reference'] or {}
        for n in f.get('notes', []):
            print(f'  {t}: reference {n}', file=sys.stderr)
    hdr = ('title', 'conv', 'exec∩conv', 'A cov%', 'B interp', 'C rec', 'C comp', 'C mixed', 'polls', 'fn',
           'never', 'D code%', 'E strong', 'E weak', 'F ref%')
    keys = ('title', 'converted', 'executed_converted', 'coverage_pct', 'interpreted_only', 'recordings',
            'match_compiled', 'match_mixed', 'polls_verified', 'functions', 'functions_never_entered',
            'code_pct_rom', 'suspects_strong', 'suspects_weak', 'ref_pct')
    w = [max(len(h), *(len(str(r[k])) for r in rows)) for h, k in zip(hdr, keys)]
    print('  '.join(h.rjust(x) if i else h.ljust(x) for i, (h, x) in enumerate(zip(hdr, w))))
    for r in rows:
        print('  '.join(str(r[k]).rjust(x) if i else str(r[k]).ljust(x) for i, (k, x) in enumerate(zip(keys, w))))
    if not o.no_history:
        os.makedirs(os.path.dirname(HIST), exist_ok=True)
        new = not os.path.exists(HIST)
        with open(HIST, 'a', newline='') as f:
            wr = csv.DictWriter(f, COLS)
            if new:
                wr.writeheader()
            wr.writerows(rows)
        print(f'appended {len(rows)} rows to {os.path.relpath(HIST, REPO)}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
