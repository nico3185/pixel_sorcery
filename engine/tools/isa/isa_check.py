#!/usr/bin/env python3
"""isa_check.py [--per-op N] [--ops ADD.w,MOVE.l,...] [--jobs J] [--interp] [-v]: the recompiler checked
instruction by instruction, independently of any game.

Every 68000 instruction test of the SingleStepTests 680x0 suite (github.com/SingleStepTests/680x0,
~8000 random cases per operation: full register and memory state before and after one instruction)
is translated by generate.py's M68KTranslator, exactly as a game's code would be, compiled with
isa_rt.c (flat memory, the function table replaced by recorders) and run from the initial state;
the final registers, SR, memory, next PC and cycles must equal the suite's. --interp runs the
same tests through the runtime's 68000 interpreter (m68k_interp.c, which runs every instruction
the recompiler did not convert) instead of the generated C.

Not counted (the denominator leaves them out, the report lists them): tests ending in an address
error (a word/long access at an odd address - on a Mega Drive the game has crashed by then; the
recompiler does not model it), tests with the trace bit set, and instructions generate.py never
emits (RESET, STOP...: games' code does not use them; listed).

The suite (no licence file; "provided as an aid to reimplementation") is downloaded into
engine/tools/isa/cache and never committed. Output: a table per operation, engine/tools/isa/build/
isa_report.md, and every failure (test name, first difference) in build/<op>.log.
"""
import argparse
import concurrent.futures as cf
import gzip
import json
import os
import random
import subprocess
import sys
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ENG = os.path.dirname(os.path.dirname(HERE))
CACHE = os.path.join(HERE, 'cache')
BUILD = os.path.join(HERE, 'build')
SUITE = 'https://raw.githubusercontent.com/SingleStepTests/680x0/main/68000/v1/'
INDEX = 'https://api.github.com/repos/SingleStepTests/680x0/contents/68000/v1'
REGS = ['d0', 'd1', 'd2', 'd3', 'd4', 'd5', 'd6', 'd7', 'a0', 'a1', 'a2', 'a3', 'a4', 'a5', 'a6',
        'usp', 'ssp', 'sr', 'pc']
ADDRESS_ERROR = 3
SUB = False

sys.path.insert(0, os.path.join(ENG, 'tools', 'recompiler'))
from generate import M68KTranslator, CYCLES  # noqa: E402
from analyze import M68KAnalyzer             # noqa: E402
from m68k_decode import decode               # noqa: E402
from capstone import Cs, CS_ARCH_M68K, CS_MODE_M68K_000   # noqa: E402


def suite_files():
    os.makedirs(CACHE, exist_ok=True)
    idx = os.path.join(CACHE, 'index.json')
    if not os.path.exists(idx):
        urllib.request.urlretrieve(INDEX, idx)
    return sorted(e['name'][:-len('.json.gz')] for e in json.load(open(idx)) if e['name'].endswith('.json.gz'))


def load(op):
    p = os.path.join(CACHE, op + '.json.gz')
    if not os.path.exists(p):
        urllib.request.urlretrieve(SUITE + op + '.json.gz', p + '.part')
        os.replace(p + '.part', p)
    return json.load(gzip.open(p))


def exception_vector(t):
    """The vector an exception test took: two reads of the vector table (supervisor data) giving
    the final PC; None for an ordinary data read there."""
    rd = [tr for tr in t.get('transactions', []) if tr[0] == 'r' and tr[4] == '.w']
    for a, b in zip(rd, rd[1:]):
        if a[3] < 0x400 and a[3] % 4 == 0 and b[3] == a[3] + 2 and \
                ((a[5] << 16 | b[5]) & 0xFFFFFF) == t['final']['pc'] & 0xFFFFFF:
            return a[3] // 4
    return None


def stacked_sr(t, vec):
    """SR the CPU pushed for the exception: the stack word written last at the lowest address."""
    ws = [tr for tr in t['transactions'] if tr[0] == 'w' and tr[4] == '.w']
    for tr in ws:
        if tr[3] == (t['final']['ssp'] & 0xFFFFFF):
            return tr[5]
    return 0


