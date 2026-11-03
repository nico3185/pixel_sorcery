#!/usr/bin/env python3
"""disasm_map.py TITLE LISTING.lst[@OFFSET] [LISTING2.lst@OFFSET ...] [--rom ROM]: the complete code map of a title from a community
disassembly that reassembles to the player's ROM (roadmap 1.0, "shift gears": completeness proven
statically). Reads an AS listing (asl -L) and keeps only the ADDRESS of every line that assembles an
instruction (not dc/dcb/binclude/data) - no names, no source: the disassembly is a map, never read
into the generator. Each address is checked against the ROM (the listing's opcode word must be the ROM's,
else the line is dropped and counted). Then compares with the generated code (kpi.generated):
  - code the disassembly has and the generator never converted (the completeness gap),
  - converted instructions the disassembly calls data (data decoded as code),
and writes games/TITLE/build/disasm_map.json (addresses only, git-ignored with the build).
"""
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kpi   # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LINE = re.compile(r'^\s*(?:\(\d+\)\s*)?\d+/\s*([0-9A-F]+)\s*:\s((?:[0-9A-F]{4}\s?)+)\s+(.*)$')
DATA = re.compile(r'^\s*(?:[\w.@$]+:|[+\-/]+)?\s*!?(dc(\.[bwl])?|dcb(\.[bwl])?|binclude|incbin|even|align|ds(\.[bwl])?|rept|endr|cnop)\b', re.I)
# a label (name: or an anonymous +/-/... label), then the mnemonic, possibly forced with AS's ! prefix
CODE = re.compile(r'^\s*(?:[\w.@$]+:|[+\-/]+)?\s*!?([a-z]{2,8})(\.[bwls])?\b', re.I)
# the 68000's mnemonics (a macro line - sound-driver data, SMPS commands - is not an instruction)
M68K = set('''abcd add adda addi addq addx and andi asl asr bcc bcs beq bge bgt bhi ble bls blt bmi bne bpl bvc bvs
bchg bclr bra bset bsr btst chk clr cmp cmpa cmpi cmpm dbcc dbcs dbeq dbf dbra dbge dbgt dbhi dble dbls dblt dbmi dbne
dbpl dbt dbvc dbvs divs divu eor eori exg ext illegal jmp jsr lea link lsl lsr move movea movem movep moveq muls mulu
nbcd neg negx nop not or ori pea reset rol ror roxl roxr rte rtr rts sbcc scc scs seq sf sge sgt shi sle sls slt smi sne
spl st svc svs stop sub suba subi subq subx swap tas trap trapv tst unlk bhs blo'''.split())


def parse(listing, rom, offset=0):
    code, dropped = {}, 0
    for line in open(listing, encoding='latin-1'):
        m = LINE.match(line)
        if not m:
            continue
        addr, hexb, src = int(m.group(1), 16) + offset, m.group(2).split(), m.group(3)
        src = src.split(';', 1)[0]
        mm = CODE.match(src)
        if not src.strip() or DATA.match(src) or not mm or mm.group(1).lower() not in M68K or addr & 1:
            continue
        if addr >= len(rom):
            continue
        b = bytes.fromhex(''.join(hexb))
        # the opcode word must be the ROM's: some macro lines list an operand from an earlier pass
        # (SH1 "1+0(a1)" listed 0001, assembled 0000 - the assembled image is the ROM's)
        if rom[addr:addr + 2] != b[:2]:
            dropped += 1
            continue
        code.setdefault(addr, len(b))
    return code, dropped


def main():
    t = sys.argv[1]
    lists = [a for a in sys.argv[2:] if not a.startswith('--') and a != (sys.argv[sys.argv.index('--rom') + 1] if '--rom' in sys.argv else None)]
    g = os.path.join(REPO, 'games', t)
    cfg = json.load(open(os.path.join(g, 'sidebyside.json')))
    roms = os.environ.get('PS_ROMS', os.path.join(os.path.dirname(REPO), 'roms'))
    rom = open(sys.argv[sys.argv.index('--rom') + 1] if '--rom' in sys.argv else os.path.join(roms, cfg['rom']), 'rb').read()
    code, dropped = {}, 0                  # several listings: a lock-on title (SHK at 0, SH3 at $200000)
    for spec in lists:
        path, _, off = spec.partition('@')
        c, d = parse(path, rom, int(off, 16) if off else 0)
        code.update(c); dropped += d
    conv, entries = kpi.generated(g, len(rom))
    missing = sorted(a for a in code if a not in conv)
    extra = sorted(a for a in conv if a not in code)
    # group the missing instructions into runs (a run = code the generator never reached)
    runs, s, prev = [], None, None
    for a in missing:
        if s is None or a > prev + 16:
            if s is not None: runs.append((s, prev))
            s = a
        prev = a
    if s is not None: runs.append((s, prev))
    print(f'{t}: the disassembly has {len(code)} instructions (lines not matching the ROM dropped: {dropped}); '
          f'generated code converts {len(conv)}')
    print(f'  both: {len(code) - len(missing)} ({100 * (len(code) - len(missing)) / max(len(code), 1):.1f}% of the disassembly\'s code)')
    print(f'  disassembly code never converted: {len(missing)} instructions in {len(runs)} runs - the completeness gap')
    print(f'  converted but data in the disassembly: {len(extra)} (data decoded as code)')
    for r in sorted(runs, key=lambda r: r[0] - r[1])[:10]:
        n = sum(1 for a in missing if r[0] <= a <= r[1])
        print(f'    ${r[0]:06X}-${r[1]:06X}: {n} instructions')
    os.makedirs(os.path.join(g, 'build'), exist_ok=True)
    json.dump({'code_addrs': ['%06X' % a for a in sorted(code)], 'code': len(code), 'both': len(code) - len(missing), 'missing': ['%06X' % a for a in missing],
               'runs': [['%06X' % a, '%06X' % b] for a, b in runs], 'data_converted': ['%06X' % a for a in extra]},
              open(os.path.join(g, 'build', 'disasm_map.json'), 'w'), indent=0)
    return 0


if __name__ == '__main__':
    sys.exit(main())
