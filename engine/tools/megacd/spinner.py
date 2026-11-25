#!/usr/bin/env python3
"""spinner.py ROM GAME OUT.bin [--png PREVIEW.png]: the load screen's spinning Sonic, from the game's own art.
Each roll frame is drawn from Sonic's sprite tables (mappings, dynamic pattern load cues, uncompressed art) into
one 32x32 sprite: 16 tiles in the VDP's 4x4 order (column by column). OUT.bin: the frames one after another
(512 bytes each). The load screen copies them to VRAM before the disc load and cycles the sprite's tile number
while it waits (swap.s / thunk.s SPIN). Nothing of the art goes in the repository: it is read from your ROM."""
import sys
GAMES = {   # mappings, DPLC, art, roll frames, DPLC format, mapping format (SH1: 5-byte pieces, byte counts)
    'sh1': dict(maps=0x211E2, dplc=0x217FE, art=0x21AFE, frames=[0x2E, 0x2F, 0x30, 0x31], s1=True),
    'sh2': dict(maps=0x6FBE0, dplc=0x714E0, art=0x50000, frames=[0x3D, 0x3E, 0x3F, 0x40], s1=False),
}
W16 = lambda r, a: int.from_bytes(r[a:a + 2], 'big')
S16 = lambda v: v - 0x10000 if v & 0x8000 else v
S8 = lambda v: v - 0x100 if v & 0x80 else v


def frame(rom, g, f):
    """the frame's tiles (as its DPLC loads them, in order) and its pieces (x, y, w, h, first tile, flips)"""
    d = g['dplc'] + W16(rom, g['dplc'] + 2 * f); tiles = []
    if g['s1']:
        n = rom[d]; p = d + 1
        for _ in range(n):
            e = W16(rom, p); p += 2; cnt = (e >> 12) + 1; t = e & 0xFFF
            tiles += [rom[g['art'] + (t + k) * 32:g['art'] + (t + k + 1) * 32] for k in range(cnt)]
    else:
        n = W16(rom, d); p = d + 2
        for _ in range(n):
            e = W16(rom, p); p += 2; cnt = (e >> 12) + 1; t = e & 0xFFF
            tiles += [rom[g['art'] + (t + k) * 32:g['art'] + (t + k + 1) * 32] for k in range(cnt)]
    m = g['maps'] + W16(rom, g['maps'] + 2 * f); pieces = []
    if g['s1']:
        n = rom[m]; p = m + 1
        for _ in range(n):
            y = S8(rom[p]); sz = rom[p + 1]; a = W16(rom, p + 2); x = S8(rom[p + 4]); p += 5
            pieces.append((x, y, (sz >> 2 & 3) + 1, (sz & 3) + 1, a & 0x7FF, a >> 11 & 1, a >> 12 & 1))
    else:
        n = W16(rom, m); p = m + 2
        for _ in range(n):
            y = S8(rom[p]); sz = rom[p + 1]; a = W16(rom, p + 2); x = S16(W16(rom, p + 6)); p += 8
            pieces.append((x, y, (sz >> 2 & 3) + 1, (sz & 3) + 1, a & 0x7FF, a >> 11 & 1, a >> 12 & 1))
    return tiles, pieces


def compose(rom, g, f):
    """the frame drawn into a 32x32 canvas of 4-bit pixels, centred on the object's origin"""
    tiles, pieces = frame(rom, g, f); cv = [[0] * 32 for _ in range(32)]
    px = lambda t, x, y: (t[y * 4 + x // 2] >> (0 if x & 1 else 4)) & 15
    for x0, y0, w, h, t0, hf, vf in pieces:
        for cx in range(w):
            for cy in range(h):
                ti = t0 + cx * h + cy                                    # pieces' tiles go column by column
                if ti >= len(tiles): continue
                for yy in range(8):
                    for xx in range(8):
                        c = px(tiles[ti], xx, yy)
                        if not c: continue
                        X = x0 + (w - 1 - cx if hf else cx) * 8 + (7 - xx if hf else xx) + 16
                        Y = y0 + (h - 1 - cy if vf else cy) * 8 + (7 - yy if vf else yy) + 16
                        if 0 <= X < 32 and 0 <= Y < 32: cv[Y][X] = c
    return to_tiles(cv), cv


def to_tiles(cv):
    out = bytearray()
    for tx in range(4):                                                  # the VDP's 4x4 sprite: column by column
        for ty in range(4):
            for yy in range(8):
                for xx in range(0, 8, 2):
                    out.append(cv[ty * 8 + yy][tx * 8 + xx] << 4 | cv[ty * 8 + yy][tx * 8 + xx + 1])
    return out


def main():
    rom = open(sys.argv[1], 'rb').read(); g = GAMES[sys.argv[2]]; blob = bytearray(); cvs = []
    sizes = [16, 24, 32, 24] if '--pulse' in sys.argv else [32]     # --pulse: the ball breathes (pre-drawn sizes:
    for sz in sizes:                                                 #  the main CPU cannot scale a sprite)
        for f in g['frames']:
            _, cv = compose(rom, g, f)
            if sz != 32:                                             # smaller, nearest pixel, centred in 32x32
                o = (32 - sz) // 2
                cv = [[cv[(y - o) * 32 // sz][(x - o) * 32 // sz] if o <= x < o + sz and o <= y < o + sz else 0
                       for x in range(32)] for y in range(32)]
            blob += to_tiles(cv); cvs.append(cv)
    open(sys.argv[3], 'wb').write(blob)
    if '--png' in sys.argv:
        from PIL import Image
        pal = [(0, 0, 0)] + [(40 + i * 14, 60 + i * 10, 120 + i * 8) for i in range(15)]
        im = Image.new('RGB', (32 * len(cvs), 32))
        for k, cv in enumerate(cvs):
            for y in range(32):
                for x in range(32): im.putpixel((k * 32 + x, y), pal[cv[y][x]])
        im.resize((im.width * 8, 256), Image.Resampling.NEAREST).save(sys.argv[sys.argv.index('--png') + 1])
    print(f'{sys.argv[3]}: {len(cvs)} frames, {len(blob)} bytes')


if __name__ == '__main__':
    main()
