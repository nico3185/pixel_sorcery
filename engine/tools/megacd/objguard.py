#!/usr/bin/env python3
"""objguard.py ROM MAP TABLE COUNT DELETE: relocate.py arguments that make an image's object table safe.

An object whose code an image does not hold - no evidence run met it - kept its cartridge address in the object
table, and the disc jumped into whatever sits there when it spawned (SH3's object $2D in Angel Island 1: the
middle of the sprite renderer). Prints --data TABLE-END (every entry relocated, read or not) and, for each entry
whose code is not in the image (MAP: zonepack.py's LO-HI=NEW list), --patch ENTRY=<DELETE's image address>:
DeleteObject, so such an object removes itself as it spawns. TABLE, COUNT (long entries) and DELETE are hex /
decimal / hex. A summary goes to stderr."""
import sys


def main():
    rom = open(sys.argv[1], 'rb').read()
    m = [(int(a.split('-')[0], 16), int(a.split('-')[1].split('=')[0], 16), int(a.split('=')[1], 16))
         for a in open(sys.argv[2]).read().strip().split(',') if '=' in a]
    t0, n, dl = int(sys.argv[3], 16), int(sys.argv[4]), int(sys.argv[5], 16)
    d = next((new + dl - lo for lo, hi, new in m if lo <= dl < hi), None)
    out = ['--data', '%06X-%06X' % (t0, t0 + 4 * n)]
    miss = 0
    for t in range(t0, t0 + 4 * n, 4):
        v = int.from_bytes(rom[t:t + 4], 'big')
        if 0x200 <= v < len(rom) and not any(lo <= v < hi for lo, hi, _ in m):
            miss += 1
            if d is not None: out += ['--patch', '%X=%08X' % (t, d)]
    print(' '.join(out))
    print(f'{miss} object entries without their code -> DeleteObject' + ('' if d is not None else
          ' (DeleteObject itself is not in the image: left as they were)'), file=sys.stderr)


if __name__ == '__main__':
    main()
