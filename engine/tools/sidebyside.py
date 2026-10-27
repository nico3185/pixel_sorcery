#!/usr/bin/env python3
"""sidebyside.py — replay one input recording on the original (GPGX tracer) and on the
recompiled build, compare 68K RAM at every controller poll, and iterate:

  1. original: replay polls -> RAM checkpoints + code coverage (entry points)
  2. extract:  merge new entry points into recomp.json, regenerate C, rebuild
  3. recomp:   replay the same polls -> RAM checkpoints + log (dispatch misses)
  4. compare:  misses -> their addresses become entry points, go to 2
               RAM divergence without misses -> report first poll + addresses, stop
               identical to the last poll -> done

  sidebyside.py ROM GAMEDIR POLLS [--max-iter N]
GAMEDIR/sidebyside.json: {"exe": "build/coi", "mask": "FE00-10000,F370-F372"}
"""
import sys, argparse, json, os, re, subprocess, sys

ENG = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PY = next((p for p in (os.path.join(ENG, '.venv', 'bin', 'python'),
                       os.path.join(ENG, '.venv', 'Scripts', 'python.exe')) if os.path.exists(p)), sys.executable)


def generated_stale(game):
    """True when games/T/src/recomp is missing, or recomp.json or a recompiler source is newer
    than its newest file (the C was generated before them)."""
    out = os.path.join(game, 'src', 'recomp')
    gen = [os.path.join(out, f) for f in os.listdir(out)] if os.path.isdir(out) else []
    if not gen:
        return True
    rdir = os.path.join(ENG, 'tools', 'recompiler')
    inputs = [os.path.join(game, 'recomp.json')] + [os.path.join(rdir, f) for f in os.listdir(rdir) if f.endswith('.py')]
    return max(os.path.getmtime(p) for p in inputs) > max(os.path.getmtime(p) for p in gen)


def exe_path(path):
    """A build output as written on macOS/Linux (build/qs), or where MSVC puts it
    (build/Release/qs.exe, build-trace/genrecomp_build/Release/genrecomp_trace.exe)."""
    d, b = os.path.split(path)
    for p in (path, path + '.exe', os.path.join(d, 'Release', b + '.exe')):
        if os.path.isfile(p):
            return p
    return path


def build(game):
    """Re-glob regenerated sources and rebuild, keeping the build dir's generator (Ninja on
    macOS/Linux, Visual Studio on Windows). Returns the build's exit code."""
    bdir = os.path.join(game, 'build')
    gen = [] if os.path.exists(os.path.join(bdir, 'CMakeCache.txt')) else ['-G', 'Ninja']
    subprocess.run(['cmake', '-S', game, '-B', bdir, *gen, '-DCMAKE_BUILD_TYPE=Release'], stdout=subprocess.DEVNULL)
    return subprocess.run(['cmake', '--build', bdir, '--config', 'Release'], stdout=subprocess.DEVNULL).returncode


def run(cmd, env=None, log=None):
    e = dict(os.environ, **(env or {}))
    with open(log or os.devnull, 'w') as f:
        subprocess.run(cmd, env=e, stdout=subprocess.DEVNULL, stderr=f)


def load_ck(path):
    """Per poll: list of block hashes (frame count field 'f=<n>' dropped, see ck_frames())."""
    return [[t for t in l.split()[1:] if not t.startswith('f=')] for l in open(path)] if os.path.exists(path) else []


def ck_frames(path):
    return [int(t[2:]) for l in open(path) for t in l.split()[1:2] if t.startswith('f=')]


def first_div(a, b):
    for i, (x, y) in enumerate(zip(a, b)):
        if x != y:
            return i, [k for k in range(256) if x[k] != y[k]]
    return None, []


