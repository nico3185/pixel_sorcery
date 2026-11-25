#!/usr/bin/env python3
"""render_compare.py DISC.cue PREFIX.polls ROM PIECE.polls [--upto READ] [--out PNG]: the first read of the
owner's play where the disc's picture differs from the cartridge's.

The piece is aligned first (disc_vs_cart.py: load lag realigned); then the frame of a read is rendered on both and
compared pixel for pixel - nothing to forgive (relocated addresses never reach the screen). The first differing
read is found by halving between the start and --upto (default: disc_vs_cart.py's parting, else the end). --out:
cartridge | disc | difference (red), side by side."""
import os, subprocess, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import disc_vs_cart as dvc
STEP = 30
VDP = False
TOP = 2
from PIL import Image, ImageChops


def frame(target, lines, fr, region):
    t = tempfile.mkdtemp(prefix='rc'); p = os.path.join(t, 'p.polls'); open(p, 'w').write(''.join(lines))
    env = {**os.environ, 'PS_POLL_REPLAY': p, 'PS_FRAMES_DIR': t, 'PS_FRAMES_EVERY': str(fr)}
    if region: env['GENRECOMP_REGION'] = '1'
    subprocess.run([dvc.REF, '--headless', '--frames', str(fr + 1), target], env=env,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    f = os.path.join(t, 'f%06d.png' % fr)
    return Image.open(f).convert('RGB') if os.path.exists(f) else None


def vdp(target, lines, fr, region):
    """VRAM (64 KB) + CRAM (128 bytes) at the end of frame fr"""
    t = tempfile.mkdtemp(prefix='rv'); p = os.path.join(t, 'p.polls'); open(p, 'w').write(''.join(lines))
    v, r = os.path.join(t, 'v.bin'), os.path.join(t, 'r.bin')
    env = {**os.environ, 'PS_POLL_REPLAY': p, 'PS_VRAM_AT': f'{fr}:{v}', 'PS_RAMDUMP_AT': f'{fr}:{r}'}
    if region: env['GENRECOMP_REGION'] = '1'
    subprocess.run([dvc.REF, '--headless', '--frames', str(fr + 2), target], env=env,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if not (os.path.exists(v) and os.path.exists(r)): return None
    return open(v, 'rb').read() + open(r, 'rb').read()[0x10000:0x10080]


def main():
    a = sys.argv[1:]; upto = None; out = None
    if '--upto' in a: i = a.index('--upto'); upto = int(a[i + 1]); del a[i:i + 2]
    if '--out' in a: i = a.index('--out'); out = a[i + 1]; del a[i:i + 2]
    global VDP
    VDP = '--vdp' in a
    if VDP: a.remove('--vdp')
    disc, prefix, rom, piece = a[:4]
    pre = open(prefix).readlines(); lines = open(piece).readlines()
    cl, fixes, r, c, dd, m = dvc.check(disc, pre, rom, lines)
    keys = [i for i in range(len(m)) if i in c and m[i] in dd]
    if not keys: print('no common reads'); return
    hi_i = len(keys) - 1
    if upto is not None: hi_i = max(k for k, i in enumerate(keys) if i <= upto)
    elif r is not None: hi_i = max(k for k, i in enumerate(keys) if i <= r)

    def at(k):
        i = keys[k]
        if VDP:                                                   # what the picture is made of, at frame end
            cv = vdp(rom, cl, c[i][0], False); dv = vdp(disc, pre + lines, dd[m[i]][0], True)
            if cv is None or dv is None: return None, None, None
            d_ = [x for x in range(len(cv)) if cv[x] != dv[x]]
            return ((d_[0], d_[-1]) if d_ else None), None, None
        ci = frame(rom, cl, c[i][0], False); di = frame(disc, pre + lines, dd[m[i]][0], True)
        if ci is None or di is None: return None, ci, di
        if ci.size != di.size: di = di.resize(ci.size)
        # lines 0-1 left out: a palette write at VBlank's end lands a few cycles later on the disc (its interrupts
        # pass the BIOS's vectors) and can colour the top line - every frame while a palette cycles (SHK's intro)
        b = ImageChops.difference(ci, di).crop((0, TOP, ci.width, ci.height)).getbbox()
        return ((b[0], b[1] + TOP, b[2], b[3] + TOP) if b else None), ci, di

    def lasting(k):
        """a difference still there 1 and 2 reads later (a one-frame blip - a palette write landing on the top line
        a few cycles later on the disc, through the BIOS's vector - is timing, not a fault)"""
        b0, ci, di = at(k)
        if not b0: return None
        for j in (1, 2):
            if k + j <= hi_i and not at(k + j)[0]: return None
        return b0, ci, di
    found = None; prev = 0
    for k in list(range(0, hi_i + 1, STEP)) + [hi_i]:            # forward, every STEP reads
        f = lasting(k)
        if f: found = (k, f); break
        prev = k
    if not found:
        print(f'the pictures match up to read {keys[hi_i]} (one-frame blips aside)'); return
    lo, hi = prev, found[0]; box, ci, di = found[1]
    while hi - lo > 1:                                            # then its first read
        mid = (lo + hi) // 2
        f = lasting(mid)
        if f: hi, (box, ci, di) = mid, f
        else: lo = mid
    i = keys[hi]
    if VDP:
        cv = vdp(rom, cl, c[i][0], False); dv = vdp(disc, pre + lines, dd[m[i]][0], True)
        d_ = [x for x in range(len(cv)) if cv[x] != dv[x]]
        runs = []
        for x in d_:
            if runs and x - runs[-1][1] <= 16: runs[-1][1] = x
            else: runs.append([x, x])
        print(f'VRAM/CRAM first differ at read {i} (cartridge frame {c[i][0]}, disc frame {dd[m[i]][0]}): {len(d_)} bytes')
        for lo_, hi_ in runs[:16]:
            nm = 'CRAM %02X' % (lo_ - 0x10000) if lo_ >= 0x10000 else 'VRAM %04X' % lo_
            print(f'  {nm}-{hi_ & 0xFFFF:04X}  cartridge {cv[lo_:hi_ + 1][:16].hex()}  disc {dv[lo_:hi_ + 1][:16].hex()}')
        ci = frame(rom, cl, c[i][0], False); di = frame(disc, pre + lines, dd[m[i]][0], True); box = (0, 0, 0, 0)
        if not out: return
    print(f'the picture first differs at read {i} (cartridge frame {c[i][0]}, disc frame {dd[m[i]][0]}): '
          f'area {box[0]},{box[1]}-{box[2]},{box[3]}')
    if out:
        d = ImageChops.difference(ci, di).convert('L').point(lambda v: 255 if v else 0)
        red = Image.new('RGB', ci.size, (255, 0, 0)); diffimg = Image.composite(red, ci.convert('L').convert('RGB'), d)
        w = Image.new('RGB', (ci.width * 3 + 8, ci.height), (40, 40, 40))
        w.paste(ci, (0, 0)); w.paste(di, (ci.width + 4, 0)); w.paste(diffimg, (2 * ci.width + 8, 0))
        w.save(out); print(f'  {out}: cartridge | disc | difference')


if __name__ == '__main__':
    main()
