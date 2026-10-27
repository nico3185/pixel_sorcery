#!/usr/bin/env python3
"""action_prior.py MOVIE.txt OUT.prior [--from FRAME] [--map A=B,B=A,C=C]

Learn how a skilled player uses the pad from an input movie (a TAS), for explore's
bursts in any game: the movie becomes runs of one held combination ("A+RIGHT for 14
frames"), and the prior keeps
  - which combination follows which (a first-order Markov chain over runs), and
  - how long each combination is held.
Buttons are renamed by role with --map (QS's jump C and fire A become WOI's jump C and attack B: --map A=B,C=C,B=A). START is dropped: pausing is not
play. Output lines: "T PREV NEXT COUNT" and "D COMBO FRAMES COUNT", combos in hex
GEN_BTN bits (U1 D2 L4 R8 B10 C20 A40 S80).
"""
import collections
import sys

BITS = {'A': 0x40, 'B': 0x10, 'C': 0x20}


def main():
    src, out = sys.argv[1], sys.argv[2]
    start = int(sys.argv[sys.argv.index('--from') + 1]) if '--from' in sys.argv else 0
    remap = {}
    if '--map' in sys.argv:
        for pair in sys.argv[sys.argv.index('--map') + 1].split(','):
            a, b = pair.split('=')
            remap[BITS[a]] = BITS[b]
    frames = [int(l.split()[0], 16) for l in open(src) if l.strip() and not l.startswith('#')][start:]

    def rename(x):
        x &= 0x7F                                   # no START, no 6-button extras
        y = x & 0x0F
        for f, t in remap.items():
            if x & f:
                y |= t
        if not remap:
            y |= x & 0x70
        return y

    runs = []
    for x in map(rename, frames):
        if runs and runs[-1][0] == x:
            runs[-1][1] += 1
        else:
            runs.append([x, 1])
    trans = collections.Counter((a[0], b[0]) for a, b in zip(runs, runs[1:]))
    durs = collections.Counter((c, min(n, 240)) for c, n in runs)
    with open(out, 'w') as f:
        f.write(f'# action prior from {src} (frames {start}+): {len(runs)} runs\n')
        for (a, b), n in sorted(trans.items()):
            f.write(f'T {a:04X} {b:04X} {n}\n')
        for (c, d), n in sorted(durs.items()):
            f.write(f'D {c:04X} {d} {n}\n')
    print(f'{len(frames)} frames, {len(runs)} runs, {len(trans)} transitions, '
          f'{len({c for c, _ in durs})} combinations -> {out}')


if __name__ == '__main__':
    main()
