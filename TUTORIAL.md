# Tutorial

Mac (Apple Silicon) with Homebrew: `brew install cmake ninja sdl2 python`. Your own ROM in
`../roms/coi.md` (or set `CASTLE_ROM`). Paths below are from the repo root.

## 1. Play

```sh
./play.sh                            # lists the titles
./play.sh coi                     # one game: builds if needed (regenerates when the recompiler changed)
./play.sh shk1                       # SHK1: its ROM is built from your SH1 + SHK2 dumps
./launch.sh                          # the Sonic and Disney shelf apps (ROMs from ../roms)
./launch.sh sh                    # one shelf: sh, dc or all
```

**Save states**: the pause menu's SAVE STATE / LOAD STATE take and restore a snapshot of the
whole machine instantly (`engine/src/ps_state.c`), one per slot.

Keys: arrows, Z/X/C = A/B/C, Return = Start, Esc quits. Gamepads work by button position
(left/bottom/right face buttons = A/B/C, Start = Start), Switch Pro included.

**Settings menu**: F1 or Tab, gamepad Back/Select (Switch: Minus) or Guide (Home), or pause
the game with Start and press **C** (a "C: SETTINGS" hint shows while paused). Aspect
ratio, widescreen enemies, low latency, volume, fullscreen, level select (Game Genie
RGST-A6XN), infinite lives/power/items (Action Replay). Saved per game.

**Resume** a recorded session: `./play.sh coi --resume [SESSION.polls] MINUTE` replays it
to that minute at full speed (exact, the port is deterministic), then you play on.

**Session recording is automatic**: every launch records your inputs and a log (app:
`~/Library/Application Support/PixelSorcery/coi/sessions/`, script:
`games/coi/sessions/`). Report roughly when something went wrong; the
session replays exactly on the original and the port (§3).

Options (environment variables):

| Variable | Effect |
|---|---|
| `PS_WIDE=16:9` / `21:9` / `32:9` / `0` | widescreen aspect (default 16:9, gameplay only) |
| `PS_WIDE_OBJECTS=0` | keep the original object activation range |
| `PS_FRAME_DELAY=off` | disable late input polling |
| `PS_LATENCY_LOG=1` | print input→present time and audio queue depth |
| `PS_WAV=file.wav` | record the audio output |

Every session records its inputs and log: `games/coi/sessions/` (app:
`~/Library/Application Support/PixelSorcery/coi/sessions/`).

## 2. Record a reference run on the original

```sh
games/coi/tools/play_and_record.sh ../roms/coi.md
```

Plays the original game (Genesis Plus GX) and records, in `games/coi/movies/`:
inputs per controller poll (`.polls`), RAM hashes per poll (`.ck`), state snapshots every
60 polls (`.states/`), every YM2612/PSG write (`.audio`) and code coverage (`.seeds`).

## 3. Check the port against the original, fix what differs

```sh
engine/.venv/bin/python engine/tools/sidebyside.py ../roms/coi.md games/coi \
    games/coi/movies/play-XXXX.polls
```

It replays the recording on both, merges any new code into `recomp.json`, regenerates,
and compares 68K RAM at every poll. It ends with `MATCH`, or with the first divergent poll
and the RAM addresses that differ. Then:

```sh
engine/tools/divergence.sh ../roms/coi.md games/coi movies/play-XXXX.polls POLL
```

prints the first instruction where the two runs differ in PC, cycles or registers, from
per-frame clocks and instruction traces (needs `build-itrace`:
`cmake -S games/coi -B games/coi/build-itrace -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS=-DPS_ITRACE`).
Typical causes found so far: a timing rule (fix in `engine/src/bus.c` or the cycle costs in
`generate.py`), a recompiler bug (fix in `generate.py`), a VDP status detail (`bus.c`).

### Without playing: every recording, every title

```sh
engine/tools/robust.sh sh2 --mixed 30 --seeds 2   # every recording of a title, compiled and mixed
engine/tools/nightly.sh [--commit]                   # all titles in parallel + the coverage KPI (no LLM)
engine/.venv/bin/python -I engine/tools/kpi.py      # coverage KPIs per title -> docs/kpi/history.csv
PS_INTERP=random:30:1 engine/.venv/bin/python engine/tools/sidebyside.py ...   # mixed mode
```

Mixed mode interprets a random 30% of the functions, which tests the compiled/interpreted
hand-over everywhere. `engine/tools/heavy.sh CMD` runs a heavy job through 5 shared slots so
parallel runs don't oversubscribe the machine.

## 4. Regenerate and run every gate

```sh
engine/tools/recomp.sh ../roms/coi.md games/coi
```

Generates C, builds, and requires: 0 dispatch misses / bad returns, video 0 frames differing
from GPGX, YM/PSG write streams identical (oracle and native port), synth spectra close to
GPGX (librosa), and the native port matching the original side by side.

## 5. Widescreen cache

The cache fills while you play (or replay) and is saved per level/room in
`~/Library/Application Support/PixelSorcery/coi/wide/`. Replaying any
recording headless bakes it:

```sh
cd games/coi && PS_POLL_REPLAY=movies/play-XXXX.polls ./build/coi --headless --frames 999999 ../../../roms/coi.md
```

`PS_WIDE_STATS=1` prints how many side cells came from the cache.

## 6. Bring up another game

1. `mkdir games/<title>`; copy `CMakeLists.txt`, `src/main.c` (VBlank callback, widescreen
   hooks), `selfcheck.args` and `sidebyside.json` from COI and adapt.
2. `engine/tools/decomp.sh ROM games/<title>` builds the tracer, collects coverage and
   writes `recomp.json` seeds and a disassembly.
3. Sound: describe the Z80 driver upload in `audio/driver.json` (see COI's).
4. `engine/tools/recomp.sh ROM games/<title>`, then record play sessions (§2) and iterate
   with §3 until every recording matches.
5. Widescreen: find gameplay state, map id and camera X/Y by correlating RAM snapshots with
   the scroll registers (as done for COI), register them with `ps_wide_set_game`.
