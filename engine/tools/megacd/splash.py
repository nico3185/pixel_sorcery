#!/usr/bin/env python3
"""splash.py ICON.png SONIC1.bin OUT.bin [--preview PNG]: the compilation disc's PixelSorcery splash (menu.s SPLASH),
shown before the game-select menu - 3 seconds at most: the icon turns once while it grows (NF frames at 20 fps), then
holds while Sonic rolls bottom-right like a loading wheel (SH1's roll frames, spinner.py, in his own colours).
Each frame: 16 x 16 cells (128 x 128 px), every cell with the best of 3 palettes (fitted over all the frames).
OUT.bin: CRAM (64 words: lines 0-2 the icon, line 3 Sonic's), then NF frames of a name table (256 words: tile 1-256 +
palette bits) and their 256 tiles (8 KB), then the roller's 16 frames (512 bytes each), then the text: its tile count, 160 name words (rows 22-25) and its tiles. ICON is the repository's
own picture (img/sorpix.png); Sonic's frames and colours are read from the player's ROM at build time."""
import sys
from pathlib import Path
import numpy as np
from PIL import Image
sys.path.insert(0, str(Path(__file__).parent))
from menu_art import kmeans, to9
import spinner

NF, S = 20, 128                  # frames, size in pixels (16 x 16 cells)
SONIC_PAL = 0x2388               # SH1: palette line 0 (Sonic's colours)
URL, YEARS = 'github.com/nico3185/pixel_sorcery', '2026-2027'


