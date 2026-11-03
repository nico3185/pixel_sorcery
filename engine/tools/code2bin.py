#!/usr/bin/env python3
"""code2bin.py TITLE: the true round trip (roadmap 1.1, owner "bin2code and code2bin") - the ROM
rebuilt from the generated output alone, then compared with the player's ROM.

Reads only games/TITLE/src/recomp: every converted instruction's address (ITRACE) and its assembly
(the "asm:" comment generate.py writes beside its C), and rom_rest.bin (the bytes no instruction was
converted from). Each instruction is re-encoded by m68k-elf-as (as roundtrip.py does: explicit sizes,
PC-relative displacements; overlapping decodings in separate layers) and written over rom_rest.bin.
The ROM is read only at the end, for the comparison: identical by SHA-1, or the differing
instructions classified (bits a 68000 ignores = data decoded as code; assembler encoding choices).
Writes games/TITLE/build/code2bin.json.
"""
import glob
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import roundtrip   # noqa: E402  (to_gas: capstone text -> GNU as)

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LINE = re.compile(r'ITRACE\(0x([0-9A-Fa-f]+)\);.*/\* asm: (.*?) \*/')
LST = re.compile(r'^\s*(\d+)\s+([0-9a-f]{4})\s+((?:[0-9a-f]{4})+)\s')


def assemble(insns):
    """{addr: bytes} for [(addr, text)]: each layer assembled to a binary image, each instruction's
    length read by decoding that image (our own output, not the ROM); a line the assembler refuses
    (68020 forms) is left out, one that would move .org backwards (it starts inside another
    instruction) goes to the next layer"""
    from capstone import Cs, CS_ARCH_M68K, CS_MODE_M68K_000
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'recompiler'))
    import m68k_decode
    md = Cs(CS_ARCH_M68K, CS_MODE_M68K_000)
    out, pending = {}, sorted(insns)
    def gas(a, t):
        t = t[:-3].strip() if t.endswith('!ea') else t
        mn, _, ops = t.partition(' ')
        return roundtrip.to_gas(a, mn, ops.strip())
    with tempfile.TemporaryDirectory() as d:
        src, obj, binf = (os.path.join(d, n) for n in ('c.s', 'c.o', 'c.bin'))
        while pending:
            layer, later = list(pending), []
            while True:
                with open(src, 'w') as f:
                    f.write('.text\n')
                    for a, t in layer:
                        f.write(f'.org 0x{a:x}\n{gas(a, t)}\n')
                r = subprocess.run([roundtrip.AS, '-m68000', '--register-prefix-optional', '-o', obj, src],
                                   capture_output=True, text=True)
                errs = [(int(m.group(1)), m.group(2)) for m in re.finditer(r'c\.s:(\d+): Error: (.*)', r.stderr)]
                if not errs:
                    break
                drop = set()
                for ln, msg in errs:
                    k = (ln - 2) // 2                     # .org line 2k+2, instruction 2k+3
                    if 0 <= k < len(layer):
                        if 'backwards' in msg: later.append(layer[k])
                        drop.add(k)
                layer = [x for i, x in enumerate(layer) if i not in drop]
            subprocess.run([roundtrip.OBJCOPY, '-O', 'binary', obj, binf], check=True)
            img = open(binf, 'rb').read()
            for a, t in layer:
                dec = m68k_decode.decode(md, img[a:a + 10], a)
                if not dec: continue
                b = bytearray(img[a:a + dec[2]])
                if t.endswith('!ea'):                    # the EA form: OR/SUB/CMP/AND/ADD <ea>,Dn with ea = #imm
                    mn = t.split()[0]
                    base = {'or': 0x8000, 'sub': 0x9000, 'cmp': 0xB000, 'and': 0xC000, 'add': 0xD000}[mn.split('.')[0]]
                    sz = {'b': 0, 'w': 1, 'l': 2}[mn.split('.')[1]]
                    reg = int(re.search(r'd([0-7])\s*$', t[:-3].strip()).group(1))
                    opw = base | reg << 9 | sz << 6 | 0x3C
                    b[0], b[1] = opw >> 8, opw & 0xFF
                out[a] = bytes(b)
            if len(later) >= len(pending): break
            pending = sorted(later)
    return out


def main():
    t = sys.argv[1]
    g = os.path.join(REPO, 'games', t)
    src = os.path.join(g, 'src', 'recomp')
    insns = {}
    for f in glob.glob(os.path.join(src, '*.c')):
        for m in LINE.finditer(open(f, errors='replace').read()):
            insns.setdefault(int(m.group(1), 16), m.group(2).strip())
    rest = bytearray(open(os.path.join(src, 'rom_rest.bin'), 'rb').read())
    enc = assemble(list(insns.items()))
    img = bytearray(rest)
    for a, b in enc.items():
        img[a:a + len(b)] = b
    # the comparison: the player's ROM, read only now
    cfg = json.load(open(os.path.join(g, 'sidebyside.json')))
    roms = os.environ.get('PS_ROMS', os.path.join(os.path.dirname(REPO), 'roms'))
    rom = open(os.path.join(roms, cfg['rom']), 'rb').read()
    same = bytes(img) == rom
    diff = [a for a, b in enc.items() if rom[a:a + len(b)] != b]
    def ignored(a):                                    # only bits a 68000 ignores differ (data decoded as code)
        b, r = enc[a], rom[a:a + len(enc[a])]
        for i in range(0, len(b) - 1, 2):
            v = (b[i] ^ r[i]) << 8 | (b[i + 1] ^ r[i + 1])
            if v and v & ~0x0700 and not (i == 2 and v & ~0xFF00 == 0):
                return False
        return True
    data_like = [a for a in diff if ignored(a)]
    other = [a for a in diff if a not in set(data_like)]
    h = hashlib.sha1(img).hexdigest()
    print(f'{t}: {len(insns)} instructions from the generated C, {len(enc)} re-encoded, {len(insns) - len(enc)} refused '
          f'(68020 forms: data); ROM rebuilt from the generated output: sha1 {h} - '
          + ('IDENTICAL to the ROM' if same else f'differs ({len(diff)} instructions: {len(data_like)} bits a 68000 ignores, '
             f'{len(other)} other)'))
    for a in other[:8]:
        print(f'  ${a:06X} {insns[a]:40s} rom {rom[a:a + len(enc[a])].hex()} built {enc[a].hex()}')
    os.makedirs(os.path.join(g, 'build'), exist_ok=True)
    json.dump({'instructions': len(insns), 'encoded': len(enc), 'identical': same, 'sha1': h,
               'differ_data_like': ['%06X' % a for a in data_like], 'differ_other': ['%06X' % a for a in other]},
              open(os.path.join(g, 'build', 'code2bin.json'), 'w'), indent=0)
    return 0 if same else 1


if __name__ == '__main__':
    sys.exit(main())
