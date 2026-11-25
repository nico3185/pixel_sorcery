#!/usr/bin/env python3
"""state_compare.py DISC.cue PREFIX.polls ROM PIECE.polls MAP... [--upto READ]: the first pad read of the owner's
play where the disc game's state differs from its cartridge's - work RAM, video RAM and the palette, dumped at the
read itself (PS_STATE_AT_READ: the one point where both have run the same game logic, whatever their timing; a frame
end or a rendered picture catches a palette write a few cycles apart).

The piece is aligned first (disc_vs_cart.py: load lag realigned). A RAM difference is forgiven when it is an
address the relocation moved: the disc's long equals the cartridge's relocated by one of the game's image maps
(MAP: zonepack.py's LO-HI=NEW lists - object code pointers, the DMA queue's sources); $FFFC00-$FFFFFF is left out
(stack, the BIOS's vectors, our stubs). Video RAM and palette must match exactly. Forward every STEP reads, then
halved to the first read; its differences are printed."""
import os, subprocess, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import disc_vs_cart as dvc
STEP = 50


def state(target, lines, read, region):
    t = tempfile.mkdtemp(prefix='sc'); p = os.path.join(t, 'p.polls'); o = os.path.join(t, 's.bin')
    open(p, 'w').write(''.join(lines))
    env = {**os.environ, 'PS_POLL_REPLAY': p, 'PS_STATE_AT_READ': f'{read}:{o}', 'PS_POLL_STOP': str(read + 1)}
    if region: env['GENRECOMP_REGION'] = '1'
    subprocess.run([dvc.REF, '--headless', '--frames', str(read * 3 + 3000), target], env=env,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return open(o, 'rb').read() if os.path.exists(o) else None


def main():
    a = sys.argv[1:]; upto = None; ign = set()
    while '--ignore' in a:                                         # --ignore FFxxxx[-FFyyyy]: RAM a game fills from
        i = a.index('--ignore'); lo_, _, hi_ = a[i + 1].partition('-')   # the beam counter (timing by nature)
        ign |= set(range(int(lo_, 16) & 0xFFFF, (int(hi_ or lo_, 16) & 0xFFFF) + 1)); del a[i:i + 2]
    if '--upto' in a: i = a.index('--upto'); upto = int(a[i + 1]); del a[i:i + 2]
    disc, prefix, rom, piece, maps = a[0], a[1], a[2], a[3], a[4:]
    regions = [[(int(x.split('-')[0], 16), int(x.split('-')[1].split('=')[0], 16), int(x.split('=')[1], 16))
                for x in open(mp).read().strip().split(',') if '=' in x] for mp in maps]
    def moved(cv, dv):
        return any(lo <= cv < hi and new + cv - lo == dv for rg in regions for lo, hi, new in rg)
    pre = open(prefix).readlines(); lines = open(piece).readlines(); k0 = len(pre)
    cl, fixes, r, c, dd, m = dvc.check(disc, pre, rom, lines)
    last = min(upto if upto is not None else len(m) - 1, len(m) - 1)

    def diffs(i):
        cs = state(rom, cl, i, False); ds = state(disc, pre + lines, k0 + m[i], True)
        if cs is None or ds is None: return None
        out = []; x = 0
        while x < 0xFC00:                                          # work RAM
            if 0xFB00 <= x < 0xFC00 or x in ign: x += 1; continue              # the DMA queue: relocated sources as VDP register bytes
            if cs[x] == ds[x]: x += 1; continue
            ok = False
            for s in range(max(0, (x - 3) & ~1), x + 1, 2):
                cv = int.from_bytes(cs[s:s + 4], 'big') & 0xFFFFFF; dv = int.from_bytes(ds[s:s + 4], 'big') & 0xFFFFFF
                if cs[s] == ds[s] and moved(cv, dv): ok = True; x = s + 4; break
            if not ok: out.append(('RAM', 0xFF0000 + x)); x += 1
        out += [('VRAM', x - 0x10000) for x in range(0x10000, 0x20000) if cs[x] != ds[x]]
        out += [('CRAM', x - 0x20000) for x in range(0x20000, min(len(cs), len(ds))) if cs[x] != ds[x]]
        return out, cs, ds

    # only reads in a level (mode $0C on both): there the main loop has finished its frame and waits for VBlank when
    # the pad is read; elsewhere (an intro decompressing art, a load) the read interrupts it wherever it is, and the
    # disc - a few cycles slower a frame - is a few bytes behind: timing, not a fault
    lvl = [i for i in range(last + 1) if i in c and m[i] in dd and c[i][1][0] == '0C' and dd[m[i]][1][0] == '0C']
    if not lvl: print('no level reads'); return
    def real(i):
        """differences at read i that are still there two reads later"""
        d_ = diffs(i)
        if not d_ or not d_[0]: return None
        j = next((x for x in lvl if x >= i + 2), None)
        d2 = diffs(j) if j is not None else d_
        return d_ if d2 and d2[0] else None
    prev, found = lvl[0], None
    for i in lvl[::STEP] + [lvl[-1]]:
        d_ = real(i)
        if d_: found = (i, d_); break
        prev = i
    if not found:
        print(f'the state matches in every level read up to {last}' + (f' ({len(fixes)} realignment(s))' if fixes else '')); return
    lo, hi = prev, found[0]; best = found[1]
    while True:
        cand = [x for x in lvl if lo < x < hi]
        if not cand: break
        mid = cand[len(cand) // 2]
        d_ = real(mid)
        if d_: hi, best = mid, d_
        else: lo = mid
    out, cs, ds = best
    print(f'the state first differs at read {hi} (disc read {k0 + m[hi]}): {len(out)} byte(s)')
    runs = []
    for kind, x in out:
        if runs and runs[-1][0] == kind and x - runs[-1][2] <= 4: runs[-1][2] = x
        else: runs.append([kind, x, x])
    base = {'RAM': lambda x: x - 0xFF0000, 'VRAM': lambda x: 0x10000 + x, 'CRAM': lambda x: 0x20000 + x}
    for kind, s, e in runs[:24]:
        b0, b1 = base[kind](s), base[kind](e) + 1
        print(f'  {kind} ${s:06X}-{e:06X}  cartridge {cs[b0:b1][:16].hex()}  disc {ds[b0:b1][:16].hex()}')


if __name__ == '__main__':
    main()
