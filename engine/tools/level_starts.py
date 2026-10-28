#!/usr/bin/env python3
"""level_starts.py TITLE [ITEM ...]: one recording per act/stage, from power-on through the game's
own level select, played on the ORIGINAL (GPGX tracer) - controller input only, so sidebyside.py
replays it on both sides like any session (RAM pokes would not be).

Recipe: games/TITLE/level_select.json (input frames and RAM addresses only):
  "code":   [[FRAME, "BTN[+BTN]", LEN], ...]   reach the level-select menu
  "move":   {"first": F, "every": N, "button": "DOWN", "len": 3}   item K = K presses
  "select": {"after": N, "button": "START", "len": 3}   N frames after the last move
  "items":  {"0": "GHZ1", ...}                  menu item -> name
  "check":  {"mode": "FFF600", "value": "0C", "settle": 240, "zone": "FFFE10"}
            the level is loaded when the byte at MODE equals VALUE; recording goes on SETTLE frames
  "item_mode": {"19": "10"}                    another VALUE for some items (a special stage)
  "also":   [[FRAME_AFTER_LOAD, "BTN", LEN], ...]   optional inputs once in the level (e.g. run right)

Writes games/TITLE/recordings/start-NAME.polls (git-ignored), its PC coverage
build/cover/start-NAME.bin (kpi.py KPI A, cover_loop.sh's exploration sources) and the traced
seeds build/starts/NAME.seeds. Prints one line per item: polls, frame the level loaded, or FAIL.
"""
import json
import os
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

ENG = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPO = os.path.dirname(ENG)


def run(t, g, rom, cfg, item, name):
    tr = os.path.join(g, 'build-trace', 'genrecomp_build', 'genrecomp_trace')
    press = [f'{f}:{b}:{n}' for f, b, n in cfg['code']]
    mv, sel, chk = cfg['move'], cfg['select'], cfg['check']
    k = int(item)
    f = mv['first']
    for i in range(k):
        press.append(f"{f + i * mv['every']}:{mv['button']}:{mv['len']}")
    fs = f + max(k - 1, 0) * mv['every'] + sel['after']
    press.append(f"{fs}:{sel['button']}:{sel['len']}")
    frames = fs + chk.get('load_max', 900)
    work = os.path.join(g, 'build', 'starts')
    os.makedirs(work, exist_ok=True)
    os.makedirs(os.path.join(g, 'recordings'), exist_ok=True)
    os.makedirs(os.path.join(g, 'build', 'cover'), exist_ok=True)
    mode = chk['mode']
    # first pass: when does the level load (watch the mode byte)
    zone = chk.get('zone')
    env = dict(os.environ, GENRECOMP_WATCH=mode + (',' + zone if zone else ''), GENRECOMP_WATCH_EVERY='100000')
    args = [tr, '--headless', '--frames', str(frames), '--trace-out', '/dev/null']
    for p in press:
        args += ['--press', p]
    out = subprocess.run(args + [rom], env=env, capture_output=True, text=True).stderr + ''
    want = int(cfg.get('item_mode', {}).get(item, chk['value']), 16)
    loaded = None
    zv = ''
    for m in re.finditer(r'watch f=(\d+) \w+=([0-9A-F]{4})(?: \w+=([0-9A-F]{4}))?', out):
        if int(m.group(2), 16) >> 8 == want and int(m.group(1)) > fs:
            loaded, zv = int(m.group(1)), m.group(3) or ''
            break
    if loaded is None:
        return f'{t} {name}: FAIL (mode {mode} never {chk["value"]} after the select at frame {fs})'
    # second pass: record the polls and the coverage up to load + settle (+ the "also" inputs)
    end = loaded + chk.get('settle', 240)
    for fa, b, n in cfg.get('also', []):
        press.append(f'{loaded + fa}:{b}:{n}')
        end = max(end, loaded + fa + n + 60)
    polls = os.path.join(g, 'recordings', f'start-{name}.polls')
    cov = os.path.join(g, 'build', 'cover', f'start-{name}.bin')
    seeds = os.path.join(work, f'{name}.seeds')
    for p in (polls, cov):
        if os.path.exists(p):
            os.remove(p)
    env = dict(os.environ, PS_POLL_RECORD=polls, PS_PCCOVER=cov)
    args = [tr, '--headless', '--frames', str(end), '--trace-out', seeds]
    for p in press:
        args += ['--press', p]
    subprocess.run(args + [rom], env=env, capture_output=True)
    n = sum(1 for _ in open(polls)) if os.path.exists(polls) else 0
    return f'{t} {name}: {n} polls, level loaded at frame {loaded}' + (f', {zone}={zv}' if zone else '') if n else f'{t} {name}: FAIL (no polls)'


def main():
    t = sys.argv[1]
    g = os.path.join(REPO, 'games', t)
    cfg = json.load(open(os.path.join(g, 'level_select.json')))
    sbs = json.load(open(os.path.join(g, 'sidebyside.json')))
    rom = os.path.abspath(os.path.join(os.environ.get('PS_ROMS', os.path.join(REPO, '..', 'roms')), sbs['rom']))
    items = sys.argv[2:] or list(cfg['items'])
    with ThreadPoolExecutor(int(os.environ.get('STARTS_JOBS', '4'))) as ex:
        for line in ex.map(lambda i: run(t, g, rom, cfg, i, cfg['items'][i]), items):
            print(line, flush=True)


if __name__ == '__main__':
    main()
