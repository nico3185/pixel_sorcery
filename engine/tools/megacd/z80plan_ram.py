#!/usr/bin/env python3
"""z80plan_ram.py DUMP WINDOWS FIXED_OUT PATCH_OUT --table OFF:COUNT ... ROMUSE.bin...: a sound
driver whose bank selects write a whole 8-bit bank number (A15-A22, "ld a,n / ld hl,$6000 /
ld (hl),a / rra ..." or a value from a table) moved to word RAM, patched in Z80 RAM.

For drivers the game unpacks into Z80 RAM (SHK: Kosinski at boot), the patches
cannot go into the ROM: PATCH_OUT lists "OFFSET VALUE" bytes (Z80 RAM) that games/z80ram.s
writes after the game's own loader. DUMP: Z80 RAM as the driver starts (tracer PS_Z80_DUMP).
Banks the driver reads (PS_ROMUSE_Z80ONLY captures) keep their offsets inside the bank; banks
whose used offsets do not collide share a window (WINDOWS: new bank numbers, hex, best first).
Tables (--table OFF:COUNT) hold one bank number per song / sample; constants are "ld a,n"
right before "ld hl,$6000" (not the operand of a 3-byte load). Banks a capture never saw keep
their value (lock-on only, unreachable).
"""
import sys


def main():
    a = sys.argv[1:]
    z = open(a[0], 'rb').read()
    windows = [int(w, 16) for w in a[1].split(',')]
    tables, caps, rest = [], [], a[4:]
    i = 0
    while i < len(rest):
        if rest[i] == '--table':
            tables.append(tuple(int(v, 16) for v in rest[i + 1].split(':'))); i += 2
        else:
            caps.append(rest[i]); i += 1
    used = set()
    for f in caps:
        raw = open(f, 'rb').read()
        nb = (len(raw) // 2)
        for i, x in enumerate(raw[nb:2 * nb]):
            if x:
                for k in range(8):
                    if x >> k & 1:
                        used.add(i * 8 + k)
    banks = {}
    for v in used:
        banks.setdefault(v >> 15, []).append(v & 0x7FFF)
    ivs = {}
    for b, offs in banks.items():
        offs.sort(); r = []
        for o in offs:
            if r and o <= r[-1][1] + 0x40:
                r[-1][1] = o + 1
            else:
                r.append([o, o + 1])
        ivs[b] = r
    occ = {w: [] for w in windows}
    assign = {}
    for b in sorted(ivs, key=lambda b: -sum(h - l for l, h in ivs[b])):
        for w in windows:
            if all(h <= l2 or l >= h2 for l, h in ivs[b] for l2, h2 in occ[w]):
                assign[b] = w; occ[w] += ivs[b]
                break
        else:
            sys.exit(f'bank {b:02X}: no free window')
    fixed = [f'{(b << 15) + l:06X}-{(b << 15) + h:06X}={(w << 15) + l:06X}'
             for b, w in assign.items() for l, h in ivs[b]]
    patches = []
    for off, count in tables:
        for k in range(count):
            v = z[off + k]
            if v in assign:
                patches.append((off + k, assign[v]))
    for i in range(1, 0x1FFB):
        if z[i] == 0x3E and z[i + 2:i + 5] == b'\x21\x00\x60' and z[i - 1] not in (0x3A, 0x32):
            if z[i + 1] in assign:
                patches.append((i + 1, assign[z[i + 1]]))
    open(a[2], 'w').write('\n'.join(fixed) + '\n')
    open(a[3], 'w').write(''.join(f'{o:04X} {v:02X}\n' for o, v in patches))
    print('banks -> windows:', {f'{b:02X}': f'{w:02X}' for b, w in sorted(assign.items())},
          f'{len(used) // 1024} KB, {len(patches)} Z80 RAM patches')


if __name__ == '__main__':
    main()
