#!/usr/bin/env python3
"""stateval.py GAMEDIR ROM K1 [K2 ...] [--polls FILE] [--interp SPEC]
Uninterrupted checkpoint run vs save-at-K + load + continue: checkpoint lines from the
restored poll on must be byte-identical."""
import json, os, subprocess, sys, time

args = [a for a in sys.argv[1:]]
polls = None; interp = None
if '--polls' in args: i = args.index('--polls'); polls = args[i + 1]; del args[i:i + 2]
if '--interp' in args: i = args.index('--interp'); interp = args[i + 1]; del args[i:i + 2]
game, rom, *ks = args
game = os.path.abspath(game)
cfg = json.load(open(os.path.join(game, 'sidebyside.json')))
exe = os.path.join(game, cfg['exe'])
polls = os.path.abspath(polls or os.path.join(game, 'build', 'selfcheck.polls'))
n = sum(1 for _ in open(polls))
work = os.path.join(game, 'build', 'stateval'); os.makedirs(work, exist_ok=True)
base = dict(os.environ, PS_WIDE_OBJECTS='0', PS_POLL_REPLAY=polls, PS_CHECKPOINT_MASK=cfg.get('mask', ''),
            PS_POLL_STOP=str(n - 1))
if interp: base['PS_INTERP'] = interp
frames = ['--headless', '--frames', str(n * 4 + 2000)]

def run(env, log):
    t = time.time()
    with open(log, 'w') as f:
        subprocess.run([exe, *frames, rom], env=env, stdout=f, stderr=subprocess.STDOUT)
    return time.time() - t

def lines(p):
    return {int(l.split()[0]): l for l in open(p)} if os.path.exists(p) else {}

full = os.path.join(work, 'full.ck')
tf = run(dict(base, PS_CHECKPOINTS=full), os.path.join(work, 'full.log'))
F = lines(full)
print(f'{os.path.basename(game)}: {n} polls, uninterrupted run {len(F)} checkpoints ({tf:.0f}s)')
for k in ks:
    st = os.path.join(work, f's{k}.state')
    if os.path.exists(st): os.remove(st)
    run(dict(base, PS_STATE_SAVE_AT=f'{k}:{st}', PS_POLL_STOP=str(min(int(k) + 30, n - 1))), os.path.join(work, f'save{k}.log'))
    saved = [l.strip() for l in open(os.path.join(work, f'save{k}.log')) if 'ps_state:' in l]
    ck = os.path.join(work, f'load{k}.ck')
    tl = run(dict(base, PS_STATE_LOAD=st, PS_CHECKPOINTS=ck), os.path.join(work, f'load{k}.log'))
    L = lines(ck)
    first = min(L) if L else None
    same = sum(1 for p, l in L.items() if F.get(p) == l)
    diff = sorted(p for p, l in L.items() if F.get(p) != l)
    missing = [p for p in F if first is not None and p >= first and p not in L]
    print(f'  K={k}: {saved[0] if saved else "NOT SAVED"}')
    print(f'    restored from poll {first}: {same}/{len(L)} identical, first difference '
          f'{diff[0] if diff else "none"}, {len(missing)} polls missing ({tl:.0f}s)')
