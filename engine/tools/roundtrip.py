#!/usr/bin/env python3
"""roundtrip.py TITLE: the ROM round trip (roadmap 0.1.3) - ROM -> generated code -> ROM, byte for byte.

Every instruction the generated C contains (its ITRACE addresses, read as kpi.py reads them) is
decoded as the generator decodes it (capstone), written back as assembly at its own address (.org)
and re-encoded by an assembler with nothing optimised (m68k-elf-as: explicit sizes kept, PC-relative
operands turned back into displacements); every other byte is carried as data. The result is
compared with the ROM: an instruction whose bytes differ is either decoded wrongly or data decoded as
code - the round-trip gap. Prints the totals and the first differences; writes
games/TITLE/build/roundtrip.json (addresses, git-ignored with the build).
"""
import json
import os
import re
import subprocess
import sys
import tempfile

from capstone import Cs, CS_ARCH_M68K, CS_MODE_M68K_000

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'recompiler'))
import kpi   # noqa: E402
import m68k_decode   # noqa: E402  (the generator's decoder: capstone fixed to what a 68000 executes)

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
AS = os.environ.get('M68K_AS', 'm68k-elf-as')
OBJCOPY = os.environ.get('M68K_OBJCOPY', 'm68k-elf-objcopy')


def hexnum(m):
    return '0x' + m.group(1)


def to_gas(addr, mnem, ops):
    """capstone text -> GNU as (Motorola syntax, register prefix optional)"""
    # PC-relative: capstone prints the target address; GNU as wants the displacement from the
    # extension word (addr + 2)
    def pcrel(m):
        disp = int(m.group(1), 16) - (addr + 2)
        idx = m.group(2)
        return f'({disp},pc{"," + idx.strip() if idx else ""})'
    ops = re.sub(r'\$([0-9a-fA-F]+)\(pc(?:,\s*([^)]+))?\)', pcrel, ops)
    ops = re.sub(r'\$([0-9a-fA-F]+)', hexnum, ops)
    mnem = re.sub(r'^(b\w\w|bra|bsr)\.b$', r'\1.s', mnem)     # short branches: GNU spells them .s
    ops = re.sub(r'(?<![\w.])0x0\((a[0-7]|sp)\)', r'(0.w,\1)', ops)   # an explicit zero d16: as would drop it
    if re.match(r'^(b\w\w|bra|bsr|db\w+)(\.\w)?$', mnem):        # branch targets relative to here, resolved by as
        ops = re.sub(r'0x([0-9a-fA-F]+)$', lambda m: '.%+d' % (int(m.group(1), 16) - addr), ops)
    mnem = re.sub(r'^(exg|abcd|sbcd|nbcd)\.[bwl]$', r'\1', mnem)   # sizes GNU as does not take
    if mnem == 'moveq':                                         # capstone prints the byte unsigned
        ops = re.sub(r'#0x([0-9a-fA-F]+)', lambda m: '#%d' % (int(m.group(1), 16) - (256 if int(m.group(1), 16) > 127 else 0)), ops)
    return f'{mnem} {ops}'.rstrip()


