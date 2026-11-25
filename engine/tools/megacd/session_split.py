#!/usr/bin/env python3
"""session_split.py DISC.cue SESSION.polls... : the owner's compilation-disc sessions cut into one cartridge
recording per game played, so each game's disc build replays them as evidence (what the owner actually did -
Tails alone in SH3 was in no test run and hung the disc).

Each session is replayed on DISC with the reference player. The menu runs from RAM ($FF0000-$FFEFFF), the games
never do: a game piece is the run of frames outside the menu after a launch. Its pad reads start at the first read
after the load; they end at the next A+B+C+Start or at the next menu. The game is the menu slot shown at the launch
($FF8002, menu.s SHOWN) on this disc's layout; the piece is then replayed on that game's cartridge (from power-on)
and kept when its game modes ($FFF600) and zones ($FFFE10) follow the disc's for its first reads (a disc that later
went wrong - a crash - still yields the piece up to there). A piece that does not match (a session made on an older
menu) is tried on the other cartridges, from a few reads either side.

Writes games/<game>/recordings/play-<session>-<k>.polls (and .from: the session and the piece's first read, for
play_check.py) and prints one line per piece.
  --only GAME[,GAME]  keep those games    --dry  print only    --min N  pieces shorter than N reads are skipped (300)"""
import os, subprocess, sys, tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..'))
REF = os.path.join(ROOT, 'games/sh1/build-trace/genrecomp_build/genrecomp_ref')
ROMS = os.path.join(ROOT, 'roms')
# this disc's menu slots (menu_compose.py --cards 1,2,3,4,5,8): game directory, cartridge (None: not a cartridge)
SLOTS = [('sh1', 'sh1.bin'), ('sh2', 'sh2.bin'), ('sh3', 'sh3.bin'), ('shk', 'shk.bin'),
         ('shk1', 'shk_sh1.bin'), ('sh-cd', None)]
CHECK = 2000         # reads compared between the disc and the cartridge (the SEGA screen alone matches any game)
GAP = 60             # frames without a read: a load


def run(target, polls, frames, watch, pclog=None, region=True):
    env = {**os.environ, 'PS_POLL_REPLAY': polls, 'PS_WATCH': watch}
    if region: env['GENRECOMP_REGION'] = '1'
    if pclog: env['PS_PC_LOG'] = pclog
    subprocess.run([REF, '--headless', '--frames', str(frames), target], env=env,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def read_watch(path):
    """[(read, frame, mode, zone, shown)] - PS_WATCH lines: read frame F600 FE10+2 8002+2"""
    out = []
    for l in open(path):
        f = l.split()
        if len(f) >= 5: out.append((int(f[0]), int(f[1]), int(f[2], 16), f[3], int(f[4], 16)))
    return out


def cart_seq(rom, polls, tmp):
    p = os.path.join(tmp, 'piece.polls'); open(p, 'w').write(''.join(polls))
    w = os.path.join(tmp, 'cart.txt')
    if os.path.exists(w): os.remove(w)
    run(rom, p, len(polls) * 2 + 600, w + ':F600,FE10+2,8002+2', region=False)
    return {r: (m, z) for r, _, m, z, _ in read_watch(w)} if os.path.exists(w) else {}


def score(disc, cart, s, n):
    """share of the first n reads (from s) whose game mode and zone match"""
    keys = [r for r in range(s, s + n) if r in disc]
    if not keys: return 0.0
    return sum(1 for r in keys if cart.get(r - s) == disc[r]) / len(keys)


def main():
    a = sys.argv[1:]
    only = dry = None; mn = 300
    if '--only' in a: i = a.index('--only'); only = set(a[i + 1].split(',')); del a[i:i + 2]
    if '--dry' in a: a.remove('--dry'); dry = True
    if '--min' in a: i = a.index('--min'); mn = int(a[i + 1]); del a[i:i + 2]
    disc, sessions = a[0], a[1:]
    tmp = tempfile.mkdtemp(prefix='split')
    for sp in sessions:
        name = os.path.basename(sp)[:-6]
        lines = open(sp).readlines()
        if not lines: continue
        w, pc = os.path.join(tmp, 'w.txt'), os.path.join(tmp, 'pc.txt')
        for f in (w, pc):
            if os.path.exists(f): os.remove(f)
        run(disc, sp, len(lines) * 2 + 1500, w + ':F600,FE10+2,8002+2', pc)
        if not os.path.exists(w): print(f'{name}: the disc replay wrote nothing'); continue
        wt = read_watch(w)
        disc_seq = {r: (m, z) for r, _, m, z, _ in wt}
        home = [i for i, l in enumerate(lines) if int(l.split()[0], 16) & 0xF0 == 0xF0
                and (i == 0 or int(lines[i - 1].split()[0], 16) & 0xF0 != 0xF0)]
        # pieces: the loader (the IP, in RAM $FF0000-$FFEFFF) runs at boot, at each launch and at each return to the
        # menu; the runs between its runs alternate menu, game, menu, game ...
        ip = {}
        for l in open(pc):
            f = l.split()
            if len(f) > 1: ip[int(f[0])] = 0xFF0000 <= int(f[1], 16) < 0xFFF000
        runs, cur, seen = [], None, False          # [first frame, last frame] of each stretch outside the loader
        for fr in sorted(ip):
            if ip[fr]: seen = True; cur = None; continue
            if not seen: continue
            if cur is None: cur = [fr, fr]; runs.append(cur)
            else: cur[1] = fr
        pieces = []
        for g0, g1 in runs[1::2]:
            rs = [r for r, fr, *_ in wt if g0 <= fr <= g1]
            if not rs: continue
            e = min([h for h in home if rs[0] <= h <= rs[-1] + 1] + [rs[-1] + 1])
            pieces.append([rs[0], e, 0])
        k = 0
        for s, e, shown in pieces:
            # the piece ends at the next home combo (the reads after it belong to the menu's return)
            if e - s < mn: continue
            order = range(len(SLOTS))
            best = None
            for slot in order:
                g, rom = SLOTS[slot]
                if not rom: continue
                rp = os.path.join(ROMS, rom)
                if not os.path.exists(rp): continue
                for d in (0, 1, -1, 2, -2, 3, -3):
                    ss = s + d
                    if ss < 0: continue
                    cs = cart_seq(rp, lines[ss:min(e, ss + CHECK)], tmp)
                    sc = score(disc_seq, cs, ss, min(CHECK, e - ss))
                    if not best or sc > best[0]: best = (sc, g, ss)
                    if sc >= 0.999: break
                if best and best[0] >= 0.999: break
            if not best: continue
            sc, g, ss = best
            k += 1
            ok = sc >= 0.3 and (not only or g in only)   # a low match is the disc parting from the cartridge: kept
            out = os.path.join(ROOT, 'games', g, 'recordings', f'play-{name}-{k}.polls')
            print(f'{name} #{k}: reads {ss}-{e} ({e - ss}), {g}, match {sc:.0%}' + ('' if ok else ' - skipped') +
                  ('' if dry or not ok else f' -> {os.path.relpath(out, ROOT)}'), flush=True)
            if ok and not dry:
                os.makedirs(os.path.dirname(out), exist_ok=True); open(out, 'w').write(''.join(lines[ss:e]))
                open(out[:-6] + '.from', 'w').write(f'{os.path.relpath(os.path.abspath(sp), ROOT)} {ss}\n')   # play_check.py: the disc session's reads before it


if __name__ == '__main__':
    main()
