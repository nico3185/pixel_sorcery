#!/usr/bin/env python3
"""jumptables.py ROM recomp.json — seed every target of a title's state dispatchers.

Games dispatch object states through small routines such as
    move.b $3(a0),d0 / add.w d0,d0 / move.w (a1,d0.w),d0 / adda.l d0,a1 / jmp (a1)
with the table passed in a1 (lea table(pc),a1 just before the call). States reached only
late in a game (a boss's defeat) are easy to miss in recordings, so seed them all.
recomp.json "dispatchers": {"0x001C84": "rel16", "0x001C74": "abs16"}
  rel16: entries are word offsets from the table, abs16: entries are absolute short addresses.
"""
import json, struct, sys

rom = open(sys.argv[1], 'rb').read()
cfg_path = sys.argv[2]
cfg = json.load(open(cfg_path))
disp = {int(k, 16): v for k, v in cfg.get('dispatchers', {}).items()}

w = lambda a: struct.unpack('>H', rom[a:a + 2])[0]
s16 = lambda v: v - 0x10000 if v & 0x8000 else v

def find_dispatchers():
    """routines shaped  moveq #0,d0 / move.b <ea>,d0 / add.w d0,d0 / move.w (aN,d0.w),d0 /
    adda.l d0,aN (rel16) | movea.l d0,aN (abs16) / jmp (aN)  - the table comes in aN"""
    found = {}
    for a in range(0, len(rom) - 20, 2):
        if w(a) != 0x7000: continue
        for ml in (2, 4, 6):                      # length of the move.b <ea>,d0
            b = a + 2 + ml
            if w(b) != 0xD040: continue           # add.w d0,d0
            m = w(b + 2); reg = m & 7
            if (m & 0xFFF8) != 0x3030 or w(b + 4) != 0x0000: continue   # move.w (aN,d0.w),d0
            nxt, jmp = w(b + 6), w(b + 8)
            if jmp != (0x4ED0 | reg): continue
            if nxt == (0xD1C0 | reg << 9): found[a] = 'rel16'      # adda.l d0,aN
            elif nxt == (0x2040 | reg << 9): found[a] = 'abs16'    # movea.l d0,aN
    return found

if not cfg.get('dispatchers') or '--auto' in sys.argv:
    auto = find_dispatchers()
    print(f'jumptables: {len(auto)} dispatcher routine(s) found: ' +
          ' '.join(f'${a:06X}:{k}' for a, k in sorted(auto.items())), file=sys.stderr)
    disp.update(auto)

def calls(target):
    """addresses of jsr/jmp target.w|.l|(pc) and bsr/bra to target"""
    for a in range(0, len(rom) - 6, 2):
        op = w(a)
        if op in (0x4EB8, 0x4EF8) and w(a + 2) == target: yield a
        elif op in (0x4EB9, 0x4EF9) and struct.unpack('>I', rom[a + 2:a + 6])[0] == target: yield a
        elif op in (0x4EBA, 0x4EFA, 0x6100, 0x6000) and a + 2 + s16(w(a + 2)) == target: yield a
        elif (op & 0xFF00) in (0x6100, 0x6000) and op & 0xFF and a + 2 + s16((op & 0xFF) * 0x101 & 0xFFFF if op & 0x80 else op & 0xFF) == target: yield a

def table_before(call):
    """table loaded into a1 shortly before the call: lea d16(pc)/abs.w/abs.l,a1 or movea.l #imm,a1"""
    for back in range(4, 26, 2):
        a = call - back
        if a < 0: break
        op = w(a)
        if op == 0x43FA: return a + 2 + s16(w(a + 2))
        if op == 0x43F8: return s16(w(a + 2)) & 0xFFFFFF
        if op in (0x43F9, 0x227C): return struct.unpack('>I', rom[a + 2:a + 6])[0] & 0xFFFFFF
    return None

new = {}
for d, kind in disp.items():
    for c in calls(d):
        t = table_before(c)
        if t is None: continue
        # the routine that loads the table is itself an entry (an object's state routine,
        # reached through a code pointer) when the lea follows a terminator
        for back in range(4, 26, 2):
            a = c - back
            if a >= 2 and w(a) in (0x43FA, 0x43F8, 0x43F9, 0x227C):
                prev = w(a - 2)
                if prev in (0x4E75, 0x4E73) or (prev & 0xFF00) == 0x6000 or \
                   (a >= 4 and w(a - 4) in (0x6000, 0x4EFA, 0x4EF8)) or (a >= 6 and w(a - 6) == 0x4EF9):
                    new[f'0x{a:06X}'] = f'sub_{a:06X}'
                break
        first = None
        for i in range(64):
            e = t + 2 * i
            if first is not None and e >= first: break        # table ends where its code starts
            v = w(e)
            tgt = (t + s16(v)) if kind == 'rel16' else v      # abs16: moveq #0 + move.w = zero-extended
            if tgt & 1 or not 0x200 <= tgt < len(rom) or (kind == 'rel16' and tgt <= e): break
            if w(tgt) in (0x0000, 0xFFFF): break                # data, not code
            first = tgt if first is None or tgt < first else first
            new[f'0x{tgt:06X}'] = f'sub_{tgt:06X}'
added = {k: v for k, v in new.items() if k not in cfg['seeds']}
cfg['seeds'].update(added)
cfg['seeds'] = dict(sorted(cfg['seeds'].items(), key=lambda kv: int(kv[0], 16)))
json.dump(cfg, open(cfg_path, 'w'), indent=2); open(cfg_path, 'a').write('\n')
print(f'jumptables: {len(new)} dispatcher targets, {len(added)} new seeds', file=sys.stderr)
