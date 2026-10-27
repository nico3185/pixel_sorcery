#!/usr/bin/env python3
"""code_refs.py ROM OUT ROMUSE.bin...: the PC-relative ties of executed code, for zonepack.py --refs.

Each line "SOURCE TARGET" (hex): a branch or a d16(pc) operand of an instruction that ran (the
exec half of PS_ROMUSE captures), and each instruction's fall-through to the next one (unless it
ends the flow: rts/rte/rtr/jmp/bra). Moving code in pieces keeps these pairs in one piece.
"""
import re, sys
import capstone

BRANCH = re.compile(r'^(b(?!tst|set|clr|chg)[a-z]{1,3}|db[a-z]{1,2})(\.[bwsl])?$')


def main():
    rom = open(sys.argv[1], 'rb').read()
    n = len(rom); nb = n // 8 + 1
    ex = bytearray(nb)
    files = [x for k, x in enumerate(sys.argv[3:]) if x != '--funcs' and sys.argv[3 + k - 1] != '--funcs']
    for f in files:
        raw = open(f, 'rb').read()
        for i, x in enumerate(raw[:nb]):
            ex[i] |= x
    md = capstone.Cs(capstone.CS_ARCH_M68K, capstone.CS_MODE_BIG_ENDIAN | capstone.CS_MODE_M68K_000)
    out = []
    starts = [a for a in range(0, n, 2) if ex[a >> 3] >> (a & 7) & 1]
    # --funcs FILE (relocate.py --funcs-out: LO-HI bodies of functions that ran): their paths a run
    # did not take are decoded linearly too - a branch there must not cross a cut either
    if '--funcs' in sys.argv:
        seen = set(starts)
        for l in open(sys.argv[sys.argv.index('--funcs') + 1]):
            if not l.strip():
                continue
            lo, hi = (int(v, 16) for v in l.split('-'))
            for i in md.disasm(rom[lo:hi], lo):
                if i.address not in seen:
                    starts.append(i.address); seen.add(i.address)
        starts.sort()
    for a in starts:
        for i in md.disasm(rom[a:a + 10], a):
            t = [int(v, 16) for v in re.findall(r'\$([0-9a-f]+)\(pc', i.op_str)]
            if BRANCH.match(i.mnemonic):
                t += [int(v, 16) for v in re.findall(r'\$([0-9a-f]+)$', i.op_str)]
            out += [(a, x) for x in t if x < n]
            # tables code names (lea T(pc) / T.l): a table of word offsets from itself keeps its
            # entries' targets at their distance - tied over its whole extent
            named = [int(v, 16) for v in re.findall(r'\$([0-9a-f]+)(?:\(pc|\.l)', i.op_str)]
            if i.mnemonic.endswith('.l') or i.mnemonic.split('.')[0] in ('lea', 'pea'):
                named += [int(v, 16) for v in re.findall(r'#\$([0-9a-f]+)', i.op_str)]
            for tb in named:
                if tb & 1 or not 0x200 <= tb < n - 2:
                    continue
                w0 = int.from_bytes(rom[tb:tb + 2], 'big')
                if 2 <= w0 <= 0x400 and not w0 & 1:
                    offs = [int.from_bytes(rom[tb + 2 * k:tb + 2 * k + 2], 'big') for k in range(w0 // 2)]
                    if all(not o & 1 and w0 <= o < 0x4000 for o in offs):
                        out.append((tb, tb + max(offs)))
            # offset tables: "move.w T(pc,dn),d1; jmp T(pc,d1)" - entries are word offsets from T,
            # so T and every routine it reaches keep their distance (entries read while they land
            # on executed code; a few misses are tolerated, tables can hold unused slots)
            for tb in (int(v, 16) for v in re.findall(r'\$([0-9a-f]+)\(pc, [ad]\d\.[wl]\)', i.op_str)):
                miss = 0
                for k in range(0, 0x200, 2):
                    if tb + k + 2 > n:
                        break
                    o = int.from_bytes(rom[tb + k:tb + k + 2], 'big', signed=True)
                    tgt = tb + o
                    if 0 <= tgt < n and not tgt & 1 and ex[tgt >> 3] >> (tgt & 7) & 1:
                        out.append((tb, tgt)); miss = 0
                    else:
                        miss += 1
                        if miss > 3:
                            break
            if i.mnemonic.split('.')[0] not in ('rts', 'rte', 'rtr', 'jmp', 'bra'):
                out.append((a, a + i.size))
            break
    open(sys.argv[2], 'w').write(''.join(f'{s:06X} {t:06X}\n' for s, t in out))
    print(f'{sys.argv[2]}: {len(out)} ties')


if __name__ == '__main__':
    main()
