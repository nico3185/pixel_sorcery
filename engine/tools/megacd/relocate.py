#!/usr/bin/env python3
"""relocate.py ROM CONFIG.json OUT [--ptrs FILE ...] [--off 0x200000]: move a cartridge game.

A Mega CD maps its BIOS at $000000, so a cartridge game loaded from disc must run elsewhere
(word RAM: $200000). This rewrites every absolute ROM address the game uses:
  - in code (the analyzer's instructions, seeded with CONFIG's traced entry points): absolute
    long operands ($xxxxxx.l: JSR/JMP/LEA/MOVE ...) and .l immediates that are ROM addresses;
  - in data: the longwords the tracer saw read as ROM addresses (PS_PTRS files, from play);
  - the vectors.
Absolute short (.w) references into the ROM cannot be rewritten in place and are listed.
Writes OUT.bin (the image to load at --off) and OUT.trapcart.md: a 4 MB test cartridge with the
relocated game at --off, its vectors at $000000 and the old range filled with ILLEGAL, so the
tracer's PS_TRAP shows every pointer the relocation missed (execution, reads or DMA there).
"""
import contextlib, io, json, os, re, sys
here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(here, '..', 'recompiler'))
from analyze import GenesisROM, M68KAnalyzer   # noqa: E402

ABS_L = re.compile(r'\$([0-9a-fA-F]+)\.l')
ABS_W = re.compile(r'(?<![#\w])\$([0-9a-fA-F]+)\.w')
IMM = re.compile(r'#\$([0-9a-fA-F]+)')


