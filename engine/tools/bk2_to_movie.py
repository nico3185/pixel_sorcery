#!/usr/bin/env python3
"""bk2_to_movie.py MOVIE.bk2 OUT.txt: BizHawk Genesis movie -> GENRECOMP_MOVIE text movie.

A .bk2 is a zip; its "Input Log.txt" has one line per frame, "|..|P1 buttons|P2 buttons|",
in the order given by the LogKey line ('.' = released). Output: one "P1 P2" hex line per
frame in GEN_BTN_* bits (genrecomp/input.h). Header.txt (ROM, core) and SyncSettings.json
(pad type, region) are printed: BizHawk's Genplus-gx core is GPGX, our oracle.
"""
import sys
import zipfile

BITS = {'Up': 0x001, 'Down': 0x002, 'Left': 0x004, 'Right': 0x008, 'B': 0x010, 'C': 0x020,
        'A': 0x040, 'Start': 0x080, 'X': 0x100, 'Y': 0x200, 'Z': 0x400, 'Mode': 0x800}


def main():
    src, out = sys.argv[1], sys.argv[2]
    z = zipfile.ZipFile(src)
    for name in ('Header.txt', 'SyncSettings.json'):
        if name in z.namelist():
            print(f'{name}: ' + z.read(name).decode('utf-8', 'replace').strip().replace('\n', ' | '))
    log = z.read('Input Log.txt').decode('utf-8', 'replace').splitlines()
    key = next(l for l in log if l.startswith('LogKey:'))[len('LogKey:'):]
    groups = [g.split('|') for g in key.strip('|').split('|#')]   # "#Power|Reset|#P1 Up|..."
    groups = [[b.lstrip('#') for b in g if b] for g in groups]
    frames, resets = [], 0
    for line in log:
        if not line.startswith('|'):
            continue
        fields = line.strip('|').split('|')
        pads = [0, 0]
        for g, f in zip(groups, fields):
            for name, ch in zip(g, f):
                if ch == '.':
                    continue
                if name in ('Power', 'Reset'):
                    resets += 1
                    continue
                player, button = name.split(' ', 1)
                pads[int(player[1:]) - 1] |= BITS[button]
        frames.append(pads)
    with open(out, 'w') as f:
        f.write(f'# from {src}\n')
        for p1, p2 in frames:
            f.write(f'{p1:04X} {p2:04X}\n')
    print(f'{len(frames)} frames -> {out}' + (f' ({resets} power/reset presses ignored)' if resets else ''))


if __name__ == '__main__':
    main()
