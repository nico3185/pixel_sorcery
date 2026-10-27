# TAS movies as end-to-end tests

A tool-assisted speedrun plays a whole game, frame-perfectly. Replayed on the original
(the GPGX oracle) and on the recompiled port, it checks every level, boss and cutscene
the run visits, not only the stretches a scripted bot reaches.

The port and the oracle are compared per controller poll (`ps_poll.c`): the n-th
time the game reads the pad, both get the same buttons and their 68K RAM must match.
So a movie has to be turned into **polls** once, on a core where it plays in sync.

## Which movies sync

- **BizHawk** movies (`.bk2`) recorded with the Genplus-gx core. BizHawk 1.11's
  core predates GPGX's December 2014 frame timing rewrite (ec554b4: "frame emulation
  now starts with vertical blanking"). On today's GPGX such a movie presses its
  buttons a few frames off and drifts at the first load: QS 3323M misses the
  title screen's START by 3 frames, then can't skip the story. `tools/oldcore` builds
  GPGX as it was just before that commit, and there the run plays to the end.
- **Gens** movies (`.gmv`) desync on any GPGX: different core, different timing.
- The movie's ROM must be the exact revision (check `Header.txt` in the .bk2: its
  `SHA1` field holds the ROM's MD5). QS's TAS is on REV01, a different build
  from REV00, so it has its own title directory (`games/qs-r1`).

## Steps

```
python3 tools/bk2_to_movie.py run.bk2 run.txt          # .bk2 -> one "P1 P2" line per frame
tools/oldcore/build.sh build/oldcore                   # once: GPGX 8744c39 + oldrun
build/oldcore/oldrun ROM run.txt run.polls 70000 FF18  # polls; prints the watched word's changes
python tools/sidebyside.py ROM GAMEDIR run.polls --max-iter 8
```

`oldrun` takes `-ppm DIR:EVERY` for screenshots, to see that the run really plays.
The polls are timing-free: the current oracle (`genrecomp_trace` with
`PS_POLL_REPLAY`) plays QS 3323M to the ending from them, like the old core.
`sidebyside.py` then replays them on the port, seeds every function the original ran
that the port lacks, and rebuilds until the two agree at every poll.

A new title can start from the same polls: replayed on `genrecomp_trace` with
`--trace-out seeds.txt`, they give every entry point the run executes
(`tools/merge_seeds.py` puts them in `recomp.json`).