def frames(icon):
    out = []
    for f in range(NF):
        t = (f + 1) / NF
        e = 1 - (1 - t) ** 3                                     # ease out
        size = max(8, int(round(S * (0.08 + 0.92 * e))))
        a = 360 * (1 - e)                                        # one turn, ending upright
        im = icon.resize((size, size), Image.Resampling.LANCZOS).rotate(a, Image.Resampling.BICUBIC, expand=True)
        c = Image.new('RGBA', (S, S), (0, 0, 0, 0))
        c.alpha_composite(im, ((S - im.width) // 2, (S - im.height) // 2)) if im.width <= S else \
            c.alpha_composite(im.crop(((im.width - S) // 2, (im.height - S) // 2, (im.width + S) // 2, (im.height + S) // 2)))
        bg = Image.new('RGB', (S, S), (0, 0, 0)); bg.paste(c, mask=c.split()[3])
        out.append(np.asarray(bg, dtype=float))
    return out


def cutout(icon):
    """the badge alone: the backdrop flood-filled from the corners (tolerance 60) becomes transparent - round,
    with its banner, no square"""
    rgb = icon.convert('RGB'); w, h = rgb.size
    mark = rgb.copy()
    from PIL import ImageDraw
    for p in ((0, 0), (w - 1, 0), (0, h - 1), (w - 1, h - 1)):
        ImageDraw.floodfill(mark, p, (255, 0, 255), thresh=60)
    m = np.asarray(mark); a = np.where((m == (255, 0, 255)).all(-1), 0, 255).astype(np.uint8)
    out = icon.copy(); out.putalpha(Image.fromarray(a)); return out


def main():
    icon = cutout(Image.open(sys.argv[1]).convert('RGBA'))
    rom = open(sys.argv[2], 'rb').read()
    fr = [to9(x) for x in frames(icon)]
    cells = np.concatenate([x.reshape(16, 8, 16, 8, 3).transpose(0, 2, 1, 3, 4).reshape(-1, 64, 3) for x in fr])
    live = cells.reshape(len(cells), -1).max(1) > 0                # black cells take no palette fitting
    feat = np.zeros((len(cells), 64)); bins = (cells[:, :, 0] // 2) * 16 + (cells[:, :, 1] // 2) * 4 + cells[:, :, 2] // 2
    for i in range(len(cells)):
        feat[i] = np.bincount(bins[i].astype(int), minlength=64) / 64
    _, lab = kmeans(feat[live], 3)
    full = np.zeros(len(cells), int); full[live] = lab
    for it in range(3):
        pals = []
        for j in range(3):
            pix = cells[live & (full == j)].reshape(-1, 3)
            pix = pix[pix.max(1) > 0] if len(pix) else cells.reshape(-1, 3)
            uniq = np.unique(pix, axis=0)
            c = uniq if len(uniq) <= 15 else np.round(kmeans(pix, 15, 10, seed=j + it)[0])
            pals.append(np.vstack([c, np.full((15 - len(c), 3), 99)]) if len(c) < 15 else c)
        err = np.stack([((cells[:, :, None, :] - p[None, None]) ** 2).sum(-1).min(-1).sum(-1) for p in pals], 1)
        full = err.argmin(1)
    cram = [0] * 64
    for j, p in enumerate(pals):
        for i, c in enumerate(p):
            if c[0] < 9: cram[j * 16 + 1 + i] = int(c[2]) << 9 | int(c[1]) << 5 | int(c[0]) << 1
    for i in range(16):
        cram[48 + i] = int.from_bytes(rom[SONIC_PAL + 2 * i:SONIC_PAL + 2 * i + 2], 'big')
    blob = bytearray(b''.join(c.to_bytes(2, 'big') for c in cram)); prev = []
    for f in range(NF):
        nt, tiles = [], bytearray()
        for k in range(256):
            cell = cells[f * 256 + k]; j = full[f * 256 + k]; p = pals[j]
            black = cell.max(1) == 0
            idx = np.where(black, 0, ((cell[:, None, :] - p[None]) ** 2).sum(-1).argmin(1) + 1)
            for r in range(8):
                for x in range(0, 8, 2): tiles.append(int(idx[r * 8 + x]) << 4 | int(idx[r * 8 + x + 1]))
            nt.append(int(k + 1) | int(j) << 13)
            if f == NF - 1: prev.append([[pals[j][i - 1] if i else (0, 0, 0) for i in idx[r * 8:r * 8 + 8]] for r in range(8)])
        blob += b''.join(w.to_bytes(2, 'big') for w in nt) + tiles
    sp = bytearray()
    g = spinner.GAMES['sh1']
    for sz in (16, 24, 32, 24):
        for fno in g['frames']:
            _, cv = spinner.compose(rom, g, fno)
            if sz != 32:
                o = (32 - sz) // 2
                cv = [[cv[(y - o) * 32 // sz][(x - o) * 32 // sz] if o <= x < o + sz and o <= y < o + sz else 0 for x in range(32)] for y in range(32)]
            sp += spinner.to_tiles(cv)
    blob += sp
    # the text under the logo (rows 22-25 of the screen): the public repository and the years, Sonic's white
    # (palette line 3, colour 6: $0EEE) - Menlo Bold 12 px, thresholded to whole pixels (pixfont's 5 x 8 letters
    # where Menlo is missing); tiles from 769, 0 = blank
    bm = np.zeros((32, 320), int)
    try:
        from PIL import ImageDraw, ImageFont
        f = ImageFont.truetype('/System/Library/Fonts/Menlo.ttc', 12, index=1)
        im = Image.new('L', (320, 32), 0); d = ImageDraw.Draw(im)
        for y, t in ((1, URL), (17, YEARS)):
            d.text(((320 - d.textlength(t, font=f)) // 2, y), t, fill=255, font=f)
        bm = (np.asarray(im) > 110).astype(int)
    except OSError:
        import pixfont
        for row, t in ((2, URL), (18, YEARS)):
            r = np.array(pixfont.text(t)); x = (320 - r.shape[1]) // 2; bm[row:row + 8, x:x + r.shape[1]] = r
    tl, names = [], []
    for cy in range(4):
        for cx in range(40):
            c = bm[cy * 8:cy * 8 + 8, cx * 8:cx * 8 + 8]
            if not c.any(): names.append(0); continue
            b = bytes(((6 if c[r, x] else 0) << 4 | (6 if c[r, x + 1] else 0)) for r in range(8) for x in range(0, 8, 2))
            if b not in tl: tl.append(b)
            names.append(0x6000 | (769 + tl.index(b)))
    blob += len(tl).to_bytes(2, 'big') + b''.join(n.to_bytes(2, 'big') for n in names) + b''.join(tl)
    open(sys.argv[3], 'wb').write(blob)
    if '--preview' in sys.argv:
        im = np.zeros((128, 128, 3))
        for k, cell in enumerate(prev):
            cy, cx = divmod(k, 16)
            im[cy * 8:cy * 8 + 8, cx * 8:cx * 8 + 8] = np.array(cell)
        Image.fromarray((im * 255 / 7).astype(np.uint8)).resize((512, 512), Image.Resampling.NEAREST).save(sys.argv[sys.argv.index('--preview') + 1])
    print(f'{sys.argv[3]}: {NF} frames, {len(blob)} bytes')


if __name__ == '__main__':
    main()
