#!/usr/bin/env python3
"""plc.py ROM TABLE IDS PTRS_OUT FORCE_OUT: SH-style pattern load cues (PLC) taken whole.

TABLE: the PLC offset table (SH2: ArtLoadCues, $42660): word offsets from TABLE to lists of
"count - 1, then (art pointer.l, VRAM.w) * count"; the art is Nemesis-compressed. IDS: list ids
(hex, comma-separated, 'all' = every list). Some lists load only on events a run may never reach
(game over, sign post, results), so they are included from the ROM's own structure:
PTRS_OUT gets each art pointer's location (relocate.py --ptrs: rewritten like observed ones),
FORCE_OUT each art's compressed extent "LO-HI" (zonepack.py --force).
"""
import sys


def nemesis_end(rom, a):
    """End address of the Nemesis stream at a (decodes the codes, counts the pixels)."""
    hdr = int.from_bytes(rom[a:a + 2], 'big')
    tiles = hdr & 0x7FFF
    p = a + 2
    codes = {}                                   # (length, code) -> pixel count
    nyb = 0
    while True:
        b = rom[p]; p += 1
        if b == 0xFF:
            break
        if b & 0x80:
            nyb = b & 0x0F
            b = rom[p]; p += 1
        length = b & 0x0F
        count = ((b >> 4) & 7) + 1
        code = rom[p]; p += 1
        codes[(length, code)] = count
    need = tiles * 64                            # pixels
    bitpos = 0
    def bit():
        nonlocal bitpos
        v = rom[p + (bitpos >> 3)] >> (7 - (bitpos & 7)) & 1
        bitpos += 1
        return v
    while need > 0:
        code, length = 0, 0
        while True:
            code = code << 1 | bit(); length += 1
            if code == 0x3F and length == 6:     # inline: 3 bits count, 4 bits value
                cnt = 0
                for _ in range(3):
                    cnt = cnt << 1 | bit()
                for _ in range(4):
                    bit()
                need -= cnt + 1
                break
            if (length, code) in codes:
                need -= codes[(length, code)]
                break
            if length > 8:
                raise ValueError(f'bad Nemesis stream at {a:06X}')
    return p + (bitpos + 7 >> 3)


def main():
    rom = open(sys.argv[1], 'rb').read()
    t = int(sys.argv[2], 16)
    w0 = int.from_bytes(rom[t:t + 2], 'big')
    offs = [int.from_bytes(rom[t + 2 * i:t + 2 * i + 2], 'big') for i in range(w0 // 2)]
    ids = range(len(offs)) if sys.argv[3] == 'all' else [int(x, 16) for x in sys.argv[3].split(',')]
    ptrs, force = [], []
    for i in ids:
        a = t + offs[i]
        c = int.from_bytes(rom[a:a + 2], 'big') + 1
        force.append((a, a + 2 + 6 * c))         # the list itself
        for k in range(c):
            loc = a + 2 + 6 * k
            art = int.from_bytes(rom[loc:loc + 4], 'big') & 0xFFFFFF
            ptrs.append(loc)
            force.append((art, nemesis_end(rom, art)))
    open(sys.argv[4], 'w').write(''.join(f'{p:06X}\n' for p in ptrs))
    open(sys.argv[5], 'w').write(''.join(f'{lo:06X}-{hi:06X}\n' for lo, hi in force))
    print(f'{len(ids)} lists, {len(ptrs)} art pointers, {sum(h - l for l, h in force) // 1024} KB')


if __name__ == '__main__':
    main()
