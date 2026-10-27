#!/usr/bin/env python3
"""z80plan_s3.py ROM DRIVER WINDOWS FIXED_OUT PATCH_OUT ROMUSE.bin...: the SH3 / SHK sound
driver's ROM banks moved into word RAM.

The Z80 reads music, sound effects and DAC samples through its 32 KB bank window. On a Mega CD
those banks must sit where the Z80 can always see them: word RAM (banks $40-$47 = $200000+).
Each original bank's used bytes (PS_ROMUSE_Z80ONLY captures) keep their offsets inside the bank;
banks whose used offsets do not collide share a window (WINDOWS: candidate new bank numbers, hex,
comma-separated, best first). Writes FIXED_OUT (zonepack.py --fixed lines) and PATCH_OUT
(relocate.py --patch values) for the driver at DRIVER (its ROM address; uploaded as it is):
  - DAC sample banks: a byte per sample at driver $00DC (8 address bits written);
  - music banks: a byte per song at $0B48, whose low nibble gives A15-A18 while each switch site
    writes A19-A23 as constants ("xor a / ld d,1 / ld (hl),d / ld (hl),a x4"): the sites are
    rewritten to set A21 instead (word RAM), the table gets the window number;
  - the sound-effect bank: constant 9-bit sequences ("xor a / ld e,1 / ld (hl),e|a x9") and the
    start-up switch ("ld a,$1F / ld b,8").
"""
import sys


def main():
    rom = open(sys.argv[1], 'rb').read()
    drv = int(sys.argv[2], 16)
    windows = [int(w, 16) for w in sys.argv[3].split(',')]
    n = len(rom); nb = n // 8 + 1
    used = set()
    for f in sys.argv[6:]:
        raw = open(f, 'rb').read()
        h = len(raw) // 2                      # half size from the file
        for i, x in enumerate(raw[h:h + min(nb, h)]):
            if x:
                for k in range(8):
                    if x >> k & 1:
                        used.add(i * 8 + k)
    banks = {}
    for a in used:
        banks.setdefault(a >> 15, []).append(a & 0x7FFF)
    ivs = {}
    for b, offs in banks.items():                # used intervals (merged across < 64 bytes)
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
    z = rom[drv:drv + 0x2000]
    for k in range(0x100):                       # DAC banks
        v = z[0xDC + k]
        if not 0x10 <= v <= 0x1F:
            break
        if v in assign:
            patches.append(f'{drv + 0xDC + k:X}={assign[v]:02X}')
    for k in range(0x80):                        # music banks (low nibble; sites set A19 now A21)
        v = z[0xB48 + k]
        if not 0x08 <= v <= 0x0F:
            break
        old = 0x10 | v
        if old in assign:
            w = assign[old]
            if w & ~0x47 or w & 0x08:
                sys.exit(f'window {w:02X} not reachable from the music switch (needs $40-$47)')
            patches.append(f'{drv + 0xB48 + k:X}={w & 7:02X}')
    p = -1
    while True:                                  # music switch sites: A19 constant -> A21
        p = z.find(bytes.fromhex('af160172'), p + 1)
        if p < 0:
            break
        patches.append(f'{drv + p + 4 - 1:X}=7777727777')
    sfx = 0x1F
    if sfx in assign:
        nbk = assign[sfx]
        seq = bytes(0x73 if nbk >> k & 1 else 0x77 for k in range(9))
        p = -1
        while True:                              # constant switches to the SFX bank
            p = z.find(bytes.fromhex('af1e01' + '73' * 5 + '77' * 4), p + 1)
            if p < 0:
                break
            patches.append(f'{drv + p + 3:X}={seq.hex().upper()}')
        p = z.find(bytes.fromhex('3e1f0608320060'))
        if p >= 0:
            patches.append(f'{drv + p + 1:X}={nbk:02X}')
    open(sys.argv[4], 'w').write('\n'.join(fixed) + '\n')
    open(sys.argv[5], 'w').write('\n'.join(patches) + '\n')
    print('banks -> windows:', {f'{b:02X}': f'{w:02X}' for b, w in sorted(assign.items())},
          f'{len(used) // 1024} KB, {len(patches)} driver patches')


if __name__ == '__main__':
    main()
