#!/usr/bin/env python3
"""menu_art.py IMAGE OUT [--preview PNG]: a picture as a Mega Drive screen (one plane, 320x224).

Scales the picture to 320x224, then fits the VDP: 9-bit colour (3 bits per channel), 8x8 tiles,
each tile using one of 4 palettes of 15 colours (index 0 is never used by a tile, so the
backdrop never shows through). Palettes come from clustering the tiles (k-means on their colour
content), then each palette is fitted to its tiles' pixels and every tile takes the palette that
represents it best; identical tiles (also mirrored) are stored once.
OUT.bin: 128 bytes CRAM (4 x 16 words), then the name table (40 x 28 words: tile number from 1,
palette in bits 13-14, H/V flip in bits 11-12), then the tiles (32 bytes each).
The picture is the user's (an input at build time); nothing of it is stored in the repository.
"""
import sys
import numpy as np
from PIL import Image

W, H = 320, 224


def kmeans(x, k, iters=12, seed=1):
    rng = np.random.default_rng(seed)
    c = x[rng.choice(len(x), size=min(k, len(x)), replace=False)].astype(float)
    for _ in range(iters):
        d = ((x[:, None, :] - c[None]) ** 2).sum(-1)
        lab = d.argmin(1)
        for j in range(len(c)):
            m = x[lab == j]
            if len(m):
                c[j] = m.mean(0)
    return c, lab


def to9(c):                                       # 0..255 -> nearest of the VDP's 8 levels
    return np.clip(np.round(c / 255 * 7), 0, 7)


def main():
    img = Image.open(sys.argv[1]).convert('RGB')
    out = sys.argv[2]
    if '--unframe' in sys.argv:                   # X0,Y0,X1,Y1 (picture px): a baked-in highlight
        x0, y0, x1, y1 = (int(v) for v in sys.argv[sys.argv.index('--unframe') + 1].split(','))
        a = np.asarray(img).copy()                # frame (yellow) takes the other cards' border
        box = a[y0:y1, x0:x1]
        yellow = (box[:, :, 0] > 170) & (box[:, :, 1] > 150) & (box[:, :, 2] < 110)
        ring = np.zeros(yellow.shape, bool); ring[:22] = ring[-22:] = True; ring[:, :22] = ring[:, -22:] = True
        box[yellow & ring] = (24, 30, 58)
        img = Image.fromarray(a)
    w = round(img.width * H / img.height)
    img = img.resize((w, H), Image.LANCZOS)
    x0 = (w - W) // 2
    px = np.asarray(img.crop((x0, 0, x0 + W, H)), dtype=float)
    q = to9(px)                                   # 9-bit colour per pixel
    tiles = q.reshape(H // 8, 8, W // 8, 8, 3).transpose(0, 2, 1, 3, 4).reshape(-1, 64, 3)
    nt = len(tiles)
    # 1. tile clusters by colour content (coarse histogram)
    feat = np.zeros((nt, 64))
    bins = (tiles[:, :, 0] // 2) * 16 + (tiles[:, :, 1] // 2) * 4 + tiles[:, :, 2] // 2
    for i in range(nt):
        feat[i] = np.bincount(bins[i].astype(int), minlength=64) / 64
    _, lab = kmeans(feat, 4)
    pals = []
    for it in range(4):
        pals = []
        for j in range(4):
            pix = tiles[lab == j].reshape(-1, 3)
            if len(pix) == 0:
                pix = tiles.reshape(-1, 3)
            uniq = np.unique(pix, axis=0)
            c = uniq if len(uniq) <= 15 else np.round(kmeans(pix, 15, 10, seed=j + it)[0])
            pals.append(np.vstack([c, np.zeros((15 - len(c), 3))]) if len(c) < 15 else c)
        # 2. each tile: the palette with the least error
        err = np.zeros((nt, 4))
        for j, p in enumerate(pals):
            d = ((tiles[:, :, None, :] - p[None, None]) ** 2).sum(-1)
            err[:, j] = d.min(-1).sum(-1)
        lab = err.argmin(1)
    # 3. index the pixels, dedupe tiles (with flips)
    cram = [0] * 64
    for j, p in enumerate(pals):
        for i, c in enumerate(p):
            r, g, b = (int(v) for v in c)
            cram[j * 16 + 1 + i] = (b << 9) | (g << 5) | (r << 1)
    store, index, names = [], {}, []
    preview = np.zeros((H, W, 3), dtype=np.uint8)
    for t in range(nt):
        p = pals[lab[t]]
        d = ((tiles[t][:, None, :] - p[None]) ** 2).sum(-1)
        ix = (d.argmin(1) + 1).reshape(8, 8)                  # 1..15
        ty, tx = divmod(t, W // 8)
        preview[ty * 8:ty * 8 + 8, tx * 8:tx * 8 + 8] = (p[ix - 1] * 36).astype(np.uint8)
        found = None
        for hf in (0, 1):
            for vf in (0, 1):
                v = ix[::-1 if vf else 1, ::-1 if hf else 1]
                k = v.tobytes()
                if k in index:
                    found = (index[k], hf, vf); break
            if found:
                break
        if not found:
            index[ix.tobytes()] = len(store) + 1
            store.append(ix)
            found = (len(store), 0, 0)
        n, hf, vf = found
        names.append(n | (int(lab[t]) << 13) | (hf << 11) | (vf << 12))
    data = bytearray()
    for c in cram:
        data += c.to_bytes(2, 'big')
    for n in names:
        data += n.to_bytes(2, 'big')
    for ix in store:
        for row in ix:
            for a in range(0, 8, 2):
                data.append(int(row[a]) << 4 | int(row[a + 1]))
    open(out, 'wb').write(data)
    if '--preview' in sys.argv:
        Image.fromarray(preview).resize((W * 3, H * 3), Image.NEAREST).save(sys.argv[sys.argv.index('--preview') + 1])
    print(f'{out}: {len(store)} tiles ({len(store) * 32} bytes), {nt} cells, 4 palettes')


if __name__ == '__main__':
    main()
