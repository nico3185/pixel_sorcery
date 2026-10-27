#!/usr/bin/env python3
"""export_ips.py GAMEDIR ROM [NAME ...] [-o OUT]: a title's cheats and RAM codes for any emulator.

Writes OUT.ips (default <title>.ips beside the ROM's folder in the current directory) with the
ROM patches - recomp.json "cheats" and the ROM toggles of src/main.c (ps_menu_add_toggle with a
ROM address) - and the header checksum ($18E) recomputed, so games that verify it still boot.
Also writes OUT.txt: every code in Pro Action Replay form (AAAAAA:VVVV), RAM toggles included,
which no ROM patch can express (they are rewritten every frame; emulators' cheat lists do that).
NAME limits the export to those cheats/toggles (case-insensitive); default: all of them.
Patches the user's own ROM; nothing ROM-derived is stored in the repository.
"""
import json
import os
import re
import sys


def main():
    args = sys.argv[1:]
    out = None
    if '-o' in args:
        i = args.index('-o'); out = args[i + 1]; del args[i:i + 2]
    game, rom_path, names = args[0], args[1], [n.upper() for n in args[2:]]
    title = os.path.basename(os.path.normpath(game))
    out = out or title
    rom = bytearray(open(rom_path, 'rb').read())
    cfg = json.load(open(os.path.join(game, 'recomp.json')))

    codes = []                                   # (name, address, bytes)
    for name, sites in cfg.get('cheats', {}).items():
        for a, hexv in sites.items():
            codes.append((name.upper().replace('_', ' '), int(a, 16), bytes.fromhex(hexv)))
    main_c = os.path.join(game, 'src', 'main.c')
    if os.path.exists(main_c):
        for m in re.finditer(r'ps_menu_add_toggle\("([^"]+)",\s*(0x[0-9A-Fa-f]+),\s*(0x[0-9A-Fa-f]+),\s*(true|false)\)',
                             open(main_c).read()):
            n, a, v, word = m.group(1), int(m.group(2), 16), int(m.group(3), 16), m.group(4) == 'true'
            codes.append((n.upper(), a, v.to_bytes(2 if word else 1, 'big')))
    if names:
        codes = [c for c in codes if c[0] in names]
    if not codes:
        sys.exit(f'{title}: no cheats or toggles' + (f' named {names}' if names else ''))

    patched = bytearray(rom)
    for name, a, b in codes:
        if a < len(rom):
            patched[a:a + len(b)] = b
    rom_codes = [c for c in codes if c[1] < len(rom)]
    if rom_codes and len(rom) > 0x200:           # header checksum: sum of the words after $200
        s = sum(int.from_bytes(patched[i:i + 2], 'big') for i in range(0x200, len(patched) - 1, 2)) & 0xFFFF
        patched[0x18E:0x190] = s.to_bytes(2, 'big')

    if rom_codes:
        ips = bytearray(b'PATCH')
        i = 0
        while i < len(rom):                      # one record per run of changed bytes
            if patched[i] == rom[i]:
                i += 1; continue
            j = i
            while j < len(rom) and patched[j] != rom[j] and j - i < 0xFFFF:
                j += 1
            ips += i.to_bytes(3, 'big') + (j - i).to_bytes(2, 'big') + patched[i:j]
            i = j
        ips += b'EOF'
        open(out + '.ips', 'wb').write(ips)
        print(f'{out}.ips: {len(rom_codes)} ROM patch(es) + header checksum')
    with open(out + '.txt', 'w') as f:
        f.write(f'# {cfg.get("title", title)}: Pro Action Replay codes (address:value)\n')
        for name, a, b in codes:
            v = int.from_bytes(b, 'big')
            kind = 'ROM' if a < len(rom) else 'RAM'
            f.write(f'{a & 0xFFFFFF:06X}:{v:0{2 * len(b)}X}  # {name} ({kind})\n')
    print(f'{out}.txt: {len(codes)} code(s)')


if __name__ == '__main__':
    main()
