#!/usr/bin/env python3
"""Compose a Z80 RAM image (8 KB) from a ROM using a driver.json upload list."""
import json, sys
cfg = json.load(open(sys.argv[2]))
rom = open(sys.argv[1], 'rb').read()
img = bytearray(0x2000)
for u in cfg['uploads']:
    r, z, n = (int(u[k], 0) for k in ('rom', 'z80', 'len'))
    img[z:z + n] = rom[r:r + n]
open(sys.argv[3], 'wb').write(img)
print(f'build_image: {len(cfg["uploads"])} blocks -> {sys.argv[3]}')
