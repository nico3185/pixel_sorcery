#!/usr/bin/env python3
"""menu_default.py OUT.png: a plain game-select picture for the compilation disc, for a build that has no
picture of its own (ultimate_sh.sh uses it when MENU is "-" or missing). 320x224, the 4x2 cards where
menu.s draws its frame (72x72 at x 13/88/163/238, y 39/114), each card's game name as text; the yellow of the
heading is the colour build_disc.py picks for the frame. Drawn here, nothing from any game."""
import sys
from PIL import Image, ImageDraw, ImageFont

CARDS = ['SH 1', 'SH 2', 'SH 3', 'SH &\nKNUCKLES', 'BLUE\nSPHERE', 'KNUCKLES\nIN SH 2',
         'SH 3 &\nKNUCKLES', 'SH CD']
BG, CARD, EDGE, TEXT, YELLOW = (16, 24, 64), (36, 72, 160), (90, 140, 230), (240, 240, 240), (255, 255, 0)


def font(size):
    try: return ImageFont.load_default(size)
    except TypeError: return ImageFont.load_default()   # Pillow < 10.1: one size


def main():
    if len(sys.argv) != 2: sys.exit(__doc__)
    im = Image.new('RGB', (320, 224), BG); d = ImageDraw.Draw(im)
    d.text((160, 18), 'ULTIMATE SH', fill=YELLOW, font=font(16), anchor='mm')
    small = font(9)
    for i, name in enumerate(CARDS):
        x, y = (13, 88, 163, 238)[i % 4], (39, 114)[i // 4]
        d.rectangle((x + 4, y + 4, x + 67, y + 67), fill=CARD, outline=EDGE)
        d.text((x + 36, y + 22), str(i + 1), fill=EDGE, font=font(14), anchor='mm')
        d.multiline_text((x + 36, y + 48), name, fill=TEXT, font=small, anchor='mm', align='center', spacing=2)
    d.text((160, 205), 'START: PLAY', fill=TEXT, font=small, anchor='mm')
    im.save(sys.argv[1])


if __name__ == '__main__':
    main()
