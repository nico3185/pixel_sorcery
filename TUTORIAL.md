# Tutorial

Mac (Apple Silicon) with Homebrew: `brew install cmake ninja sdl2 python`. Your own ROM in
`../roms/coi.md` (or set `ROM`). Paths below are from the repo root.

## 1. Play

```sh
./play.sh                            # lists the titles
./play.sh coi                     # one game: builds if needed (regenerates when the recompiler changed)
./play.sh shk1                       # SHK1: its ROM is built from your SH1 + SHK2 dumps
./launch.sh                          # the `sh` and `dc` shelf apps (ROMs from ../roms)
./launch.sh sh                    # one shelf: sh, dc or all
```

**Save states**: the pause menu's SAVE STATE / LOAD STATE take and restore a snapshot of the
whole machine instantly (`engine/src/ps_state.c`), one per slot.

Keys: arrows, Z/X/C = A/B/C, Return = Start, Esc quits. Gamepads work by button position
(left/bottom/right face buttons = A/B/C, Start = Start), Switch Pro included.

**Rewind mode**: press Backspace or ] (on a gamepad: REWIND / FORWARD, the triggers unless set in
CONTROLLER SETUP) and the game pauses on a timeline of the
last 30 seconds (a snapshot of the whole machine every second, in memory): the selected moment
full screen, a filmstrip of its neighbours and a bar. Left goes back, Right forward to the present
(hold to scroll), A / Return plays from there, B / Esc returns to the present. The session's
recording is cut there, so it still replays to what you played. Hold REWIND to go back, FORWARD to
come forward again (Left / Right jump a second), A / Return / Start plays from the moment shown,
B / Esc returns to the present.

**Settings menu**: F1 or Tab, gamepad Back/Select (Switch: Minus) or Guide (Home), or pause
the game with Start and press **C** (a "C: SETTINGS" hint shows while paused). Aspect
ratio, widescreen enemies, low latency, volume, fullscreen, level select (Game Genie
RGST-A6XN), infinite lives/power/items (Action Replay). Saved per game.

**Share a session or crash report** (never automatic): the app's share action shows the consent text
(`engine/tools/share/consent.txt`: what is sent, where, how consent is recorded and withdrawn). Only
"I agree" packs and sends the latest run: inputs, version/settings, a scrubbed log and, if the run crashed,
only the signal name or exception code. The project validates the consent and checksums, records the report type
in its database, replays accepted sessions nightly, and includes them in `robust.sh`; withdrawn reports are
deleted. No screenshots, audio, ROM or game data are sent. The replay loop preserves failures for maintainers;
it does not make source changes on its own.

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

## 7. Every build, from a folder of your dumps

A fresh checkout needs only your own dumps, in one folder, under any names (sub-folders and zips
are fine; disc images as `.cue` + `.bin`; the Mega CD BIOS as a `.bin`):

```sh
git clone --recursive <this repository> PixelSorcery && cd PixelSorcery
make setup                              # engine/.venv + a check of the system tools
make all ROM_DIR=/path/to/your/dumps    # everything your dumps allow; the rest prints SKIP
```

**System tools** (`make setup` names any that are missing): zsh, CMake, Ninja, a C compiler, SDL2
and zlib, Python 3 with venv. macOS: `brew install cmake ninja sdl2 python`; Debian/Ubuntu: `sudo apt
install zsh cmake ninja-build gcc pkg-config libsdl2-dev zlib1g-dev python3-venv python3-tk`; Windows:
MSYS2 MinGW64 (`engine/tools/windows/setup.ps1` installs it). The Mega CD compilation disc also needs
the m68k-elf binutils (macOS: `brew install m68k-elf-binutils`).

**How the dumps are found.** `make prepare-roms` reads every file in `ROM_DIR` and recognises the
games by content (header serial and SHA-1, `engine/tools/roms.json`), not by name. Each recognised
dump is linked into `ROMS` (default `../roms`, beside the checkout) under the name the builds use
(`sh1.bin`, `coi.md`, `cd/shcd/shcd.cue`, ...); a Mega CD BIOS goes to the platform's
PixelSorcery BIOS folder (`PS_SCD_BIOS` overrides it). It then makes the images built from two
cartridges: SH3K and SHK2 (lock-on), SHK1, SHBS.
Every later step reads `ROMS` only, so `make all` can be re-run any time; it rebuilds what changed.

