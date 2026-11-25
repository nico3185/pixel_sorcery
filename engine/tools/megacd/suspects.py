#!/usr/bin/env python3
"""suspects.py TRACER ROM WORKDIR IMAGE.bin PIECE.polls: the wrong guesses of a disc image that a play reaches.

An image's code and data are rewritten where the relocation decided a long is an address: from evidence (a run
read it as a pointer) or by rule where no run went (--scan-tables: a long naming an offset table; --all-code: an
operand of code the analyzer decoded). A rule can mistake data for an address (SHK: Knuckles' move
tables at $19B5C/$19D28 - "0003 0018" became "0022 5D18" - he ended a pixel off). PIECE (the owner's play, as the
cartridge needs it) is run on the cartridge with the tracer; the bytes it reads that no build run read (WORKDIR's
cpu/ and dma/ captures) and that IMAGE.bin (relocate.py's output: the ROM's layout, rewritten) holds differently
from ROM are printed: each a long the relocation guessed. Code bytes (operands) are listed apart."""
import glob, os, subprocess, sys, tempfile


def bits(f, n):
    raw = open(f, 'rb').read(); h = len(raw) // 2
    ex, rd = raw[:h], raw[h:]
    return ex[:n // 8 + 1], rd[:n // 8 + 1]


def main():
    tr, rom_p, work, img_p, piece = sys.argv[1:6]
    rom = open(rom_p, 'rb').read(); img = open(img_p, 'rb').read(); n = len(rom)
    t = tempfile.mkdtemp(prefix='sus')
    subprocess.run([tr, '--headless', '--frames', str(sum(1 for _ in open(piece)) * 2 + 600), '--trace-out', '/dev/null', rom_p],
                   env={**os.environ, 'PS_ROMUSE': t, 'PS_POLL_REPLAY': piece},
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    pr, px = bytearray(n // 8 + 1), bytearray(n // 8 + 1)
    for f in glob.glob(t + '/*.bin'):
        ex, rd = bits(f, n)
        for i, x in enumerate(rd): pr[i] |= x
        for i, x in enumerate(ex): px[i] |= x
    er = bytearray(n // 8 + 1)
    for f in glob.glob(os.path.join(work, 'cpu', '*', '*.bin')) + glob.glob(os.path.join(work, 'dma', '*', '*.bin')):
        _, rd = bits(f, n)
        for i, x in enumerate(rd): er[i] |= x
    on = lambda b, a: b[a >> 3] >> (a & 7) & 1
    data, code = [], []
    for a in range(0x200, min(n, len(img))):
        if img[a] != rom[a] and (on(pr, a) or on(px, a)) and not on(er, a):
            (code if on(px, a) else data).append(a)
    # each rewritten long: a real pointer is followed - the play reads (or runs, or DMAs) what it names; a long
    # whose target the play never touches was data the rule took for an address
    seen, out = set(), []
    for a in data + code:
        for p in range(a - 3 + ((a - 3) & 1), a + 1, 2):
            if p in seen or p < 0x200 or img[p:p + 4] == rom[p:p + 4]: continue
            v = int.from_bytes(rom[p:p + 4], 'big')
            if not (0x200 <= v < n) or v & 1: continue
            seen.add(p)
            used = any(on(pr, x) or on(px, x) for x in range(v, min(n, v + 16)))
            if not used: out.append((p, v, 'code' if on(px, p) else 'data'))
    print(f'{len(out)} rewritten long(s) this play reads but never follows - data taken for an address:')
    for p, v, k in out:
        print(f'  {p:06X} ({k}): rom {rom[p:p + 4].hex()}  image {img[p:p + 4].hex()}')


if __name__ == '__main__':
    main()
