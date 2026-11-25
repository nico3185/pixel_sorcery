#!/usr/bin/env python3
"""menu_compose.py ROMDIR SCD.cue OUT.bin [--preview PNG]: the game-select screen made only of the games' own
graphics (owner: "generated from tiles and in game ... not AI generated in any way including the background",
"mega drive all of it", "even background could have some parallax").

  plane B  Green Hill's background as SH1's title screen has it in VRAM: its tiles, its name table, its two
           palette lines - menu.s scrolls it line by line (clouds slow, the water faster towards the bottom);
  plane A  the cards: SH2's option-screen boxes (its tiles) around each game's title screen shrunk to 56 x 48
           (menu_shots.py: booted on the reference player), "SELECT GAME" in SH2's menu font doubled, and a
           box at the bottom where menu.s writes the selected game's name (same font).
The VDP dumps come from the cartridge tracer (PS_RAMDUMP) run on the player's ROMs at build time; nothing of
them is stored in the repository.

OUT.bin (menu.s): CRAM (64 words) | plane A names 40 x 28 | plane B names 64 x 32 | plane B scroll speed per
screen line (224 words, 1/256 px a frame) | the 8 games' names for the bottom box, then 'NOT ON THIS DISC' (9 x 35 words) | the cards' layout (a
count word, 8 slots: game, sprite x, sprite y words, left / right / up / down slot bytes) | tiles.
Plane A is shown 4 px to the right (menu.s): the 9-cell cards with a 1-cell gap fill the 320 px evenly."""
import os, subprocess, sys, tempfile
from pathlib import Path
import numpy as np
from PIL import Image

HERE = Path(__file__).resolve().parent; ROOT = HERE.parents[2]
sys.path.insert(0, str(HERE))
import menu_shots

TRACE = ROOT / 'games/sh1/build-trace/genrecomp_build/genrecomp_trace'
NAMES = ['SH THE HEDGEHOG', 'SH THE HEDGEHOG 2', 'SH THE HEDGEHOG 3', 'SH AND KNUCKLES',
         'SH 1 AND KNUCKLES', 'SH 2 AND KNUCKLES', 'SH 3 AND KNUCKLES', 'SH CD',
         'NOT ON THIS DISC']                               # menu.s shows it for a card whose game the disc lacks
CARD_COL, CARD_ROW = (0, 10, 20, 30), (5, 14)        # plane A cells; a card is 9 x 8 cells, its shadow 1 more
VSB = 8                                              # plane B vertical scroll (menu.s)