| Route | Output | Needs (in your dumps) |
|---|---|---|
| `make setup` | `engine/.venv`, submodule | Python 3, the tools above |
| `make prepare-roms` | `../roms` filled, lock-on images, BIOS installed | any of the games |
| `make build-all` | every cartridge port: `games/TITLE/build/` | each game's ROM |
| `make build TITLE=sh2` | one port (title code or folder) | its ROM |
| `make check TITLE=sh2` | the port, regenerated and self-checked against the original | its ROM |
| `make play TITLE=sh2` | builds if needed, then plays | its ROM |
| `make shk1-rom` | `../roms/shk1.bin`, SHK1 for a Genesis | SH1 REV00, SHK, SH2 |
| `make shbs-rom` | `../roms/shk_sh1.bin`, SHBS for a Genesis | SHK, SH1 |
| `make shcd` | `games/shcd/build/shcd_native` | a SHCD disc + its BIOS |
| `make shcd-reference` | the same plus the emulator-core reference build | a SHCD disc + its BIOS |
| `make sh-compilation` | `~/Documents/PixelSorcery/Ultimate SH (US)/ultimate.cue`, the Ultimate SH Mega CD disc (built in `build/ultimate-sh-US`) | a SHCD disc (US by default), SH1 (2, 3, SHK: their cards) |
| `make sh-compilation-all` | one disc per region of SHCD you have | SHCD discs |
| `make derived-all` | the four rows above, each when its inputs are there | |
| `make all` | setup, prepare-roms, build-all, derived-all | |
| `make launcher-macos` / `-linux` / `-windows` | the SH Collection shelf of the built ports | built ports, on that OS |
| `make package-macos` / `-linux` / `-windows` | the PixelSorcery app: DMG / .deb / Windows zip + exe (`VERSION=x.y.z`) | on that OS (MSYS2 for Windows) |

**The compilation disc** is found without arguments: the SHCD disc of `COMPILATION_REGION` (US,
the default: 60 Hz like the cartridges; EU: 50 Hz; JP: not working yet) is recognised by its hash
anywhere under `ROM_DIR` or `ROMS` (`engine/tools/megacd/find_disc.py`). Its game-select picture is
`menu.png` (or a converted `menu.bin`) in `ROM_DIR` if you have one, else a plain 4x2 card menu is
drawn (`engine/tools/megacd/menu_default.py`). Overrides, on the make command line only (a variable
left exported in your shell is ignored): `COMPILATION_DISC=/path/disc.cue`, `MENU_IMAGE=/path/menu.png`
(`-` = the plain menu), `SONIC_COMPILATION_OUT=dir`. It takes about 20 minutes (evidence runs of every
game on the tracer).

**Where to find what was made.** The finished disc (its `.cue` and tracks) and the ROMs made from your
dumps are copied to `EXPORT_DIR`, by default `Documents/PixelSorcery` (`Ultimate SH (US)/ultimate.cue`,
`shk1.bin`, `shk_sh1.bin`); open the `.cue` in OpenEmu or another Mega CD emulator, or play it
here: `games/sh1/build-trace/genrecomp_build/genrecomp_ref ~/Documents/PixelSorcery/"Ultimate SH (US)"/ultimate.cue`.
`EXPORT_DIR=-` keeps them only in the build folders. Everything they hold comes from your dumps: keep
them to yourself.

**From the build screen.** Every route above is also in the build screen: "More builds" in the
PixelSorcery app (Mac) and its window on Windows and Linux, `x` in the terminal screen
(`engine/tools/build_all.sh --tui`). Pick one (the compilation disc with its region) and Build; Show
opens the folder of what it made.

**Other variables:** `CORES` (parallel jobs, default 4), `DISC`/`SONICCD_DISC` (another SHCD
cue), `ROMS` or `PS_ROMS_DIR` (another prepared folder), `VERSION` (packages). `make help` lists
the routes.

The launcher and package routes are per operating system, not cross-compilers: the shelf holds
the ports already built on that system. macOS bundles SDL; Linux uses the system's SDL2 and zlib;
Windows bundles SDL2, zlib and the MinGW runtime.