def main():
    t = sys.argv[1]
    g = os.path.join(REPO, 'games', t)
    cfg = json.load(open(os.path.join(g, 'sidebyside.json')))
    roms = os.environ.get('PS_ROMS', os.path.join(os.path.dirname(REPO), 'roms'))
    rom = open(os.path.join(roms, cfg['rom']), 'rb').read()
    conv, _ = kpi.generated(g, len(rom))
    md = Cs(CS_ARCH_M68K, CS_MODE_M68K_000)
    insns, undecodable = [], []
    for a in sorted(conv):
        dec = m68k_decode.decode(md, rom[a:a + 10], a)
        if dec is not None and a + 10 > len(rom):   # near the end: its true length, decoded with padding
            full = m68k_decode.decode(md, (rom[a:a + 10] + b'\xff' * 10)[:10], a)
            if full is None or a + full[2] > len(rom):
                dec = None
        if dec is None:                              # one running past the ROM's end is data
            undecodable.append(a)
            undecodable.append(a)
            continue
        mnem, ops, n, norm = dec
        insns.append((a, n, to_gas(a, mnem, ops), norm))
    # instructions that overlap (one decoded inside another's bytes: both can't be code) go to
    # separate layers, each assembled on its own: every instruction at its own address
    layers, ends = [], []
    for k, (a, n, line, norm) in enumerate(insns):
        j = next((j for j, e in enumerate(ends) if e <= a), None)
        if j is None:
            layers.append([]); ends.append(0); j = len(layers) - 1
        layers[j].append(k); ends[j] = a + n
    overlapping = sum(len(l) for l in layers[1:])
    asm_of, bad_lines = {}, {}
    with tempfile.TemporaryDirectory() as d:
        src, obj, binf = os.path.join(d, 'rt.s'), os.path.join(d, 'rt.o'), os.path.join(d, 'rt.bin')
        for layer in layers:
            def assemble(skip):
                with open(src, 'w') as f:
                    f.write('.text\n')
                    for k in layer:
                        a, n, line, _ = insns[k]
                        f.write(f'.org 0x{a:x}\n' + ('' if k in skip else line) + '\n')
                return subprocess.run([AS, '-m68000', '--register-prefix-optional', '-o', obj, src],
                                      capture_output=True, text=True)
            # pass 1: the lines the 68000 assembler refuses (data decoded as code: 68020 modes, ...)
            for m in re.finditer(r'rt\.s:(\d+): Error: (.*)', assemble(set()).stderr):
                bad_lines[layer[(int(m.group(1)) - 3) // 2]] = m.group(2)
            r = assemble(set(bad_lines))       # pass 2: everything else
            if r.returncode:
                print(r.stderr[:2000]); return 2
            subprocess.run([OBJCOPY, '-O', 'binary', obj, binf], check=True)
            asm = open(binf, 'rb').read()
            for k in layer:
                a, n, _, _ = insns[k]
                asm_of[k] = asm[a:a + n]
    out = bytearray(rom)                       # every byte not re-encoded is carried as data
    diff, rejected = [], []
    for k, (a, n, line, norm) in enumerate(insns):
        if k in bad_lines:
            rejected.append((a, line, bad_lines[k])); continue
        if asm_of[k] != rom[a:a + n]:
            diff.append((a, line, rom[a:a + n].hex(), asm_of[k].hex(), asm_of[k] == norm))
        out[a:a + n] = asm_of[k]
    exact = out == rom
    # re-encoded to the decoder's bytes but not the ROM's: only bits a 68000 ignores differ (a byte
    # immediate's high byte, index scale bits) - an encoding no assembler writes: data decoded as code
    def ignored_only(x):                       # every differing word differs only in bits a 68000 ignores
        w, g2 = bytes.fromhex(x[2]), bytes.fromhex(x[3])
        if len(w) != len(g2):
            return False
        for i in range(0, len(w) - 1, 2):
            v = (w[i] ^ g2[i]) << 8 | (w[i + 1] ^ g2[i + 1])
            if v and not (v & ~0x0700 == 0 or (i == 2 and v & ~0xFF00 == 0 and re.search(r'\.b\s.*#', x[1]))):
                return False
        return True
    data_like = [x for x in diff if x[4] or ignored_only(x)]
    # ADD/SUB/CMP/AND/OR #imm,Dn written with the immediate as the source operand: GNU as always
    # picks ADDI/SUBI/... (same effect, other opcode) - an encoding choice, kept apart
    choice = [x for x in diff if x not in data_like and re.match(r'(add|sub|cmp|and|or)\.[bwl] #', x[1])]
    data_like_ids = {id(x) for x in data_like + choice}
    other = [x for x in diff if id(x) not in data_like_ids]
    print(f'{t}: {len(insns)} instructions re-encoded, {len(insns) - len(diff) - len(rejected)} byte-exact, '
          f'{len(diff)} differ, {len(rejected)} not 68000 code, {len(undecodable)} undecodable, {overlapping} overlap '
          f'another; ROM '
          + ('identical' if exact else f'differs in {sum(1 for x, y in zip(out, rom) if x != y)} bytes'))
    print(f'  data decoded as code: {len(data_like)} with bits a 68000 ignores set, {len(rejected)} '
          f'68020-only forms, {overlapping} overlapping; {len(choice)} assembler encoding choices; to look at: {len(other)}')
    for a, line, want, got, _ in other[:12]:
        print(f'  ${a:06X} {line:40s} rom {want}  asm {got}')
    for a, line, why in rejected[:8]:
        print(f'  ${a:06X} {line:40s} not assembled: {why}')
    os.makedirs(os.path.join(g, 'build'), exist_ok=True)
    e2e = None
    if '--e2e' in sys.argv:
        e2e = whole_rom(g, rom, insns, layers[0] if layers else [], {k for k in range(len(insns))
                        if k in bad_lines or asm_of[k] != rom[insns[k][0]:insns[k][0] + insns[k][1]]})
        if e2e is None:
            return 2
    json.dump({'instructions': len(insns), 'exact': len(insns) - len(diff) - len(rejected),
               'data_like': len(data_like), 'other': ['$%06X' % a for a, *_ in other], 'differ': ['$%06X' % a for a, *_ in diff],
               'not_assembled': ['$%06X' % a for a, *_ in rejected], 'undecodable': ['$%06X' % a for a in undecodable],
               'overlapping': overlapping, 'rom_identical': exact, 'e2e': e2e},
              open(os.path.join(g, 'build', 'roundtrip.json'), 'w'), indent=1)
    return 0 if e2e is None or e2e['identical'] else 1


def whole_rom(g, rom, insns, layer, as_data):
    """--e2e: the whole ROM as ONE assembly source (build/roundtrip.s, git-ignored: ROM-derived) -
    every converted instruction that re-encodes to its own bytes written as an instruction, the
    ones that don't (data decoded as code, encoding choices) and every other byte as dc - assembled
    and linked from nothing but that file, compared with the ROM by size and SHA-1"""
    import hashlib
    src = os.path.join(g, 'build', 'roundtrip.s')
    code = words = 0
    with open(src, 'w') as f:
        f.write('| ROM rebuilt from the generator\'s decoding (roundtrip.py --e2e)\n.text\n')
        pos = 0
        def data(a, b):
            for i in range(a, b, 16):
                f.write('dc.b ' + ','.join('0x%02x' % x for x in rom[i:min(i + 16, b)]) + '\n')
        for k in layer:                        # layer 0: address order, no overlap
            a, n, line, _ = insns[k]
            data(pos, a)
            if k in as_data:
                f.write('dc.w ' + ','.join('0x%04x' % int.from_bytes(rom[i:i + 2], 'big') for i in range(a, a + n, 2))
                        + f'  | {line}\n')
                words += 1
            else:
                f.write(line + '\n'); code += 1
            pos = a + n
        data(pos, len(rom))
    with tempfile.TemporaryDirectory() as d:
        obj, binf = os.path.join(d, 'rom.o'), os.path.join(d, 'rom.bin')
        r = subprocess.run([AS, '-m68000', '--register-prefix-optional', '-o', obj, src], capture_output=True, text=True)
        if r.returncode:
            print(r.stderr[:2000]); return None
        subprocess.run([OBJCOPY, '-O', 'binary', obj, binf], check=True)
        out = open(binf, 'rb').read()
    same = out == rom
    h = hashlib.sha1(out).hexdigest()
    print(f'  e2e: {os.path.relpath(src, REPO)} -> {len(out)} bytes, sha1 {h} - '
          + ('IDENTICAL to the ROM' if same else f'differs from the ROM (sha1 {hashlib.sha1(rom).hexdigest()})')
          + f'; {code} instructions as code, {words} as words (data decoded as code, encoding choices)')
    return {'identical': same, 'sha1': h, 'code': code, 'words': words}


if __name__ == '__main__':
    sys.exit(main())
