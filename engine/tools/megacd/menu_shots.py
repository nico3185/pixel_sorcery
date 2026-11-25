#!/usr/bin/env python3
"""menu_shots.py MENU.png OUT.png ROMDIR SCD.cue [REF]: the game-select picture with each game's own title screen
in its card (owner: "the select game screen should be the game splash in each box"). Each ROM boots on the
reference player (REF, default the SH1 build-trace genrecomp_ref) with no input, and one frame of its title
screen - the middle of the screen, its sides squeezed a little - replaces the card's picture at the same place;
SHCD from its own disc. A game whose ROM is missing keeps the picture's own card. Built from the player's ROMs
at build time; nothing of them is stored."""
import os, subprocess, sys, tempfile
from pathlib import Path
from PIL import Image

HERE = Path(__file__).resolve().parent; ROOT = HERE.parents[2]
# card order (menu.s: 1-8, left to right, top row first) - ROM, the frame its title screen is up (no input)
CARDS = [('sh1.bin', 720), ('sh2.bin', 960), ('sh3.bin', 600), ('shk.bin', 720),
         ('shk1.bin', 780), ('shk_sh2.bin', 840), ('sh3k.bin', 600), ('cd', 1300)]
XS, YS, PW, PH = (124, 464, 804, 1144), (206, 544), 276, 224   # the cards' pictures in the 1536 x 1024 menu


def run(ref, rom, frames, env):
    subprocess.run([ref, '--headless', '--frames', str(frames), rom], env={**os.environ, **env},
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def shot(ref, romdir, cue, k, tmp):
    rom, target = CARDS[k]
    d = Path(tmp) / str(k); d.mkdir()
    env = {}
    if rom == 'cd':
        if not cue or not os.path.exists(cue): return None
        mv = d / 'boot.mv'                               # Start on the BIOS screen, CD-ROM: the intro, then the title
        m = {i: 0x80 for a in (300, 420) for i in range(a, a + 4)}
        mv.write_text(''.join('%x\n' % m.get(i, 0) for i in range(target + 10)))
        env, path = {'GENRECOMP_REGION': '1', 'GENRECOMP_MOVIE': str(mv)}, cue
    else:
        path = os.path.join(romdir, rom)
        if not os.path.exists(path): return None
    run(ref, path, target + 1, {**env, 'PS_FRAMES_DIR': str(d), 'PS_FRAMES_EVERY': str(target)})
    f = d / ('f%06d.png' % target)
    return Image.open(f).convert('RGB') if f.exists() else None


def main():
    menu, out, romdir = sys.argv[1], sys.argv[2], sys.argv[3]
    cue = sys.argv[4] if len(sys.argv) > 4 else None
    ref = sys.argv[5] if len(sys.argv) > 5 else str(ROOT / 'games/sh1/build-trace/genrecomp_build/genrecomp_ref')
    im = Image.open(menu).convert('RGB')
    if im.size != (1536, 1024) or not os.path.exists(ref):
        im.save(out); print(f'{out}: the picture as it is (needs a 1536 x 1024 menu and {ref})'); return
    done = 0
    with tempfile.TemporaryDirectory() as tmp:
        for k in range(8):
            s = shot(ref, romdir, cue, k, tmp)
            if s is None: continue
            bb = s.point(lambda v: 255 if v > 8 else 0).getbbox()   # a narrower screen (SHCD's title): its
            if bb: s = s.crop((bb[0], 0, bb[2], s.height))             # black side columns off
            w = min(s.width, PW * s.height // PH + 20)
            x0 = (s.width - w) // 2
            im.paste(s.crop((x0, 0, x0 + w, s.height)).resize((PW, PH), Image.Resampling.LANCZOS),
                     (XS[k % 4], YS[k // 4])); done += 1
    im.save(out)
    print(f'{out}: {done} of 8 cards with a title screen')


if __name__ == '__main__':
    main()
