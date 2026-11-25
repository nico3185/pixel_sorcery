#!/usr/bin/env python3
"""play_check.py DISC.cue [GAME...]: every piece of the owner's disc sessions (games/<g>/recordings/play-*.polls,
session_split.py's, with their .from: the session and the piece's first read) replayed on DISC - booted with the
session's own reads up to the launch - and on the game's cartridge, read by read (disc_vs_cart.py). A piece that
parts from the cartridge is a disc fault its play reaches. Exit 1 when one parts. --frames DIR: frames at the parting."""
import glob, os, subprocess, sys, tempfile
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..'))
ROMS = {'sh1': 'sh1.bin', 'sh2': 'sh2.bin', 'sh3': 'sh3.bin', 'shk': 'shk.bin', 'shk1': 'shk_sh1.bin'}
a = sys.argv[1:]; fr = []
if '--frames' in a: i = a.index('--frames'); fr = a[i:i + 2]; del a[i:i + 2]
disc, games = a[0], a[1:] or list(ROMS)
bad = 0
for g in games:
    for p in sorted(glob.glob(os.path.join(ROOT, 'games', g, 'recordings', 'play-*.polls'))):
        f = p[:-6] + '.from'
        if not os.path.exists(f): continue
        sess, start = open(f).read().split()
        pre = os.path.join(tempfile.mkdtemp(prefix='pc'), 'pre.polls')
        open(pre, 'w').write(''.join(open(os.path.join(ROOT, sess)).readlines()[:int(start)]))
        out = subprocess.run([sys.executable, os.path.join(os.path.dirname(__file__), 'disc_vs_cart.py'), disc, pre,
                              os.path.join(ROOT, 'roms', ROMS[g]), p] + fr, capture_output=True, text=True).stdout.strip()
        print(f'{g}: {out}', flush=True)
        bad += 'parts at' in out
sys.exit(1 if bad else 0)
