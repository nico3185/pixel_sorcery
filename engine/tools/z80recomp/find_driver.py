#!/usr/bin/env python3
"""find_driver.py ROM STATE.bin Z80TRACE [--name N] > driver.json
   find_driver.py --capture DUMP PCS [--name N] > driver.json   (driver unpacked by the 68K)

Describe a game's Z80 sound driver for z80recomp without reading the 68K upload code:
  - Z80 RAM is taken from a ps_poll state snapshot (68K RAM 64K | Z80 RAM 8K | ...)
    while the driver runs; every run of >= 64 bytes found verbatim in the ROM becomes an
    upload block (ROM offset -> Z80 address)
  - the code range is bounded by the Z80 PCs executed in the tracer's PS_Z80_TRACE
Bytes the driver rewrote while running (work RAM) are simply not matched, as they should be.
"""
import json, re, sys

if sys.argv[1] == '--capture':
    # find_driver.py --capture DUMP PCS [--name N]: a driver the 68K unpacks (SH2) is not
    # in the ROM verbatim. DUMP is Z80 RAM as it starts (tracer PS_Z80_DUMP), PCS the bitmap of
    # executed Z80 addresses (PS_Z80_PCS); recomp.sh recaptures the image the same way.
    dump = open(sys.argv[2], 'rb').read()
    bits = open(sys.argv[3], 'rb').read()
    name = sys.argv[sys.argv.index('--name') + 1] if '--name' in sys.argv else 'game'
    pcs = [i for i in range(0x2000) if bits[i >> 3] >> (i & 7) & 1]
    end = max(pcs)
    while end + 1 < 0x2000:                        # to the end of the uploaded bytes (a short
        run = next((k for k in range(1, 9) if dump[end + k] if end + k < 0x2000), 0)   # zero run
        if not run:                                  # is NOPs/operands, 8+ zeros end it)
            break
        end += run
    print(json.dumps({"comment": "found by find_driver.py --capture: Z80 RAM captured as the driver starts "
                      "(unpacked by the 68K); code range from executed Z80 PCs to the end of the upload",
                      "name": name, "capture": True, "code": [f"0x0000-0x{end:04X}"]}, indent=2))
    print(f'{len(pcs)} executed Z80 addresses, code up to ${end:04X}', file=sys.stderr)
    sys.exit(0)

rom = open(sys.argv[1], 'rb').read()
zram = open(sys.argv[2], 'rb').read()[0x10000:0x12000]
name = sys.argv[sys.argv.index('--name') + 1] if '--name' in sys.argv else 'game'

pcs = set()
for line in open(sys.argv[3]):
    m = re.match(r'(?:\d+ )?([0-9A-F]{4}) ', line)
    if m:
        pcs.add(int(m.group(1), 16))
pcs = {p for p in pcs if p < 0x2000}

blocks, z = [], 0
while z < 0x2000:
    best = None
    if any(zram[z:z + 16]):
        pat = zram[z:z + 16]
        start = 0
        while True:
            r = rom.find(pat, start)
            if r < 0:
                break
            n = 16
            while z + n < 0x2000 and r + n < len(rom) and rom[r + n] == zram[z + n]:
                n += 1
            if not best or n > best[2]:
                best = (r, z, n)
            start = r + 1
    if best:
        blocks.append(best)
        z += best[2]
    else:
        z += 1

blocks = [b for b in blocks if b[2] >= 64]      # shorter runs are chance matches in work RAM
code_end = max(pcs) + 4 if pcs else 0
# the whole uploaded block the executed code is in: a trace only sees the paths it ran,
# and code just past them (QS $0DC4, WOI $0CF5) was missed
for r, z, n in blocks:
    if pcs and z <= max(pcs) < z + n:
        code_end = max(code_end, z + n - 1)
out = {
    "comment": "found by find_driver.py: Z80 RAM blocks located in the ROM; code range from executed Z80 PCs",
    "name": name,
    "uploads": [{"rom": f"0x{r:06X}", "z80": f"0x{z:04X}", "len": f"0x{n:04X}"} for r, z, n in blocks],
    "code": [f"0x0000-0x{min(code_end, 0x1FFF):04X}"],
}
print(json.dumps(out, indent=2))
print(f'{len(blocks)} blocks, {sum(b[2] for b in blocks)} bytes matched, {len(pcs)} executed Z80 addresses, '
      f'code up to ${code_end:04X}', file=sys.stderr)