def ram_diff(a, b, mask):
    out = []
    for i in range(0, 0x10000, 2):
        if a[i:i + 2] != b[i:i + 2] and not any(lo <= i < hi for lo, hi in mask):
            out.append(i)
    runs = []
    for i in out:
        if runs and i - runs[-1][1] <= 4: runs[-1][1] = i
        else: runs.append([i, i])
    return len(out), ['$FF%04X-%04X' % (x, y + 2) for x, y in runs[:16]]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('rom'); ap.add_argument('game'); ap.add_argument('polls')
    ap.add_argument('--max-iter', type=int, default=8)
    o = ap.parse_args()
    rom, game, polls = map(os.path.abspath, (o.rom, o.game, o.polls))
    cfg = json.load(open(os.path.join(game, 'sidebyside.json')))
    mask_s = cfg.get('mask', '')
    mask = [tuple(int(v, 16) for v in m.split('-')) for m in mask_s.split(',') if m]
    work = os.path.join(game, 'build', 'sidebyside'); os.makedirs(work, exist_ok=True)
    tracer = exe_path(os.path.join(game, 'build-trace', 'genrecomp_build', 'genrecomp_trace'))
    exe = exe_path(os.path.join(game, cfg['exe']))
    n = sum(1 for _ in open(polls))
    base = {'PS_WIDE_OBJECTS': '0', 'PS_POLL_REPLAY': polls, 'PS_CHECKPOINT_MASK': mask_s, 'PS_POLL_STOP': str(n - 1)}
    # logic settings of the session (SESSION.meta): the oracle gets the same codes
    # (sidebyside.json "oracle": {"cheats": [...], "toggles": [...]}, bit order of the menu)
    meta = os.path.splitext(polls)[0] + '.meta'
    ev = []
    if os.path.exists(meta):
        ev = [list(map(int, (l.split() + ['0'])[2:6])) for l in open(meta) if l.startswith('logic ')]
    if cfg.get('wide_objects_logic') and any(e[0] and e[3] for e in ev):   # title hooks widen activation
        print('session played with widescreen enemies on a wide picture: game logic differs from the original by design; not comparable')
        sys.exit(2)
    oc_cfg = cfg.get('oracle', {})
    if any(e[1] for e in ev) and oc_cfg.get('cheats'):
        base['PS_ORACLE_PATCH'] = ','.join(oc_cfg['cheats'])
    if any(e[2] for e in ev) and oc_cfg.get('toggles'):
        base['PS_ORACLE_TOGGLES'] = ','.join(oc_cfg['toggles'])
    frames = ['--headless', '--frames', str(n * 4 + 2000)]
    ock, rck = os.path.join(work, 'orig.ck'), os.path.join(work, 'recomp.ck')
    seeds = os.path.join(work, 'orig.seeds')

    # instruction coverage for free: every PC the original executes, one bitmap per recording,
    # OR-merged across replays (engine/tools/kpi.py reads build/cover/*.bin)
    cover = os.path.join(game, 'build', 'cover'); os.makedirs(cover, exist_ok=True)
    pcc = os.path.join(cover, os.path.splitext(os.path.basename(polls))[0] + '.bin')
    print(f'original: replaying {n} polls on the GPGX tracer')
    run([tracer, *frames, '--trace-out', seeds, rom], dict(base, PS_CHECKPOINTS=ock, PS_PCCOVER=pcc))
    oc = load_ck(ock)
    print(f'original: {len(oc)} checkpoints')
    extra = os.path.join(work, os.path.basename(polls) + '.misses'); open(extra, 'a').close()   # per recording

    for it in range(1, o.max_iter + 1):
        out = subprocess.run([PY, os.path.join(ENG, 'tools', 'merge_seeds.py'),
                              os.path.join(game, 'recomp.json'), seeds, extra],
                             capture_output=True, text=True).stdout.strip()
        added = int(re.search(r'added (\d+)', out, re.I).group(1)) if re.search(r'added (\d+)', out, re.I) else 0
        if added or it == 1:
            # regenerate only when the C is older than its inputs (recomp.json, the recompiler):
            # a full regeneration recompiles everything (15-30 min for SH3K); ninja alone is a no-op
            if added or generated_stale(game):
                print(f'[{it}] extract: {added} new entry points -> regenerate + build')
                subprocess.run([PY, os.path.join(ENG, 'tools', 'recompiler', 'generate.py'), rom,
                                '-o', os.path.join(game, 'src', 'recomp'), '-c', os.path.join(game, 'recomp.json')],
                               cwd=game, stdout=subprocess.DEVNULL)
            else:
                print(f'[{it}] extract: 0 new entry points, generated C up to date -> build')
            if build(game):
                print('STOP: build failed'); return 4
            exe = exe_path(os.path.join(game, cfg['exe']))
        log = os.path.join(work, 'recomp.log')
        run([exe, *frames, rom], dict(base, PS_CHECKPOINTS=rck), log)
        rc = load_ck(rck)
        txt = open(log).read()
        misses = sorted(set(re.findall(r'no function at \$([0-9A-F]{6})', txt)))
        k, blocks = first_div(oc, rc)
        fo, fr = ck_frames(ock), ck_frames(rck)
        if fo and fr:
            d0 = fr[0] - fo[0]
            drift = next((i for i, (x, y) in enumerate(zip(fo, fr)) if y - x != d0), None)
            print(f'[{it}] timing: frame offset {d0:+d} at poll 0, first change at poll {drift}'
                  + (f' (orig {fo[drift]-fo[0]}, recomp {fr[drift]-fr[0]} frames)' if drift is not None else ''))
        print(f'[{it}] recomp: {len(rc)}/{len(oc)} polls, misses {len(misses)}, '
              f'first divergence {"none" if k is None else "poll %d" % k}')
        rom_size = os.path.getsize(rom)
        new = [m for m in misses if ('0x' + m.lower()) not in open(extra).read()
               and not (int(m, 16) & 1) and 0x200 <= int(m, 16) < rom_size]   # never seed garbage
        if new:
            with open(extra, 'a') as f:
                for m in new: f.write('0x%s\n' % m.lower())
            print(f'[{it}] fix: {len(new)} missed functions fed back as entry points: {" ".join("$" + m for m in new[:8])}')
            continue
        if k is None and len(rc) >= len(oc):
            print(f'MATCH: recompiled build equals the original at all {len(oc)} polls')
            return 0
        if k is None:
            print(f'STOP: recompiled run ended early at poll {len(rc)} (crash?) — see {log}')
            return 1
        for side, x in (('orig', tracer), ('recomp', exe)):
            env = dict(base, PS_CHECKPOINT_DUMP=f'{k}:{work}', PS_POLL_STOP=str(k))
            run([x, *frames] + (['--trace-out', os.devnull] if side == 'orig' else []) + [rom], env)
            os.replace(os.path.join(work, f'ram_{k}.bin'), os.path.join(work, f'{side}_{k}.bin'))
        a = open(os.path.join(work, f'orig_{k}.bin'), 'rb').read()[:0x10000]
        b = open(os.path.join(work, f'recomp_{k}.bin'), 'rb').read()[:0x10000]
        cnt, runs = ram_diff(a, b, mask)
        print(f'STOP: RAM diverges at poll {k} ({cnt} words): {" ".join(runs)}')
        print(f'      dumps: {work}/orig_{k}.bin, recomp_{k}.bin (GPGX word order); log {log}')
        return 2
    print('STOP: iteration limit'); return 3


if __name__ == '__main__':
    sys.exit(main())
