#!/usr/bin/env python3
"""disc_vs_cart.py DISC.cue PREFIX.polls ROM PIECE.polls...: does a game on the compilation disc follow the
owner's input as its cartridge does? Each PIECE (session_split.py's piece of a disc session) is replayed on DISC
after PREFIX (the session's reads up to that game's launch: boot, menu, the card chosen) and on ROM from power-on.
Game mode ($FFF600), zone ($FFFE10) and the player's x ($FFB010) are compared read by read.

Load lag is not a fault: the disc's CPU gets a little less done per frame (its interrupts pass through the BIOS
vectors, its code runs from Mega CD memory), so a load the game spreads over frames (SH3 decompresses a level's
art between VBlanks) can take one frame more - one pad read more - than on the cartridge. The owner's sessions were
made on the disc, so his reads follow the disc: replayed on the cartridge, everything after such a load arrives one
read early. A parting where one side is still in the mode the other has just left (or both in the same mode and
the next RESYNC reads match once one read is dropped or repeated) is realigned - the cartridge's input loses (or
repeats) that read - and the comparison goes on. A parting no realignment explains is reported: a disc fault.

--aligned OUT: write the piece as the cartridge needs it (realigned: for the builds' evidence).
--frames DIR: the disc's frames around a fault (every 30 frames)."""
import os, subprocess, sys, tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..'))
REF = os.path.join(ROOT, 'games/sh1/build-trace/genrecomp_build/genrecomp_ref')
RESYNC = 200          # reads that must match after a realignment
MAXFIX = 40           # realignments per piece


def watch(target, lines, region):
    t = tempfile.mkdtemp(prefix='dvc'); w = os.path.join(t, 'w.txt'); p = os.path.join(t, 'p.polls')
    open(p, 'w').write(''.join(lines))
    env = {**os.environ, 'PS_POLL_REPLAY': p, 'PS_WATCH': w + ':F600,FE10+2,B010+2'}
    if region: env['GENRECOMP_REGION'] = '1'
    subprocess.run([REF, '--headless', '--frames', str(len(lines) * 2 + 900), target], env=env,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    out = {}
    if os.path.exists(w):
        for l in open(w):
            f = l.split()
            if len(f) >= 5 and int(f[0]) not in out: out[int(f[0])] = (int(f[1]), (f[2], f[3], f[4]))
    return out


def compare(c, d, m, lo, hi):
    """first cartridge read in [lo, hi) whose state differs from the disc's read m[i]; None: all match"""
    for i in range(lo, hi):
        if i in c and m[i] in d and c[i][1] != d[m[i]][1]: return i
    return None


def check(disc, pre, rom, lines, fdir=None):
    k0 = len(pre)
    d = watch(disc, pre + lines, True)
    dd = {r - k0: v for r, v in d.items() if r >= k0}               # the disc, by piece read
    m = list(range(len(lines)))                                     # cartridge read i <- disc read m[i]
    fixes, start, c, cl = [], 0, None, None
    while True:
        if c is None:
            cl = [lines[j] for j in m if 0 <= j < len(lines)]
            m = m[:len(cl)]
            c = watch(rom, cl, False)
        r = compare(c, dd, m, start, len(m))
        if r is None: return cl, fixes, None, c, dd, m
        # a blip: one or two reads apart, then the same again with nothing changed (an object's field set a read
        # sooner on one side) - noted, not realigned
        if all(compare(c, dd, m, r + k, min(len(m), r + k + RESYNC)) is not None for k in (1, 2)) is False and \
                compare(c, dd, m, r + 3, min(len(m), r + 3 + RESYNC)) is None:
            fixes.append((r, 'blip')); start = r + 1; continue
        if len(fixes) >= MAXFIX: return cl, fixes, r, c, dd, m
        # realign at r: the disc read one more (drop disc read m[r] from the cartridge's input) or one less
        # (repeat it); kept when the next RESYNC reads match
        best = None
        for name, nm in (('drop', m[:r] + [j + 1 for j in m[r:]]), ('repeat', m[:r] + [m[r] - 1] + m[r:])):
            for back in (0, 1, 2, 3):                               # the extra read may sit a few reads before
                if back:
                    nm = (m[:r - back] + [j + 1 for j in m[r - back:]]) if name == 'drop' else \
                         (m[:r - back] + [m[r - back] - 1] + m[r - back:])
                cl2 = [lines[j] for j in nm if 0 <= j < len(lines)]
                c2 = watch(rom, cl2, False)
                nm2 = nm[:len(cl2)]
                if compare(c2, dd, nm2, max(0, r - back), min(len(nm2), r + RESYNC)) is None:
                    best = (name, back, nm2); break
            if best: break
        if not best: return cl, fixes, r, c, dd, m
        fixes.append((r - best[1], best[0])); m = best[2]; c = None; start = max(0, r - best[1])


def main():
    a = sys.argv[1:]; fdir = aligned = None
    if '--frames' in a: i = a.index('--frames'); fdir = a[i + 1]; del a[i:i + 2]
    if '--aligned' in a: i = a.index('--aligned'); aligned = a[i + 1]; del a[i:i + 2]
    disc, prefix, rom, pieces = a[0], a[1], a[2], a[3:]
    pre = open(prefix).readlines()
    for p in pieces:
        lines = open(p).readlines()
        if not lines: continue
        cl, fixes, r, c, dd, m = check(disc, pre, rom, lines)
        lg = [f'{x}{"-" if k == "drop" else "+"}' for x, k in fixes if k != 'blip']
        bl = [str(x) for x, k in fixes if k == 'blip']
        lag = (f', {len(lg)} load lag(s) realigned (reads {" ".join(lg)})' if lg else '') + \
              (f', {len(bl)} blip(s) (reads {" ".join(bl)})' if bl else '')
        if aligned: open(aligned, 'w').write(''.join(cl))
        if r is None:
            print(f'{os.path.basename(p)}: {len(lines)} reads - follows the cartridge throughout{lag}'); continue
        fr = dd[m[r]][0]
        print(f'{os.path.basename(p)}: {len(lines)} reads{lag}; FAULT at read {r} (disc frame {fr}): cartridge '
              f'mode/zone/x {"/".join(c[r][1])}, disc {"/".join(dd[m[r]][1])}', flush=True)
        if fdir:
            os.makedirs(fdir, exist_ok=True)
            t = tempfile.mkdtemp(prefix='dvc'); both = os.path.join(t, 'b.polls'); open(both, 'w').write(''.join(pre + lines))
            env = {**os.environ, 'GENRECOMP_REGION': '1', 'PS_POLL_REPLAY': both, 'PS_FRAMES_DIR': fdir, 'PS_FRAMES_EVERY': '30'}
            subprocess.run([REF, '--headless', '--frames', str(fr + 300), disc], env=env,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


if __name__ == '__main__':
    main()
