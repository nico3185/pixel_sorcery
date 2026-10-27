#!/usr/bin/env python3
"""knuckles_refs.py KIS2_ROM RECOMP_JSON OUT.md [--json OUT.json] — SHK1, step 1:
what Knuckles' code in SHK2 depends on outside itself.

SHK2 (the SHK + SH2 lock-on, the player's own dump: shk_sh2.bin) runs a SH2 engine
from the SHK cartridge's patch chip ($300000-$33FFFF); its player object is Knuckles. Starting
from the entry points our SHK2 port knows in the player object's range ($30E000-$310200: the
object, glide, climb, wall grab), this walks every instruction reachable by branches and calls,
and lists:
  - the functions inside the range (Knuckles' own code, to carry over);
  - calls and jumps leaving it (SH2 engine routines: floor/wall finders, animation, art,
    sound, camera...), each with its callers - the table to map to SH1 (step 2);
  - RAM it reads or writes by absolute address (player variables, camera, timers...);
  - ROM data it points at (animation scripts, mappings, DPLC, art, palettes).
Object-relative fields (d(a0)) are listed by offset, to compare with SH1's object layout.
Nothing from the ROM is written out but addresses and instruction text.
"""
import argparse, collections, json, re
import capstone

LO, HI = 0x315328, 0x31768E

ap = argparse.ArgumentParser()
ap.add_argument('rom'); ap.add_argument('recomp'); ap.add_argument('out')
ap.add_argument('--json')
a = ap.parse_args()
rom = open(a.rom, 'rb').read()
seeds = sorted(int(k, 16) for k in json.load(open(a.recomp))['seeds'] if LO <= int(k, 16) < HI)
md = capstone.Cs(capstone.CS_ARCH_M68K, capstone.CS_MODE_BIG_ENDIAN | capstone.CS_MODE_M68K_000)

insns = {}
todo = list(seeds)
ext_calls = collections.defaultdict(set)      # target -> callers
ram = collections.defaultdict(set)            # RAM address -> instructions
romdata = collections.defaultdict(set)        # ROM data address -> instructions
fields = collections.Counter()                # d(a0) offsets
ENDS = ('rts', 'rte', 'rtr', 'jmp', 'bra')
while todo:
    pc = todo.pop()
    while LO <= pc < HI and pc not in insns:
        ins = next(md.disasm(rom[pc:pc + 10], pc), None)
        if ins is None:
            break
        insns[pc] = ins
        m, ops = ins.mnemonic, ins.op_str
        base = m.split('.')[0]
        # branch / call targets
        tgt = None
        if base in ('bsr', 'jsr', 'bra', 'jmp') or re.match(r'^b[a-z]{2}$', base) or base.startswith('db'):
            t = re.search(r'\$([0-9a-f]+)(?:\.[wl])?$', ops.split(',')[-1].strip())
            if t and '(' not in ops.split(',')[-1]:
                tgt = int(t.group(1), 16)
        if tgt is not None:
            if LO <= tgt < HI:
                todo.append(tgt)
            else:
                ext_calls[tgt].add(pc)
        # absolute memory operands
        for v in re.findall(r'\$([0-9a-f]+)\.([wl])', ops):
            x = int(v[0], 16)
            if v[1] == 'w' and x >= 0x8000:
                x |= 0xFF0000
            if x >= 0xFF0000:
                ram[x].add(pc)
            elif x < len(rom) and not (LO <= x < HI) and base not in ('jsr', 'jmp'):
                romdata[x].add(pc)
        for v in re.findall(r'#\$([0-9a-f]+)', ops):     # immediate ROM pointers (lea/move.l #addr)
            x = int(v, 16)
            if 0x200000 <= x < len(rom) and x % 2 == 0 and ('move.l' in m or base == 'lea'):
                romdata[x].add(pc)
        for v in re.findall(r'\$([0-9a-f]+)\(pc\)', ops):   # pc-relative data
            x = int(v, 16)
            if base not in ('lea', 'pea', 'jsr', 'jmp') or not (LO <= x < HI):
                romdata[x].add(pc)
        for v in re.findall(r'(-?\$?[0-9a-f]+)\(a0\)', ops):
            fields[v] += 1
        if base in ENDS or (base == 'jmp'):
            break
        pc += ins.size

funcs = sorted(seeds)
with open(a.out, 'w') as f:
    f.write('# Knuckles in SHK2: external references (step 1)\n\n')
    f.write(f'Range ${LO:06X}-${HI:06X}: {len(insns)} instructions reached from {len(funcs)} entry points.\n\n')
    f.write(f'## Calls and jumps out of the range ({len(ext_calls)} targets)\n\n| target | callers | first caller |\n|---|---|---|\n')
    for t, c in sorted(ext_calls.items(), key=lambda kv: -len(kv[1])):
        f.write(f'| ${t:06X} | {len(c)} | ${min(c):06X} |\n')
    f.write(f'\n## RAM by absolute address ({len(ram)})\n\n| address | uses |\n|---|---|\n')
    for x, c in sorted(ram.items()):
        f.write(f'| ${x:06X} | {len(c)} |\n')
    f.write(f'\n## ROM data ({len(romdata)})\n\n| address | uses |\n|---|---|\n')
    for x, c in sorted(romdata.items()):
        f.write(f'| ${x:06X} | {len(c)} |\n')
    f.write(f'\n## Object fields d(a0) ({len(fields)})\n\n| offset | uses |\n|---|---|\n')
    for o, n in sorted(fields.items(), key=lambda kv: int(kv[0].replace('$', '0x'), 16) if kv[0].lstrip('-$').isalnum() else 0):
        f.write(f'| {o} | {n} |\n')
if a.json:
    json.dump({'calls': {f'{t:06X}': sorted(f'{c:06X}' for c in v) for t, v in ext_calls.items()},
               'ram': {f'{x:06X}': len(v) for x, v in ram.items()},
               'rom': {f'{x:06X}': len(v) for x, v in romdata.items()},
               'fields': dict(fields)}, open(a.json, 'w'), indent=1)
print(f'{len(insns)} instructions; {len(ext_calls)} external call targets, {len(ram)} RAM addresses, '
      f'{len(romdata)} ROM data references, {len(fields)} object fields -> {a.out}')
