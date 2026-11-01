#!/usr/bin/env python3
"""gaps.py TITLE: what the generated code says is missing, read logically - then looked for in the ROM.

1. References: every converted instruction (the generated C, read as kpi.py reads it) decoded as the
   generator decodes it; each ROM address it names - an absolute operand, a PC-relative one, an
   immediate (move.l #, lea, pea) - that is not converted is a candidate.
2. Pointer tables: runs of 3+ big-endian longwords (or 16-bit offset tables after a converted
   `jmp (pc,dn)` style dispatch are left to jumptables.py) in the bytes the generator left as data,
   where most entries point at converted entry points: the other entries are candidates.
3. In the ROM: each candidate decoded from its address with the generator's decoder until a return
   (rts/rte/rtr/jmp/bra): plausible code = every instruction decodes, a 68000 form, ends within 400
   bytes. Prints counts per class and the plausible candidates; writes games/TITLE/build/gaps.json
   (addresses only, git-ignored with the build).
"""
import json
import os
import sys

from capstone import Cs, CS_ARCH_M68K, CS_MODE_M68K_000

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'recompiler'))
import kpi   # noqa: E402
import m68k_decode   # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ENDS = ('rts', 'rte', 'rtr', 'jmp', 'bra')


def plausible(md, rom, a):
    """decodes as straight-line 68000 code from a to a return; the instructions, or None"""
    out, p = [], a
    while p < len(rom) and p - a < 400:
        d = m68k_decode.decode(md, rom[p:p + 10], p)
        if d is None:
            return None
        mnem, ops, n, _ = d
        if mnem.startswith('dc.') or 'illegal' in mnem:
            return None
        out.append((p, mnem, ops))
        p += n
        if mnem.split('.')[0] in ENDS:
            return out
    return None


def main():
    t = sys.argv[1]
    g = os.path.join(REPO, 'games', t)
    cfg = json.load(open(os.path.join(g, 'sidebyside.json')))
    roms = os.environ.get('PS_ROMS', os.path.join(os.path.dirname(REPO), 'roms'))
    rom = open(os.path.join(roms, cfg['rom']), 'rb').read()
    conv, entries = kpi.generated(g, len(rom))
    md = Cs(CS_ARCH_M68K, CS_MODE_M68K_000)
    md.detail = True
    covered = set()                       # every byte inside a converted instruction
    refs = {}                             # target -> (kind, from)
    for a in sorted(conv):
        d = m68k_decode.decode(md, rom[a:a + 10], a)
        if d is None:
            continue
        mnem, ops, n, _ = d
        covered.update(range(a, a + n))
        for insn in md.disasm(rom[a:a + n], a):
            for o in insn.operands:
                v = None
                if o.type == 2:                                  # immediate
                    v, kind = o.imm & 0xFFFFFFFF, 'immediate'
                elif o.type == 3:                                # memory
                    m = o.mem
                    if o.address_mode in (16, 17):               # absolute .w / .l
                        v, kind = o.imm & 0xFFFFFF, 'absolute'
                    elif o.address_mode in (11, 12):             # (d16,pc) / (d8,pc,xn)
                        v, kind = (a + 2 + m.disp) & 0xFFFFFF, 'pc-relative'
                elif o.type == 8:                                # branch displacement
                    v, kind = (a + 2 + o.br_disp.disp) & 0xFFFFFF, 'branch'
                if v is None or v & 1 or not 0x200 <= v < len(rom) or v in conv:
                    continue
                if mnem.startswith(('lea', 'pea', 'jsr', 'jmp', 'move.l', 'movea.l')) or kind in ('branch', 'pc-relative'):
                    refs.setdefault(v, (kind, a, mnem))
            break
    # pointer tables in the bytes left as data
    tables = {}
    i = 0x200
    while i + 12 <= len(rom):
        if i in covered:
            i += 2; continue
        run = []
        j = i
        while j + 4 <= len(rom) and j not in covered:
            v = int.from_bytes(rom[j:j + 4], 'big')
            if v & 1 or not 0x200 <= v < len(rom):
                break
            run.append(v); j += 4
        if len(run) >= 3 and sum(1 for v in run if v in entries) * 2 >= len(run):
            for v in run:
                if v not in conv:
                    tables.setdefault(v, i)
            i = j
        else:
            i += 2
    cand = {**{v: ('table', f, '') for v, f in tables.items()}, **refs}
    found = {}
    for v in sorted(cand):
        body = plausible(md, rom, v)
        if body:
            found[v] = (cand[v], len(body))
    by = {}
    for v, (k, n) in found.items():
        by.setdefault(k[0], []).append(v)
    print(f'{t}: {len(conv)} converted instructions, {len(entries)} entry points')
    print(f'  candidates: {len(refs)} referenced by converted code, {len(tables)} in pointer tables; '
          f'{len(found)} decode as plausible code in the ROM')
    for k, vs in sorted(by.items()):
        print(f'    {k:12s} {len(vs):5d}  e.g. ' + ' '.join('$%06X' % v for v in vs[:6]))
    for v in list(found)[:15]:
        (k, f, m), n = found[v]
        print(f'  ${v:06X} {n:3d} insns  <- {k} at ${f:06X} {m}')
    os.makedirs(os.path.join(g, 'build'), exist_ok=True)
    json.dump({'%06X' % v: {'kind': k, 'from': '%06X' % f, 'insns': n} for v, ((k, f, _), n) in found.items()},
              open(os.path.join(g, 'build', 'gaps.json'), 'w'), indent=1)
    return 0


if __name__ == '__main__':
    sys.exit(main())
