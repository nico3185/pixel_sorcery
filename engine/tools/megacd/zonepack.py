#!/usr/bin/env python3
"""zonepack.py ROMSIZE [--use DIR:MODES ...] [--exec-only DIR:MODES ...] [--fixed LO-HI=NEW ...]
               [--gap N] [--place LO-HI,...] [--refs FILE]: which ROM ranges an image needs, packed.

Reads PS_ROMUSE captures keyed by game mode (DIR/<key>.bin: exec bitmap, read bitmap, one bit
per ROM byte; the key's high byte is the game mode). --use DIR:MODES takes executed and read
bytes under those modes (hex, '+'-separated, '*' = all); --exec-only takes only executed bytes
(boot: the checksum loop reads the whole ROM under the same mode). Used 256-byte blocks are
merged into ranges across gaps < --gap (default 0x800: keeps tables with their unread entries
together), then packed into the --place areas in order (default word RAM 200000-240000, then
the program-RAM window 020000-040000), largest ranges first. --fixed ranges keep a given address
(the Z80's banks) and are excluded from packing. Prints relocate.py's --map value and the sizes.
"""
import glob, os, sys

B = 0x100


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


def bits(path, n, halves):
    nb = n // 8 + 1
    raw = open(path, 'rb').read()
    hb = len(raw) // 2                               # half size from the file (an evidence ROM
    out = set()                                      # may be bigger: a lock-on dump)
    for h in halves:
        for i, x in enumerate(raw[h * hb:h * hb + min(nb, hb)]):
            if x:
                for k in range(8):
                    if x >> k & 1:
                        out.add((i * 8 + k) // B)
                        if h == 0:                   # exec marks an instruction's first word: its
                            out.add((i * 8 + k + 9) // B)   # extension words may cross a block
    return out


def collect(spec, n, halves):
    d, modes = spec.rsplit(':', 1)
    want = None if modes == '*' else {int(m, 16) for m in modes.split('+')}
    out = set()
    for dd in glob.glob(d):
        for f in glob.glob(os.path.join(dd, '*.bin')):
            if want is None or int(os.path.basename(f)[:4], 16) >> 8 in want:
                out |= bits(f, n, halves)
    return out


def main():
    a = sys.argv[1:]
    n = int(a[0], 0)
    gap = int(a[a.index('--gap') + 1], 0) if '--gap' in a else 0x800
    blocks = {0}                                     # vectors + header always
    fixed = []
    for i, x in enumerate(a):
        if x == '--use':
            blocks |= collect(a[i + 1], n, (0, 1))
        elif x == '--exec-only':
            blocks |= collect(a[i + 1], n, (0,))
        elif x == '--force':                         # LO-HI: always included (boot code that
            lo, hi = (int(v, 16) for v in a[i + 1].split('-'))   # runs before recording starts)
            blocks |= set(range(lo // B, (hi + B - 1) // B))
        elif x == '--force-file':                    # FILE of LO-HI lines (relocate.py --funcs-out)
            for l in open(a[i + 1]):
                if l.strip():
                    lo, hi = (int(v, 16) for v in l.split('-'))
                    blocks |= set(range(lo // B, (hi + B - 1) // B))
        elif x == '--fixed':
            r, new = a[i + 1].split('=')
            lo, hi = (int(v, 16) for v in r.split('-'))
            fixed.append((lo, hi, int(new, 16)))
    for lo, hi, _ in fixed:                          # blocks wholly inside; a block shared with
        blocks -= set(range((lo + B - 1) // B, hi // B))   # other data is packed too (relocate.py
                                                     # takes the first range holding an address)
    # --refs FILE: "source target" per line (PC-relative references of executed code). A
    # reference ties every block between its source and target: those spans are rigid segments,
    # moved as one (their internal distances kept). The blocks a segment holds but this image does
    # not use are holes, filled with other ranges.
    refs = []
    for i, x in enumerate(a):
        if x == '--refs':
            refs += [tuple(int(v, 16) for v in l.split()) for l in open(a[i + 1]) if l.strip()]
    tie = [0] * (n // B + 2)                         # tie[b]: a reference spans boundary b*B
    for s_, t_ in refs:
        lo_, hi_ = min(s_, t_), max(s_, t_)
        for b in range(lo_ // B + 1, hi_ // B + 1):
            tie[b] = 1
    for i, x in enumerate(a):                        # --rigid LO-HI: data addressed as base +
        if x == '--rigid':                           # offset (sprite art DMA'd per frame): the used
            lo, hi = (int(v, 16) for v in a[i + 1].split('-'))   # part moves as one piece
            inside = [b for b in blocks if lo // B <= b < (hi + B - 1) // B]
            if inside:
                for b in range(min(inside), max(inside) + 1):
                    blocks.add(b)
                    if b > min(inside):
                        tie[b] = 1
    rom = open(a[a.index('--rom') + 1], 'rb').read() if '--rom' in a else None
    for i, x in enumerate(a):                        # --near FILE:SIZE (relocate.py --targets-out):
        if x != '--near':                            # data a code operand names is a table's
            continue                                 # start - its first SIZE bytes (or up to the
        f, size = a[i + 1].rsplit(':', 1)            # next named address) come along, read or not
        ts = sorted(int(l, 16) for l in open(f) if l.strip())
        size = int(size, 0)
        for k, t in enumerate(ts):
            if t // B not in blocks:                 # only tables the image already uses
                continue
            end = min(t + size, ts[k + 1] if k + 1 < len(ts) else t + size, n)
            if rom and not t & 1:                    # --rom: an offset table (word offsets from
                w0 = int.from_bytes(rom[t:t + 2], 'big')   # its start, frames after it: sprite
                if 2 <= w0 <= 0x400 and not w0 & 1:  # mappings, animation scripts) comes whole,
                    offs = [int.from_bytes(rom[t + 2 * i:t + 2 * i + 2], 'big') for i in range(w0 // 2)]
                    if all(not o_ & 1 and 2 <= o_ < 0x8000 for o_ in offs):    # to its last
                                                     # (frames may share entries inside the table)
                        end = max(end, min(t + max(offs) + 0x100, n))           # frame + 256
            if any(lo <= t < hi for lo, hi, _ in fixed):
                continue
            blocks |= set(range(t // B, (end + B - 1) // B))
    if rom and '--scan-tables' in a:                 # pointers in data to offset tables (an
        for _ in range(2):                           # object's property list naming its mappings)
            for b in sorted(blocks):
                for p_ in range(b * B, min(b * B + B, n - 3), 2):
                    v = int.from_bytes(rom[p_:p_ + 4], 'big')
                    end = offset_table(rom, v, n) if v < n else None
                    if end and not any(lo <= v < hi for lo, hi, _ in fixed):
                        blocks |= set(range(v // B, (end + B - 1) // B))
    used = sorted(blocks)
    segs = []                                        # rigid: consecutive blocks tied together
    for b in used:
        if segs and all(tie[k] for k in range(segs[-1][1], b + 1)):
            segs[-1][1] = b + 1
        else:
            segs.append([b, b + 1])
    rigid = [[lo, hi] for lo, hi in segs if hi - lo > 1 or tie[lo] or tie[hi]]
    # a rigid segment does not cut through contiguous data: used blocks right next to it (a
    # compressed stream running on past the segment's edge) join it
    changed = True
    while changed:
        changed = False
        rigid.sort()
        for r in rigid:
            while r[0] - 1 in blocks and not any(o is not r and o[0] <= r[0] - 1 < o[1] for o in rigid):
                r[0] -= 1; changed = True
            while r[1] in blocks and not any(o is not r and o[0] <= r[1] < o[1] for o in rigid):
                r[1] += 1; changed = True
        merged = []
        for r in sorted(rigid):
            if merged and r[0] <= merged[-1][1]:
                merged[-1][1] = max(merged[-1][1], r[1]); changed = True
            else:
                merged.append(r)
        rigid = merged
    rigid = [(lo, hi) for lo, hi in rigid]
    inrigid = {b for lo, hi in rigid for b in range(lo, hi)}
    runs = []                                        # the rest: data ranges (tables kept whole)
    for b in used:
        if b in inrigid:
            continue
        if runs and (b - runs[-1][1]) * B < gap and not any(k in inrigid for k in range(runs[-1][1], b)):
            runs[-1][1] = b + 1
        else:
            runs.append([b, b + 1])
    place = a[a.index('--place') + 1] if '--place' in a else '200000-240000,020000-040000'
    vec = [r for r in runs if r[0] == 0]             # vectors + header: at the start of word RAM
    items = [(lo, hi, True) for lo, hi in rigid] + [(lo, hi, False) for lo, hi in runs if lo != 0]

    def pack(order, bestfit):
        free = [[int(p.split('-')[0], 16), int(p.split('-')[1], 16)] for p in place.split(',')]
        for lo, hi, new in fixed:                    # fixed ranges take their room
            free = [iv for f0, f1 in free for iv in ([f0, min(f1, new)], [max(f0, new + hi - lo), f1]) if iv[1] > iv[0]]

        def take(sz):                                # word-aligned (byte-exact fixed ranges can
            cands = [f for f in free if f[1] - ((f[0] + 1) & ~1) >= sz]   # leave odd edges)
            if not cands:
                return None
            f0, f1 = min(cands, key=(lambda f: f[1] - f[0]) if bestfit else (lambda f: f[0]))
            a0 = (f0 + 1) & ~1
            free.remove([f0, f1])
            if f1 - (a0 + sz) > 0:
                free.append([(a0 + sz + 1) & ~1, f1])
            return a0
        out_rigid, out_data, holes = [], [], 0
        for lo, hi, isrigid in [(l, h, False) for l, h in vec] + order:
            new = take((hi - lo) * B)
            if new is None:
                return None, f'does not fit: {"rigid segment" if isrigid else "range"} {lo * B:06X}-{hi * B:06X} ' \
                    f'({(hi - lo) * B // 1024} KB); free: ' + ', '.join(f'{x:06X}-{y:06X}' for x, y in sorted(free))
            if not isrigid:
                out_data.append((lo * B, min(hi * B, n), new))
                continue
            out_rigid.append((lo * B, min(hi * B, n), new))
            k = lo if '--no-holes' not in a else hi  # its holes become free space (--no-holes:
            while k < hi:                            # they stay unused)
                if k in blocks:
                    k += 1; continue
                h = k
                while h < hi and h not in blocks:
                    h += 1
                free.append([new + (k - lo) * B, new + (h - lo) * B]); holes += (h - k) * B
                k = h
        return (out_rigid, out_data, holes, free), None
    # orders tried: largest first (rigid and data together: a big solid data range needs room as
    # much as code does), then code first (data fits into code holes and leftovers), best fit
    size = lambda r: r[0] - r[1]
    orders = [(sorted(items, key=size), False),
              (sorted([x for x in items if x[2]], key=size) + sorted([x for x in items if not x[2]], key=size), False),
              (sorted([x for x in items if x[2]], key=size) + sorted([x for x in items if not x[2]], key=size), True)]
    res = err = None
    for order, bf in orders:
        res, e = pack(order, bf)
        if res:
            break
        err = err or e
    if not res:
        sys.exit(err)
    out_rigid, out_data, holes, free = res
    out_fixed = list(fixed)
    # --dma-use DIR:MODES: data only DMA reads (sprite art the game's DMA queue sends): into
    # program-RAM banks the queue selects per transfer (--dma-place, virtual mirrors of the window,
    # default 0A0000-0C0000 = bank 2, 0E0000-100000 = bank 3). Each blob moves whole (merged
    # across gaps < 16 KB: art is addressed as base + offset).
    out_dma = []
    dmab = set()
    for i, x in enumerate(a):
        if x == '--dma-use':
            dmab |= collect(a[i + 1], n, (1,))
    dmab -= blocks
    for lo, hi, _ in fixed:
        dmab -= set(range(lo // B, (hi + B - 1) // B))
    if dmab:
        druns = []
        for b in sorted(dmab):
            if druns and (b - druns[-1][1]) * B < 0x4000:
                druns[-1][1] = b + 1
            else:
                druns.append([b, b + 1])
        dplace = a[a.index('--dma-place') + 1] if '--dma-place' in a else '0A0000-0C0000,0E0000-100000'
        dfree = [[int(p.split('-')[0], 16), int(p.split('-')[1], 16)] for p in dplace.split(',')]
        for lo, hi in sorted(druns, key=lambda r: r[0] - r[1]):
            sz = (hi - lo) * B
            for f in sorted(dfree):
                if f[1] - f[0] >= sz:
                    out_dma.append((lo * B, min(hi * B, n), f[0])); f[0] += sz
                    break
            else:
                sys.exit(f'DMA data does not fit: {lo * B:06X}-{hi * B:06X} ({sz // 1024} KB)')
        print(f'# DMA-only: {len(druns)} ranges, {sum(h - l for l, h, _ in out_dma) // 1024} KB', file=sys.stderr)
    out_data += out_dma
    tot = sum(hi - lo for lo, hi, _ in out_fixed + out_rigid + out_data)
    print(f'# {len(out_rigid)} rigid segments, {len(out_data)} data ranges, {tot // 1024} KB placed, '
          f'{holes // 1024} KB of holes reused; free {sum(y - x for x, y in free) // 1024} KB', file=sys.stderr)
    # order matters: relocate.py writes regions in this order, data ranges last (into holes)
    print(','.join(f'{lo:06X}-{hi:06X}={new:06X}' for lo, hi, new in out_fixed + out_rigid + out_data))

if __name__ == '__main__':
    main()