# ------------------------------------------------------------------ VDP dumps (PS_RAMDUMP: RAM, VRAM, CRAM, regs)
def dump(rom, frame, movie=None):
    with tempfile.TemporaryDirectory() as t:
        env = {**os.environ, 'PS_RAMDUMP': f'{frame}:{t}/d.ram'}
        if movie:
            Path(t, 'm.mv').write_text(''.join('%x\n' % movie.get(i, 0) for i in range(frame + 10)))
            env['GENRECOMP_MOVIE'] = f'{t}/m.mv'
        subprocess.run([str(TRACE), '--headless', '--frames', str(frame + 2), '--trace-out', '/dev/null', rom], env=env,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        b = Path(t, 'd.ram').read_bytes()
    d = dict(vram=b[0x10000:0x20000], reg=b[0x20080:0x200A0])
    c = b[0x20000:0x20080]
    d['cram'] = [c[2 * i + 1] << 8 | c[2 * i] for i in range(64)]        # GPGX: BBBGGGRRR, little-endian words
    d['rgb'] = [(w & 7, w >> 3 & 7, w >> 6 & 7) for w in d['cram']]       # VDP levels 0..7
    r = d['reg']; d['nb'] = (r[4] & 7) << 13; d['na'] = (r[2] & 0x38) << 10; d['pw'] = [32, 64, 0, 128][r[16] & 3]
    return d


def tile(d, t):                                      # 8 x 8 colour indices
    v = d['vram'][t * 32:t * 32 + 32]
    return np.array([[v[y * 4 + x // 2] >> (4 if x % 2 == 0 else 0) & 15 for x in range(8)] for y in range(8)])


def entry(d, base, cx, cy):
    o = base + 2 * (cy * d['pw'] + cx); return d['vram'][o] << 8 | d['vram'][o + 1]


# ------------------------------------------------------------------ plane A, drawn in RGBA (VDP levels 0..7)
class Canvas:
    def __init__(self): self.px = np.zeros((224, 320, 4), int); self.pic = np.zeros((224, 320), bool)   # alpha 0: plane B shows

    def put(self, x, y, idx, pal, hf=0, vf=0):       # one tile's indices in a palette of 16 (index 0 transparent)
        if vf: idx = idx[::-1]
        if hf: idx = idx[:, ::-1]
        for yy in range(8):
            for xx in range(8):
                c = idx[yy, xx]
                if c and 0 <= y + yy < 224 and 0 <= x + xx < 320: self.px[y + yy, x + xx] = (*pal[c], 1)


def main():
    romdir, cue, out = sys.argv[1], sys.argv[2], sys.argv[3]
    s1 = dump(os.path.join(romdir, 'sh1.bin'), 720)                # SH1's title: Green Hill's background
    mv = {i: v for a, v in ((900, 2), (940, 2), (1000, 0x80)) for i in range(a, a + 4)}
    s2 = dump(os.path.join(romdir, 'sh2.bin'), 1300, mv)           # SH2's options: font and boxes
    font = lambda ch: tile(s2, {' ': 0}.get(ch, 0x10 + int(ch) if ch.isdigit() else 0x1E + ord(ch) - 65 if ch.isalpha()
                                         else {'*': 0x1A, ':': 0x1C, '.': 0x1D}.get(ch, 0)))
    s2p = s2['rgb']
    white = [(0, 0, 0)] + s2p[1:16]                                   # the font's own colours (line 0)
    gold = [(0, 0, 0)] + [(r, g, min(b, 1)) for r, g, b in s2p[1:16]] # the same shades, yellowed (the title)
    red = [(0, 0, 0)] + s2p[1:16]                                     # option box: line 0 = the red (selected) border
    cv = Canvas()
    # the title, the font doubled
    txt = 'SELECT GAME'; x0 = (320 - 16 * len(txt)) // 2 - 4
    for k, ch in enumerate(txt):
        if ch == ' ': continue
        g = np.kron(font(ch), np.ones((2, 2), int))
        for ty in range(2):
            for tx in range(2): cv.put(x0 + 16 * k + 8 * tx, 8 + 8 * ty, g[8 * ty:8 * ty + 8, 8 * tx:8 * tx + 8], gold)
    # a box of SH2's options screen: corners, edges, fill, its shadow (right column, bottom row)
    box_t = {k: tile(s2, t) for k, t in dict(tl=0x71, t=0x72, tr=0x73, trs=0x74, l=0x75, f=0x70, r=0x76, rs=0x77,
                                                 bl=0x7F, b=0x80, br=0x81, sl=0x82, s=0x83, sr=0x84).items()}
    fillc = s2['rgb'][s2['reg'][7] & 0x3F]       # what shows inside SH2's boxes: its backdrop blue
    def box(c0, r0, w, h):                       # w x h cells inside the shadow
        cv.px[r0 * 8 + 2:(r0 + h) * 8 - 2, c0 * 8 + 2:(c0 + w) * 8 - 2] = (*fillc, 1)
        for r in range(h + 1):
            for c in range(w + 1):
                k = (('tl' if c == 0 else 'tr' if c == w - 1 else 'trs' if c == w else 't') if r == 0 else
                     ('sl' if c == 0 else 'sr' if c == w else 's') if r == h else
                     ('bl' if c == 0 else 'br' if c == w - 1 else 'rs' if c == w else 'b') if r == h - 1 else
                     ('l' if c == 0 else 'r' if c == w - 1 else 'rs' if c == w else 'f'))
                cv.put((c0 + c) * 8, (r0 + r) * 8, box_t[k], red)
    # the cards on this disc (--cards 1,2,3,4,5,8: game numbers), four to a row, a shorter row centred - each slot:
    # its game, where it is (plane A cells), its neighbours left / right / up / down (menu.s moves through them)
    cards = [int(c) for c in sys.argv[sys.argv.index('--cards') + 1].split(',')] if '--cards' in sys.argv else list(range(1, 9))
    rows = [cards[:4], cards[4:8]]
    slots = []
    for r, row in enumerate(rows):
        c0 = (40 - (10 * len(row) - 1)) // 2
        for i, gm in enumerate(row): slots.append(dict(game=gm, col=c0 + 10 * i, row=CARD_ROW[r], r=r))
    def near(sl, r): return min((j for j, o in enumerate(slots) if o['r'] == r), key=lambda j: abs(slots[j]['col'] - sl['col']), default=None)
    for j, sl in enumerate(slots):
        sl['L'] = j - 1 if j > 0 and slots[j - 1]['r'] == sl['r'] else j
        sl['R'] = j + 1 if j + 1 < len(slots) and slots[j + 1]['r'] == sl['r'] else j
        up, dn = near(sl, sl['r'] - 1) if sl['r'] else None, near(sl, sl['r'] + 1)
        sl['U'] = j if up is None else up; sl['D'] = j if dn is None else dn
    shots = {}
    with tempfile.TemporaryDirectory() as tmp:
        for k in [c - 1 for c in cards]:
            shots[k] = menu_shots.shot(str(menu_shots.ROOT / 'games/sh1/build-trace/genrecomp_build/genrecomp_ref'),
                                romdir, cue, k, tmp)
    for sl in slots:
        k = sl['game'] - 1
        c0, r0 = sl['col'], sl['row']
        box(c0, r0, 9, 8)
        s = shots[k]
        if s is None: continue
        bb = s.point(lambda v: 255 if v > 8 else 0).getbbox()
        if bb: s = s.crop((bb[0], 0, bb[2], s.height))
        PWd, PHd = 68, 60                                               # the box inside its 2 px border line
        w = min(s.width, s.height * PWd // PHd + 24); xs = (s.width - w) // 2
        p = np.asarray(s.crop((xs, 0, xs + w, s.height)).resize((PWd, PHd), Image.Resampling.LANCZOS), float)
        q = np.clip(np.round(p / 255 * 7), 0, 7).astype(int)
        cv.px[r0 * 8 + 2:r0 * 8 + 2 + PHd, c0 * 8 + 2:c0 * 8 + 2 + PWd] = np.dstack([q, np.ones((PHd, PWd), int)])
        cv.pic[r0 * 8 + 2:r0 * 8 + 2 + PHd, c0 * 8 + 2:c0 * 8 + 2 + PWd] = True
        for r in range(8):                                              # the border line over the picture again
            for c in range(9):
                if r in (0, 7) or c in (0, 8):
                    k = ('tl' if c == 0 else 'tr' if c == 8 else 't') if r == 0 else ('bl' if c == 0 else 'br' if c == 8 else 'b') if r == 7 else ('l' if c == 0 else 'r')
                    cv.put((c0 + c) * 8, (r0 + r) * 8, box_t[k], red)
    box(1, 23, 37, 3)                                                   # the name box (menu.s writes the name)
    # ---------------------------------------------------------------- palettes: lines 2-3 Green Hill's, as they are;
    # line 0 the menu font's and boxes' colours, then what the screenshots need most; line 1 the screenshots
    fi = sorted({int(c) for ch in 'ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789' for c in font(ch).flat if c})
    bi = sorted({int(c) for t in box_t.values() for c in t.flat if c})
    fixed = []
    for c in [white[i] for i in fi] + [gold[i] for i in fi] + [red[i] for i in bi] + [fillc]:
        if c not in fixed and c != (0, 0, 0): fixed.append(c)
    a = cv.px; opaque = a[:, :, 3] == 1
    line0 = fixed[:15]
    ghz = [s1['rgb'][32:48], s1['rgb'][48:64]]
    from menu_art import kmeans
    nf = len(line0)
    cells = [(cy, cx) for cy in range(28) for cx in range(40) if cv.pic[cy * 8:cy * 8 + 8, cx * 8:cx * 8 + 8].any()]
    pix = {k: a[k[0] * 8:k[0] * 8 + 8, k[1] * 8:k[1] * 8 + 8][a[k[0] * 8:k[0] * 8 + 8, k[1] * 8:k[1] * 8 + 8, 3] == 1][:, :3]
           for k in cells}
    lab = {k: i % 2 for i, k in enumerate(cells)}
    for it in range(5):                          # lines 0 (its spare slots) and 1 fitted to their tiles, then each
        l0, l1 = list(line0), []                 # tile takes the best of the four lines
        for j, room in ((0, 15 - nf), (1, 15)):
            px_ = np.concatenate([pix[k] for k in cells if lab[k] == j] or [np.zeros((1, 3))])
            u = np.unique(px_, axis=0)
            c = u if len(u) <= room else np.clip(np.round(kmeans(px_.astype(float), room, 8, seed=it + j)[0]), 0, 7)
            (l0 if j == 0 else l1).extend(tuple(int(v) for v in x) for x in c[:room])
        cand = [l0, l1, list(ghz[0][1:]), list(ghz[1][1:])]
        for k in cells:
            errs = [((pix[k][:, None, :] - np.array(cp)[None]) ** 2).sum(-1).min(-1).sum() for cp in cand]
            lab[k] = int(np.argmin(errs))
    line0, line1 = l0, l1
    pals = [line0 + [(0, 0, 0)] * (15 - len(line0)), line1 + [(0, 0, 0)] * (15 - len(line1)), list(ghz[0][1:]), list(ghz[1][1:])]
    cram = [0] * 64
    for j, p in enumerate(pals):
        for i, (r, g, b) in enumerate(p): cram[16 * j + 1 + i] = b << 9 | g << 5 | r << 1
    bd = s1['reg'][7] & 0x3F
    r, g, b = s1['rgb'][bd]; cram[0] = b << 9 | g << 5 | r << 1          # the backdrop: SH1's
    # ---------------------------------------------------------------- plane A tiles (index 0 = transparent)
    store, index = [], {}
    def add(ix):
        for hf in (0, 1):
            for vf in (0, 1):
                v = ix[::-1 if vf else 1, ::-1 if hf else 1]; k = v.tobytes()
                if k in index: return index[k], hf, vf
        store.append(ix); index[ix.tobytes()] = len(store); return len(store), 0, 0
    names_a = []
    prev = np.zeros((224, 320, 3), np.uint8)
    for cy in range(28):
        for cx in range(40):
            cell = a[cy * 8:cy * 8 + 8, cx * 8:cx * 8 + 8]
            if not cell[:, :, 3].any(): names_a.append(0); continue
            best = None
            for j, p in enumerate(pals):
                pa = np.array(p)
                d = ((cell[:, :, None, :3] - pa[None, None]) ** 2).sum(-1)
                e = (d.min(-1) * cell[:, :, 3]).sum()
                if best is None or e < best[0]: best = (e, j, d.argmin(-1) + 1)
            _, j, ix = best; ix = ix * cell[:, :, 3]
            n, hf, vf = add(ix)
            names_a.append(n | j << 13 | hf << 11 | vf << 12 | 0x8000)       # plane A in front of the sprites' plane? high
            for yy in range(8):
                for xx in range(8):
                    if ix[yy, xx]: prev[cy * 8 + yy, cx * 8 + xx] = np.array(pals[j][ix[yy, xx] - 1]) * 36
    # the 8 names in the box: font tiles in line 0 (white)
    names_foot = []
    pa0 = np.array(pals[0])
    fidx = int(((pa0 - np.array(fillc)) ** 2).sum(-1).argmin()) + 1          # the box's blue in line 0
    for nm in NAMES:
        o = (35 - len(nm)) // 2
        row = [names_a[24 * 40 + 2 + k] for k in range(35)]                  # the box as composed (its blue)
        for k, ch in enumerate(nm):
            if ch == ' ': continue
            ix = font(ch)
            rgbix = np.array([[white[c] if c else (0, 0, 0) for c in r_] for r_ in ix])
            m = ((rgbix[:, :, None, :] - pa0[None, None]) ** 2).sum(-1).argmin(-1) + 1
            n, hf, vf = add(np.where(ix > 0, m, fidx))
            row[o + k] = n | hf << 11 | vf << 12 | 0x8000
        names_foot.append(row)
    # ---------------------------------------------------------------- plane B: SH1's name table and tiles
    nb_names, bmap = [], {}
    for cy in range(32):
        for cx in range(64):
            e = entry(s1, s1['nb'], cx, cy); t = e & 0x7FF
            if t not in bmap:
                store.append(tile(s1, t)); bmap[t] = len(store)
            nb_names.append(bmap[t] | (e & 0x7800 & ~0x8000) | (e & 0x6000))
    # line speeds (screen lines; plane B line = screen line + VSB): the clouds slowest, the water faster downwards
    sp = []
    for y in range(224):
        by = y + VSB
        sp.append(32 if by < 60 else 64 if by < 112 else 112 if by < 150 else int(160 + (by - 150) * 4))
    data = bytearray()
    for c in cram: data += int(c).to_bytes(2, 'big')
    for n in names_a: data += int(n).to_bytes(2, 'big')
    for n in nb_names: data += int(n).to_bytes(2, 'big')
    for v in sp: data += v.to_bytes(2, 'big')
    for row in names_foot:
        for n in row: data += int(n).to_bytes(2, 'big')
    data += len(slots).to_bytes(2, 'big')                               # the layout (menu.s): 8 slots of 10 bytes
    for j in range(8):
        sl = slots[min(j, len(slots) - 1)]
        data += sl['game'].to_bytes(2, 'big') + (sl['col'] * 8 + 4 + 128).to_bytes(2, 'big') + (sl['row'] * 8 + 128).to_bytes(2, 'big')
        data += bytes([sl['L'], sl['R'], sl['U'], sl['D']])
    for ix in store:
        for r_ in ix:
            for x in range(0, 8, 2): data.append(int(r_[x]) << 4 | int(r_[x + 1]))
    Path(out).write_bytes(data)
    print(f'{out}: {len(store)} tiles ({len(bmap)} background), plane A + B, palettes: menu {len(line0)}, screenshots {len(line1)}, Green Hill 2')
    if '--preview' in sys.argv:                  # plane B under plane A, as menu.s shows the first frame
        bgi = np.zeros((256, 512, 3), np.uint8)
        allp = [(0, 0, 0)] + [tuple(v) for v in pals[0]] + [(0, 0, 0)] + [tuple(v) for v in pals[1]]
        for cy in range(32):
            for cx in range(64):
                e = nb_names[cy * 64 + cx]; ix = store[(e & 0x7FF) - 1]; j = e >> 13 & 3
                if e >> 11 & 1: ix = ix[:, ::-1]
                if e >> 12 & 1: ix = ix[::-1]
                for yy in range(8):
                    for xx in range(8):
                        c = ix[yy, xx]
                        bgi[cy * 8 + yy, cx * 8 + xx] = np.array(pals[j][c - 1] if c else s1['rgb'][bd]) * 36
        scr = bgi[VSB:VSB + 224, :320].copy()
        m = a[:, :, 3] == 1
        sh = np.zeros_like(scr); sh[:, 4:] = prev[:, :316]; mm = np.zeros(m.shape, bool); mm[:, 4:] = m[:, :316]
        scr[mm] = sh[mm]
        Image.fromarray(scr).resize((960, 672), Image.Resampling.NEAREST).save(sys.argv[sys.argv.index('--preview') + 1])


if __name__ == '__main__':
    main()
