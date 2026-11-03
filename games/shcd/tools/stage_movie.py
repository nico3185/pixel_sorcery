#!/usr/bin/env python3
"""stage_movie.py K FRAMES OUT: the input movie for stage-select entry K (title screen: Up, Down, Down,
Left, Right, B; 74 entries on the Japanese disc), FRAMES frames long: BIOS menu Start then CD-ROM, the
code, Down to the entry, Start, then run right with C every 90 frames. Used by stage_starts.sh and
native_starts.sh."""
import sys

k, n = int(sys.argv[1]), int(sys.argv[2]); m = {}
def press(at, b, d=4):
    for i in range(at, at + d): m[i] = b
press(300, 0x80); press(420, 0x80)                     # BIOS menu: Start, then CD-ROM
for j, b in enumerate([1, 2, 2, 4, 8, 0x10]): press(1200 + j * 20, b, 6)    # the code
for j in range(2 * k): press(1400 + j * 12, 2, 3)      # Down: the cursor moves every second press
s = 1400 + 24 * k + 30; press(s, 0x80)                 # Start: the entry
for i in range(s + 200, n):                            # then right, C every 90 frames
    m[i] = 0x08 | (0x20 if (i - s) % 90 < 10 else 0)
open(sys.argv[3], 'w').write(''.join('%x\n' % m.get(i, 0) for i in range(n)))
