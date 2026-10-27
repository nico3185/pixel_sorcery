#!/usr/bin/env python3
"""check_seeds.py ROM recomp.json [--fix] — reject seeds that start inside an instruction.

A seed decoded from the middle of another function's instruction is garbage that some
run executed (e.g. after a bad poke); its function splits real code and the game then runs
the wrong bytes (COI: $01085A inside andi.b at $010856 broke the final boss).
A seed is bad when it lies strictly inside an instruction decoded from another seed and
no other seed reaches it as an instruction start. --fix removes bad seeds.
"""
import sys, json, capstone
rom=open(sys.argv[1],"rb").read(); seeds=sorted(int(k,16) for k in json.load(open(sys.argv[2]))['seeds'])
# a seed on blank ROM (all zero bytes: an unmirrored dump, e.g. QS-R1 .md whose
# $40000-$FFFFF banks the cartridge mirrors from bank 0) means the wrong ROM file, not bad seeds
blank=[s for s in seeds if s+8<=len(rom) and rom[s:s+8]==bytes(8)]
if blank:
    print(len(blank),'seeds point at blank (zero) ROM, e.g. 0x%06X: wrong ROM file (unmirrored dump?) - nothing removed' % blank[0])
    sys.exit(2 if '--fix' in sys.argv else 0)
md=capstone.Cs(capstone.CS_ARCH_M68K, capstone.CS_MODE_BIG_ENDIAN|capstone.CS_MODE_M68K_000)
starts={}; inside={}
for s in seeds:
    a=s
    for _ in range(400):
        ins=next(md.disasm(rom[a:a+10], a), None)
        if ins is None: break
        starts.setdefault(a, set()).add(s)
        for b in range(a+2, a+ins.size, 2): inside.setdefault(b, s)
        if ins.mnemonic in ('rts','rte','jmp','bra','bra.w','bra.b','bra.s') or ins.mnemonic.startswith('bra'): break
        a+=ins.size
bad=[(s,inside[s]) for s in seeds if s in inside and not (starts.get(s, set()) - {s})]
print(len(seeds),'seeds;',len(bad),'start inside an instruction of another function')
for s,f in bad: print(f'  0x{s:06X} inside code reached from 0x{f:06X}')
if '--fix' in sys.argv and bad:
    c = json.load(open(sys.argv[2]))
    for s, _ in bad: c['seeds'].pop(f'0x{s:06X}', None)
    json.dump(c, open(sys.argv[2], 'w'), indent=2); open(sys.argv[2], 'a').write('\n')
    print('removed', len(bad), 'seed(s)')
