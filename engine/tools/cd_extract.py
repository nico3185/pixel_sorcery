#!/usr/bin/env python3
"""cd_extract.py TRACK01.bin|.iso OUTDIR [--list]: the files of a Mega CD disc's data track (ISO 9660,
2352-byte MODE1 sectors or 2048-byte .iso), for recompiling its programs (SHCD: .MMD main-CPU
programs, .BIN sub-CPU programs). Writes OUTDIR/<NAME> per file and OUTDIR/mmd.tsv: per .MMD its
header (load address, 0 = Word RAM $200000; entry; HInt; VInt), and for each Word RAM program
OUTDIR/img/<NAME>.bin + .json: the program at $200000 in a cartridge-sized image (vectors at its
entry/HInt/VInt, ILLEGAL below it), the generator's input as for a cartridge; a work RAM program
(load $FF0000 / $FF2000: copied there without its 0x100-byte header) likewise in a 16 MB image. From your own disc only: the
output is game data, never committed (games/*/disc/ is git-ignored)."""
import json
import os
import re
import struct
import sys


def reader(path):
    f = open(path, 'rb')
    raw = not path.lower().endswith('.iso')                     # .bin/.img: raw 2352-byte sectors
    def sec(n):
        f.seek(n * 2352 + 16 if raw else n * 2048)
        return f.read(2048)
    def read(lba, size):
        return b''.join(sec(lba + i) for i in range((size + 2047) // 2048))[:size]
    return sec, read


def files(sec, read):
    pvd = sec(16)
    if pvd[1:6] != b'CD001': sys.exit('not an ISO 9660 data track')
    ext, ln = struct.unpack('<I', pvd[158:162])[0], struct.unpack('<I', pvd[166:170])[0]
    d, i, out = read(ext, ln), 0, []
    while i < len(d):
        n = d[i]
        if n == 0: i = (i // 2048 + 1) * 2048; continue
        e = d[i:i + n]
        name = e[33:33 + e[32]].decode('ascii', 'replace').split(';')[0]
        if name not in ('\x00', '\x01') and not e[25] & 2:
            out.append((name, struct.unpack('<I', e[2:6])[0], struct.unpack('<I', e[10:14])[0]))
        i += n
    return out


def write_image(out, name, b, entry, hint, vint, load=0):
    if load:                                                    # work RAM: the program without its
        img = bytearray(b'\x4a\xfc' * (load // 2)) + b[0x100:]  # 0x100-byte header, at its address
        img += b'\x4a\xfc' * ((0x1000000 - len(img)) // 2)       # (a 16 MB image)
    else:
        img = bytearray(b'\x4a\xfc' * 0x100000) + b           # ILLEGAL up to $200000: never code
    struct.pack_into('>I', img, 0, 0x00FFFD00)
    for v in range(1, 64): struct.pack_into('>I', img, v * 4, entry)
    struct.pack_into('>I', img, 0x70, hint); struct.pack_into('>I', img, 0x78, vint)
    img[0x100:0x110] = b'SEGA MEGA DRIVE '
    d = os.path.join(out, 'img'); os.makedirs(d, exist_ok=True)
    stem = os.path.join(d, name.split('.')[0].rstrip('_'))
    open(stem + '.bin', 'wb').write(img)
    seeds = {'0x%06X' % a: n for a, n in ((entry, 'mmd_entry'), (hint, 'mmd_hint'), (vint, 'mmd_vint')) if a}
    json.dump({'title': os.path.basename(stem), 'seeds': seeds, 'overlap_ties': 'inner', 'filler': '4AFC'}, open(stem + '.json', 'w'))


def main():
    if len(sys.argv) < 3: sys.exit(__doc__)
    sec, read = reader(sys.argv[1])
    out = sys.argv[2]; os.makedirs(out, exist_ok=True)
    rows = []
    for name, lba, size in files(sec, read):
        if '--list' in sys.argv: print(f'{name:16} lba {lba:6} size {size}'); continue
        if name.endswith('.STM'): continue                      # FMV streams: not code
        b = read(lba, size)
        open(os.path.join(out, name), 'wb').write(b)
        if name.endswith('.MMD'):
            flags, load, words, entry, hint, vint = struct.unpack('>HIHIII', b[:0x14])
            if load == 0 or load >= 0xFF0000: write_image(out, name, b, entry, hint, vint, load)
            rows.append(f'{name}\t{load:06X}\t{entry:06X}\t{hint:06X}\t{vint:06X}\t{flags:04X}\t{size}')
    if '--list' not in sys.argv:          # the boot code: the BIOS copies the disc's sector 0 from $200
        boot = read(0, 0x1000)[0x200:]     # (security block, IP) to $FF0000; the IP copies a routine
        write_image(out, 'IP', bytes(0x100) + boot, 0xFF0000, 0, 0, 0xFF0000)          # of its own to $FF1000
        m = re.search(rb'\x41\xFA(..)\x43\xF9\x00\xFF\x10\x00', boot, re.S)      # lea src(pc),a0; lea $FF1000,a1
        if m:                                                    # (where the IP starts depends on the region)
            write_image(out, 'IP_1000', bytes(0x100) + boot[m.start() + 2 + struct.unpack('>h', m.group(1))[0]:],
                        0xFF1000, 0, 0, 0xFF1000)
    if rows:
        open(os.path.join(out, 'mmd.tsv'), 'w').write('name\tload\tentry\thint\tvint\tflags\tsize\n' + '\n'.join(rows) + '\n')
        print(f'{len(rows)} .MMD programs -> {out}/mmd.tsv')


if __name__ == '__main__':
    main()