def offset_table(rom, t, n):
    """t starts a table of word offsets from t (sprite mappings, animation scripts): first word
    even and small, every entry even and inside a plausible span. Returns its end or None."""
    if t & 1 or not (0x200 <= t < n - 2):
        return None
    w0 = int.from_bytes(rom[t:t + 2], 'big')
    if not (2 <= w0 <= 0x400) or w0 & 1:
        return None
    offs = [int.from_bytes(rom[t + 2 * i:t + 2 * i + 2], 'big') for i in range(w0 // 2)]
    if not all(not o & 1 and w0 <= o < 0x4000 for o in offs):
        return None
    return min(t + max(offs) + 0x100, n)


def main():
    args = sys.argv[1:]
    off = int(args[args.index('--off') + 1], 0) if '--off' in args else 0x200000
    # --map LO-HI=NEW[,...]: ROM ranges and where they go (default: the whole ROM at --off).
    # Unmapped addresses are left as they are (the trap cartridge catches their use).
    regions = []
    if '--map' in args:
        for r in args[args.index('--map') + 1].split(','):
            rng, new = r.split('=')
            lo, hi = (int(x, 16) for x in rng.split('-'))
            regions.append((lo, hi, int(new, 16)))
    blobs = []                                    # --blob NEW=FILE: code/data at a new address
    for i, a in enumerate(args):                  # (free space in a region: helpers for patches)
        if a == '--blob':
            ad, fn = args[i + 1].split('=')
            blobs.append((int(ad, 16), open(fn, 'rb').read()))
    patches = []
    for i, a in enumerate(args):
        if a == '--patch':                        # ADDR=HEX (original address): game fixes, e.g. a
            ad, hx = args[i + 1].split('=')       # checksum branch that cannot hold once relocated
            patches.append((int(ad, 16), bytes.fromhex(hx)))
    ptr_files = [args[i + 1] for i, a in enumerate(args) if a == '--ptrs']
    exec_files = [args[i + 1] for i, a in enumerate(args) if a == '--exec']
    rom_path, cfg_path, out = args[0], args[1], args[2]
    rom = GenesisROM(rom_path)
    data = bytearray(rom.data)
    n = len(data)
    if not regions:
        regions = [(0, n, off)]

    def reloc(v):                                 # new address of ROM address v, None if unmapped
        for lo, hi, new in regions:
            if lo <= v < hi:
                return new + (v - lo)
        return None
    an = M68KAnalyzer(rom)
    an.load_config(json.load(open(cfg_path)))
    with contextlib.redirect_stdout(io.StringIO()):
        an.analyze()

    # instructions that ran (tracer PS_ROMUSE bitmaps: exec half): only their operands are
    # rewritten - the analyzer also decodes data as code, and an operand there would be data
    executed, readdata = set(), bytearray(n // 8 + 1)
    for f in exec_files:
        raw = open(f, 'rb').read()
        h = len(raw) // 2                         # exec half, read half (an evidence ROM may be
        b = raw[:min(h, n // 8 + 1)]              # bigger: a lock-on dump)
        executed |= {i for i in range(0, min(n, len(b) * 8), 2) if b[i >> 3] >> (i & 7) & 1}
        for i, x in enumerate(raw[h:h + min(h, n // 8 + 1)]):
            readdata[i] |= x
    code_bytes = set()
    ran_bytes = set()                             # bytes of instructions that ran
    fixed, short_refs, imm_fixed, tables = 0, [], 0, 0
    PCREL = re.compile(r'\$([0-9a-fA-F]+)\(pc, [ad]\d\.[wl]\)')
    # a callback stored by move.l #CODE,... (an object's next routine, a timer's action): not
    # round (fixed-point speeds are: $18000) and an instruction starts there
    # (stored into an object field: (aN) or d(aN) - a score of 100000 = $186A0 goes elsewhere)
    #  or the address of a RAM jump stub (JMP $4EF9 at $FFF0-$FFFF: SHK's VBlank and HBlank
    #  handlers, $584 and $D10 - low ROM, below the round-constant cut)
    codeptr = lambda mn, v, ops='': (mn == 'move.l' and v in an.instructions and (
        (v >= 0x10000 and v & 0xFF and re.search(r',\s*((-?\$?[0-9a-f]*)\(a[0-6]\)|\$[bc][0-9a-f]{3}\.w)$', ops))
        or (v >= 0x200 and re.search(r',\s*\$fff[0-9a-f]\.w$', ops))))
    #  or an object slot by address ($FFB000-$FFCFFF: a character spawned on a restart)
    if executed:                                  # exact instruction starts: decode them directly
        import capstone
        md = capstone.Cs(capstone.CS_ARCH_M68K, capstone.CS_MODE_BIG_ENDIAN | capstone.CS_MODE_M68K_000)
        insns = {}
        for a in executed:
            for i in md.disasm(rom.data[a:a + 10], a):
                insns[a] = (i.mnemonic, i.op_str, i.size, bytes(i.bytes)); break
        # and the rest of every function that ran (paths a run did not take: the trap showed a
        # JSR at $00A1D0 that ran later), unless the analyzer's decode overlaps bytes read as data
        isdata = lambda a, sz: any(readdata[x >> 3] >> (x & 7) & 1 for x in range(a, a + sz))
        for f in an.functions.values():
            ia = f.get('insn_addrs') or []
            if '--all-code' in args or any(x in executed for x in ia):   # --all-code: every function
                for x in ia:
                    if x not in insns and x in an.instructions and not isdata(x, an.instructions[x][2]):
                        insns[x] = an.instructions[x]
        # absolute calls/jumps/loads of code outside every known function (an object routine the
        # analyzer never reached): their target is an instruction - they are code too
        ABSCODE = re.compile(r'^\$([0-9a-f]+)\.l$')
        for x, ins in an.instructions.items():
            if x not in insns and ins[0] in ('jsr', 'jmp', 'lea', 'pea') and not isdata(x, ins[2]):
                t_ = ABSCODE.match(ins[1].split(',')[0].strip())
                if t_ and int(t_.group(1), 16) in an.instructions:
                    insns[x] = ins
            elif x not in insns and ins[0] == 'move.l' and ins[1].startswith('#$') and not isdata(x, ins[2]):
                if codeptr('move.l', int(ins[1][2:].split(',')[0], 16), ins[1]):   # a callback store
                    insns[x] = ins
    else:
        insns = an.instructions
    if '--funcs-out' in args and executed:        # bodies of the functions that ran (paths a run
        spans = []                                # did not take included): zonepack.py --force
        for f in an.functions.values():
            ia = f.get('insn_addrs') or []
            if any(x in executed for x in ia):
                for x in ia:
                    if x in an.instructions:
                        spans.append((x, x + an.instructions[x][2]))
        spans.sort(); merged = []
        for lo_, hi_ in spans:
            if merged and lo_ <= merged[-1][1] + 16:
                merged[-1][1] = max(merged[-1][1], hi_)
            else:
                merged.append([lo_, hi_])
        open(args[args.index('--funcs-out') + 1], 'w').write(''.join(f'{a:06X}-{b:06X}\n' for a, b in merged))
    # --strict-imm: immediates, and operands of instructions that never ran, count as addresses
    # only where the evidence agrees (a 2 MB ROM holds many constants that look like addresses)
    strict = '--strict-imm' in args
    BRANCH = re.compile(r'^(b[a-z]{2,3}|db[a-z]{1,2})(\.[bwsl])?$')
    cross = []                                    # relative references between differently moved ranges
    targets = set()                               # --targets-out: ROM addresses code operands name
    for a, (mn, ops, size, raw) in sorted(insns.items()):
        code_bytes.update(range(a, a + size))
        if a in executed:
            ran_bytes.update(range(a, a + size))
        if reloc(a) is not None:
            tgts = [int(v, 16) for v in re.findall(r'\$([0-9a-fA-F]+)\(pc', ops)]
            if BRANCH.match(mn) and mn.split('.')[0] not in ('btst', 'bset', 'bclr', 'bchg'):
                tgts += [int(v, 16) for v in re.findall(r'\$([0-9a-fA-F]+)$', ops)]
            for t in tgts:
                if t < n and (reloc(t) is None or reloc(t) - t != reloc(a) - a):
                    cross.append((a, mn, ops, t))
        m = PCREL.search(ops)
        if m and mn.split('.')[-1] == 'l' and mn.split('.')[0] in ('move', 'movea'):
            t = int(m.group(1), 16)               # a longword table read PC-relative (no read hook
            v0 = int.from_bytes(rom.data[t:t + 4], 'big')   # sees those): its entries while they are
            if not (0x200 <= v0 < n) or v0 & 1:  # ROM addresses; "table - 4" bases (indices from 1)
                t += 4
            while t + 4 <= n:
                v = int.from_bytes(rom.data[t:t + 4], 'big')
                if not (0x200 <= v < n) or v & 1 or data[t:t + 4] != rom.data[t:t + 4]:
                    break
                if reloc(v) is not None:          # an entry this image does not hold (an object
                    data[t:t + 4] = reloc(v).to_bytes(4, 'big'); tables += 1   # another zone uses)
                t += 4                            # stays as it was; the table goes on
        vals = [int(v, 16) for v in ABS_L.findall(ops)]
        if mn.endswith('.l') or mn.split('.')[0] in ('lea', 'pea', 'jsr', 'jmp'):
            imms = [int(v, 16) for v in IMM.findall(ops) if 0x200 <= int(v, 16) < n]
            if executed and strict:               # small round constants are counts/sizes ($800 =
                imms = [v for v in imms if v & 0xFF or v >= 0x10000 or mn.split('.')[0] in ('lea', 'pea', 'movea')]   # 2 KB block)
            if executed and strict:               # an immediate is an address only with evidence:
                rd = lambda x: readdata[x >> 3] >> (x & 7) & 1          # moved into an address
                imms = [v for v in imms if mn.split('.')[0] == 'movea'  # register, an instruction
                        or v in executed or any(rd(x) for x in range(v, min(v + 64, n)))
                        or codeptr(mn, v, ops)
                        or offset_table(rom.data, v, n)]   # starts there, a table of offsets starts there,
                                                  # or data is read there (a sprite table's first entries may never be)
            vals += imms                          # read there (a score of 5000 = $1388 in code is no pointer)
            imm_fixed += len(imms)
        if executed and strict and a not in executed:   # decoded but never run (maybe data
            rd2 = lambda x: readdata[x >> 3] >> (x & 7) & 1         # read as code - a sprite
            vals = [v for v in vals if v >= n or v in executed or rd2(v)    # mapping's frames): its
                    or (mn.split('.')[0] in ('jsr', 'jmp', 'lea', 'pea') and v in an.instructions)   # (code targets count,
                    or codeptr(mn, v, ops)]                                    #  and stored callbacks)
        if '--targets-out' in args:               # operands count only where the evidence agrees
            targets.update(v for v in vals if 0x200 <= v < n)
        for v in vals:
            if v >= n:
                continue                          # RAM, I/O: untouched
            nv = reloc(v)
            if nv is None:
                continue
            b = v.to_bytes(4, 'big')
            for k in range(2, size - 3, 2):       # the extension words holding it
                if data[a + k:a + k + 4] == b:
                    data[a + k:a + k + 4] = nv.to_bytes(4, 'big')
                    fixed += 1
                    break
        for v in (int(x, 16) for x in ABS_W.findall(ops)):
            if 0x200 <= v < min(n, 0x8000):
                short_refs.append((a, mn, ops))
    ptrs = set()
    for f in ptr_files:
        ptrs |= {int(l, 16) for l in open(f) if l.strip()}
    data_fixed = 0
    DECIMAL = {10 ** k for k in range(3, 10)}
    for p in sorted(ptrs):
        if p in code_bytes or data[p:p + 4] != rom.data[p:p + 4]:
            continue                              # an operand (done above) or a table entry
        v = int.from_bytes(rom.data[p:p + 4], 'big')
        if v in DECIMAL or (v < 0x10000 and v % 1000 == 0):
            continue                              # a decimal constant (SHK's powers of ten for its
                                                  #  digit display: 1000 read like a pointer)
        if 0x200 <= (v & 0xFFFFFF) < n and reloc(v & 0xFFFFFF) is not None:   # a top-byte tag stays
            data[p:p + 4] = ((v & 0xFF000000) | reloc(v & 0xFFFFFF)).to_bytes(4, 'big')
            data_fixed += 1
    scanned = 0
    if '--scan-tables' in args:                   # longwords in data naming an offset table
        skip = ran_bytes if executed else code_bytes   # with evidence: data inside code areas too
        for lo, hi, _ in regions:                 # (an object's attribute block: mappings, art)
            for p in range(lo + (lo & 1), hi - 3, 2):
                if p in skip or data[p:p + 4] != rom.data[p:p + 4]:
                    continue
                v = int.from_bytes(rom.data[p:p + 4], 'big')
                if v < n and reloc(v) is not None and offset_table(rom.data, v, n):
                    data[p:p + 4] = reloc(v).to_bytes(4, 'big'); scanned += 1
    for i in range(1, 64):                        # vectors (not the initial SSP)
        v = int.from_bytes(rom.data[i * 4:i * 4 + 4], 'big')
        if 0x200 <= v < n and reloc(v) is not None:
            data[i * 4:i * 4 + 4] = reloc(v).to_bytes(4, 'big')
    for h in (0x1A0, 0x1A4):                      # header ROM start/end: games' checksum loops use them
        v = int.from_bytes(rom.data[h:h + 4], 'big')
        if v < n and reloc(v) is not None:
            data[h:h + 4] = reloc(v).to_bytes(4, 'big')
    for ad, hx in patches:
        data[ad:ad + len(hx)] = hx
    s = sum(int.from_bytes(data[i:i + 2], 'big') for i in range(0x200, n - 1, 2)) & 0xFFFF
    data[0x18E:0x190] = s.to_bytes(2, 'big')      # checksum of the relocated image
    open(out + '.bin', 'wb').write(data)

    cart = bytearray(b'\x4a\xfc' * (0x400000 // 2))        # ILLEGAL everywhere
    cart[0:0x200] = data[0:0x200]                          # relocated vectors + header
    for lo, hi, new in regions:                            # each region where it now lives
        cart[new:new + (hi - lo)] = data[lo:hi]
    for ad, b in blobs:
        cart[ad:ad + len(b)] = b
    open(out + '.trapcart.md', 'wb').write(cart)
    open(out + '.map', 'w').write(''.join(f'{lo:06X}-{hi:06X}={new:06X}\n' for lo, hi, new in regions))
    print(f'{len(executed) or len(an.instructions)} instructions considered, {tables} PC-relative table entries; {fixed} absolute ROM references rewritten '
          f'({imm_fixed} of them immediates); {data_fixed} data pointers (of {len(ptrs)} seen); '
          f'{len(short_refs)} .w ROM references (not rewritable); {scanned} table pointers found in data:')
    for a, mn, ops in short_refs[:20]:
        print(f'  ${a:06X}  {mn} {ops}')
    if '--targets-out' in args:                   # for zonepack.py --near: tables start there
        open(args[args.index('--targets-out') + 1], 'w').write(''.join(f'{t:06X}\n' for t in sorted(targets)))
    if '--cross-out' in args:                     # for zonepack.py --join: source target per line
        open(args[args.index('--cross-out') + 1], 'w').write(''.join(f'{a:06X} {t:06X}\n' for a, _, _, t in cross))
    if cross:                                     # --map cut points must not split these
        print(f'{len(cross)} PC-relative references cross a --map boundary (target moved differently):')
        for a, mn, ops, t in cross[:40]:
            print(f'  ${a:06X}  {mn} {ops}  -> ${t:06X}' + ('' if reloc(t) is not None else ' (unmapped)'))
    print(f'{out}.bin ({n} bytes, for ${off:X}), {out}.trapcart.md (test: PS_TRAP=200-{off:X})')


if __name__ == '__main__':
    main()
