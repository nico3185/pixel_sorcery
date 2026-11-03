#!/usr/bin/env python3
"""z80_drivers.py CAPTURE_DIR OUT_DIR: SHCD's Z80 sound drivers -> C (no Z80 emulation on the
native build). CAPTURE_DIR holds the captures of one run through the BIOS and the disc
(z80_drivers.sh): imgN.bin, Z80 RAM at each start whose RAM differs from the previous one, and, from
the tracer only, pcsN.bin, the Z80 addresses executed until the next start. Starts whose code bytes
are the same (the BIOS restarting its driver with other work RAM) are one driver. Each driver's code
range runs from $0000 (or its highest executed address) on through the uploaded bytes
(find_driver.py's rule: 8 zeros end it); on SHCD's two drivers both give the same range. Writes OUT_DIR/z80_<k>.c (z80recomp.py) and OUT_DIR/z80_drivers.c: the code bytes
each driver was recompiled from and ps_shcd_z80_drivers(), which registers them all; at each
/RESET release the runtime starts the one whose bytes are in Z80 RAM (ps_audio_add_driver).
Everything written is derived from the player's BIOS and disc: git-ignored (src/recomp).
"""
import glob
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
Z80RECOMP = os.path.join(HERE, '..', '..', '..', 'engine', 'tools', 'z80recomp', 'z80recomp.py')


def main():
    cap, out = sys.argv[1], sys.argv[2]
    drivers = []                                   # [image, executed set, code end]
    for f in sorted(glob.glob(os.path.join(cap, 'img*.bin')), key=lambda p: int(re.search(r'(\d+)', os.path.basename(p)).group(1))):
        img = open(f, 'rb').read()
        pf = f.replace('img', 'pcs')
        if os.path.exists(pf):
            b = open(pf, 'rb').read()
            pcs = {i for i in range(0x2000) if b[i >> 3] >> (i & 7) & 1}
            if not pcs:
                continue                           # uploaded, never run
        else:
            pcs = {0}                              # native capture: from the entry point
        same = next((d for d in drivers if img[:d[2] + 1] == d[0][:d[2] + 1]), None)
        if same:
            same[1] |= pcs
        else:
            drivers.append([img, pcs, 0])
        d = same or drivers[-1]
        end = max(d[1])
        while end + 1 < 0x2000:
            run = next((k for k in range(1, 9) if end + k < 0x2000 and d[0][end + k]), 0)
            if not run:
                break
            end += run
        d[2] = max(d[2], end)
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, 'z80_drivers.c'), 'w') as w:
        w.write('/* SHCD\'s Z80 sound drivers (games/shcd/tools/z80_drivers.py): the code bytes each was\n'
                ' * recompiled from, checked against Z80 RAM at each start */\n#include "genrecomp/ps_z80rt.h"\n')
        w.write('void ps_audio_add_driver(void (*run)(PsZ80 *), const unsigned char *image, unsigned len);\n')
        for k, (img, pcs, end) in enumerate(drivers):
            open(os.path.join(out, f'z80_{k}.bin'), 'wb').write(img)
            subprocess.run([sys.executable, Z80RECOMP, '--image', os.path.join(out, f'z80_{k}.bin'),
                            '--code', f'0x0000-0x{end:04X}', '--name', f'cd{k}', '-o', os.path.join(out, f'z80_{k}.c')],
                           check=True, stdout=subprocess.DEVNULL)
            os.remove(os.path.join(out, f'z80_{k}.bin'))
            w.write(f'void ps_z80_cd{k}_run(PsZ80 *z);\n')
            w.write(f'static const unsigned char s_image{k}[{end + 1}] = {{{",".join(str(x) for x in img[:end + 1])}}};\n')
            print(f'driver {k}: code $0000-${end:04X}', file=sys.stderr)
        w.write('void ps_shcd_z80_drivers(void) {\n')
        for k, (_, _, end) in enumerate(drivers):
            w.write(f'    ps_audio_add_driver(ps_z80_cd{k}_run, s_image{k}, {end + 1});\n')
        w.write('}\n')


if __name__ == '__main__':
    main()