def build_group(op, per_op, seed, interp=False):
    tests = load(op)
    random.Random(seed).shuffle(tests)
    md = Cs(CS_ARCH_M68K, CS_MODE_M68K_000)
    tr = M68KTranslator(None, {})
    fns, rows, ram = [], [], []
    stat = {'address_error': 0, 'trace': 0, 'untranslated': 0, 'undecoded': 0}
    untranslated = set()
    for t in tests:
        if len(rows) + stat['untranslated'] + stat['undecoded'] >= per_op:
            break
        i, f = t['initial'], t['final']
        if i['sr'] & 0x8000:
            stat['trace'] += 1
            continue
        vec = exception_vector(t)
        if vec == ADDRESS_ERROR:
            stat['address_error'] += 1
            continue
        pc = i['pc']
        mem = {a: v for a, v in i['ram']}
        for k, w in enumerate(i['prefetch']):
            mem[pc + 2 * k], mem[pc + 2 * k + 1] = w >> 8, w & 0xFF
        code = bytes(mem.get(pc + k, 0) for k in range(10))
        d = decode(md, code, pc)                  # the analyzer's decode, as for a game
        if d is None:
            stat['undecoded'] += 1
            continue
        mnem, op_str, size, raw = d
        op_str = M68KAnalyzer._fix_pc_base(op_str, raw)
        lines = tr.translate_instruction(pc, mnem, op_str, raw, pc, pc)
        body = '\n'.join(lines)
        if 'not translated:' in body and not interp:   # handed to the interpreter (generate.py)
            stat['untranslated'] += 1          # a failure: counted, not excluded
            untranslated.add(f'{mnem} {op_str}'.split(',')[0][:24])
            continue
        n = len(rows)
        fns.append(f'static void t{n}(void) {{ /* {mnem} {op_str} */\n{body}\n}}')
        r0 = len(ram) // 2
        for a, v in sorted(mem.items()):
            ram += [a, v]
        r1 = len(ram) // 2
        for a, v in f['ram']:
            ram += [a, v]
        init = [i[r] for r in REGS]
        fin = [f[r] for r in REGS]
        rows.append(f'{{T(t{n}), {json.dumps(t["name"])}, {{{",".join(map(str, init))}}}, '
                    f'{{{",".join(map(str, fin))}}}, {r0}, {r1 - r0}, {r1}, {len(f["ram"])}, {size}, '
                    f'{t["length"]}, {vec if vec is not None else -1}, {stacked_sr(t, vec) if vec else 0}}}')
    os.makedirs(BUILD, exist_ok=True)
    src = os.path.join(BUILD, f'isa_{op}.c')
    with open(src, 'w') as o:
        o.write('#include "isa_rt.h"\n#ifdef ISA_DATA_ONLY\n#define T(n) 0\n#else\n#define T(n) n\n'
                '#include <genrecomp/genrecomp.h>\n')
        for sz in (8, 16, 32):     # the helpers generate.py writes into recomp_common.h
            step = '(n == 7 ? 2 : 1)' if sz == 8 else str(sz // 8)
            o.write(f'static inline uint{sz}_t _postinc{sz}(int n) {{ uint{sz}_t v = bus_read{sz}(g_m68k.a[n]); g_m68k.a[n] += {step}; return v; }}\n')
            o.write(f'static inline uint{sz}_t _predec{sz}(int n) {{ g_m68k.a[n] -= {step}; return bus_read{sz}(g_m68k.a[n]); }}\n')
        o.write('\n'.join(fns) + '\n#endif\n')
        o.write('const IsaTest isa_tests[] = {\n' + ',\n'.join(rows) + '\n};\n')
        o.write(f'const int isa_ntests = {len(rows)};\n')
        o.write('const uint32_t isa_ram[] = {' + ','.join(map(str, ram or [0])) + '};\n')
    return src, len(rows), stat, sorted(untranslated)


def cycles_c():
    """The interpreter's cycle table (generate.py writes the same one next to a game's C)."""
    p = os.path.join(BUILD, 'ps_m68k_cycles.c')
    if not os.path.exists(p):
        os.makedirs(BUILD, exist_ok=True)
        rows = ',\n'.join(','.join(map(str, CYCLES[i:i + 32])) for i in range(0, 0x10000, 32))
        open(p, 'w').write('#include <stdint.h>\nconst uint16_t ps_m68k_cycles[0x10000] = {\n' + rows + '\n};\n')
    return p


def run_group(op, per_op, seed, verbose, interp=False):
    src, n, stat, untr = build_group(op, per_op, seed, interp)
    tag = ('_sub' if SUB else '_interp') if interp else ''
    exe = src[:-2] + tag
    cc = ['cc', '-O0', '-w', '-I', os.path.join(ENG, 'include'), '-I', HERE, src,
          os.path.join(HERE, 'isa_rt.c'), '-o', exe]
    if interp:
        cc = ['cc', '-O0', '-w', '-DISA_INTERP', '-DISA_DATA_ONLY', '-I', os.path.join(ENG, 'include'),
              '-I', os.path.join(ENG, 'src'), '-I', HERE, src, os.path.join(HERE, 'isa_srt.c' if SUB else 'isa_rt.c'),
              os.path.join(ENG, 'src', 's68k_interp.c' if SUB else 'm68k_interp.c'), cycles_c(), '-o', exe]
        untr, stat['untranslated'] = [], 0      # the interpreter decodes for itself
    res = {'op': op, 'tests': n, **stat, 'untranslated_forms': untr}
    if n == 0:
        res.update(passed=0, failed=0, cycles_off=0)
        return res
    res['tests'] = n
    c = subprocess.run(cc, capture_output=True, text=True)
    if c.returncode:
        res.update(passed=0, failed=n, cycles_off=0, error='compile: ' + c.stderr.strip().splitlines()[-1][:200])
        open(os.path.join(BUILD, op + tag + '.log'), 'w').write(c.stderr)
        return res
    r = subprocess.run([exe, '-v'], capture_output=True, text=True, timeout=600)
    out = r.stdout.splitlines()
    open(os.path.join(BUILD, op + tag + '.log'), 'w').write(r.stdout + r.stderr)
    tot = [l for l in out if l.startswith('TOTAL')]
    if not tot:
        res.update(passed=0, failed=n, cycles_off=0, error=f'crashed (rc {r.returncode})')
        return res
    _, p, fl, cy = tot[-1].split('\t')
    res.update(passed=int(p), failed=int(fl), cycles_off=int(cy))
    fails = [l.split('\t') for l in out if l.startswith('FAIL')]
    # the same tests on GPGX's core: a failure GPGX shares is a dispute with the suite, not ours
    gx = os.path.join(BUILD, f'gpgx_{op}{tag}')
    gp = os.path.join(ENG, 'ext', 'Genesis-Plus-GX', 'core')
    c = subprocess.run(['cc', '-O1', '-w', '-DLSB_FIRST', '-DISA_DATA_ONLY', '-I', gp, '-I', gp + '/m68k',
                        '-I', gp + '/debug', '-I', HERE, os.path.join(HERE, 'isa_gpgx.c'), src, '-o', gx],
                       capture_output=True, text=True)
    gpgx_bad = set()
    if c.returncode == 0:
        g = subprocess.run([gx], capture_output=True, text=True, timeout=600).stdout.splitlines()
        rows = [l.split('\t') for l in g if l.startswith('GPGX')]
        gpgx_bad = {x[1] for x in rows if x[2] != 'OK'}
        gpgx_cyc = {x[1]: int(x[3]) for x in rows}
        res['gpgx_fail'] = len(gpgx_bad)
        # cycles: a count that differs from the suite but equals GPGX's is GPGX's timing (our
        # cycle table is its table); the rest are ours
        cyc = [l.split('\t') for l in out if l.startswith('CYC')]
        res['cycles_as_gpgx'] = sum(1 for x in cyc if gpgx_cyc.get(x[1]) == int(x[2].split(',')[0]))
        ours_cyc = [x for x in cyc if gpgx_cyc.get(x[1]) != int(x[2].split(',')[0])]
        res['cycles_off'] = len(ours_cyc)
        if ours_cyc:
            res['first_cycle_diff'] = f'{ours_cyc[0][1]}: {ours_cyc[0][2]} (GPGX {gpgx_cyc.get(ours_cyc[0][1])})'
    ours = [f for f in fails if f[1] not in gpgx_bad]
    res['disputed'] = len(fails) - len(ours)
    res['ours'] = len(ours)
    if ours:
        res['first_failure'] = f'{ours[0][1]}: {ours[0][2]}'
    elif fails:
        res['first_failure'] = f'(dispute, GPGX agrees with us) {fails[0][1]}: {fails[0][2]}'
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--per-op', type=int, default=500)
    ap.add_argument('--ops', default='')
    ap.add_argument('--jobs', type=int, default=4)
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--interp', action='store_true')
    ap.add_argument('--sub', action='store_true', help='the Mega CD sub CPU\'s build of the interpreter (s68k_interp.c)')
    ap.add_argument('-v', action='store_true')
    a = ap.parse_args()
    global SUB
    SUB = a.sub
    a.interp = a.interp or a.sub
    ops = a.ops.split(',') if a.ops else suite_files()
    with cf.ThreadPoolExecutor(a.jobs) as ex:
        results = list(ex.map(lambda op: run_group(op, a.per_op, a.seed, a.v, a.interp), ops))
    for r in results:                          # untranslated / undecoded tests count as failed
        r['tests'] += r['untranslated'] + r['undecoded']
    lines = ['| operation | tests | pass | fail: ours | fail: GPGX does the same | cycles off (GPGX differs too) | '
             'excluded: address error | first failure |',
             '|---|---:|---:|---:|---:|---:|---:|---|']
    T = P = C = AE = D = 0
    for r in results:
        r.setdefault('disputed', 0)
        r['ours'] = r['tests'] - r['passed'] - r['disputed']      # untranslated forms are ours too
        T += r['tests']; P += r['passed']; C += r['cycles_off']; AE += r['address_error']; D += r['disputed']
        note = r.get('error') or r.get('first_failure', '') or r.get('first_cycle_diff', '')
        if r['untranslated_forms']:
            note = (note + '; ' if note else '') + 'not translated: ' + ', '.join(r['untranslated_forms'][:4])
        lines.append(f"| {r['op']} | {r['tests']} | {r['passed']} | {r['ours']} | {r['disputed']} | {r['cycles_off']} | "
                     f"{r['address_error']} | {note[:160]} |")
    lines.append(f'| **all** | **{T}** | **{P} ({100 * P / max(T, 1):.1f}%)** | **{T - P - D}** | **{D}** | **{C}** | '
                 f'**{AE}** | |')
    rep = '\n'.join(lines)
    os.makedirs(BUILD, exist_ok=True)
    tag = ('_sub' if a.sub else '_interp') if a.interp else ''
    open(os.path.join(BUILD, f'isa_report{tag}.md'), 'w').write(rep + '\n')
    json.dump(results, open(os.path.join(BUILD, f'isa_results{tag}.json'), 'w'), indent=1)
    print(rep)


if __name__ == '__main__':
    main()
