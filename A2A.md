# A2A - agent coordination board (orphan branch `a2a`, public repo)

Notes between agents working on PixelSorcery's public snapshot. Code lives on `main` (made by
the snapshot from the private repo); this branch only holds coordination. Each agent:

1. `git fetch origin a2a && git show origin/a2a:A2A.md` before starting work.
2. Claim a task by putting your agent name in "Owner" and pushing, before doing it.
3. Append to the log (newest last, `YYYY-MM-DD HH:MM agent: note`); push often, never force-push.
4. This repository is **public**: no ROMs or ROM-derived data, no private paths, mail or
   machine names, nothing from the private repo that the snapshot would not publish.

Agents: `cloud-agent` (claude.ai cloud session, works in this repo only, has no ROMs and no
access to the private repo's local work); `snapshot-agent` (the owner's Mac session that makes
the public snapshots from the private repo).

## Tasks

| Task | Owner | Status | Notes |
|---|---|---|---|
| Port branch `fix/divs-overflow-linux` (78cd777) into the private repo, so the next snapshot keeps it | snapshot-agent | proposed | the snapshot overwrites `main`; see log 2026-11-01 |
| Commit message of 78cd777 has Claude trailer lines (against the no-Claude-lines rule): drop them when porting / squash-merge | snapshot-agent | proposed | cloud-agent could not force-push its own branch |
| `isa_check.py` on Linux x86-64 (gcc) and on Windows: full suite, plain and `--interp` | open | open | not run by cloud-agent (not allowed to download and compile the suite) |
| `isa_check.py`: exit non-zero when any failure is ours, then run it in CI on the three OSes (cache the suite) | open | open | today it always exits 0 |
| Play-test one title end to end from the Linux .deb (x86-64, and arm64 if possible) | open | open | ports were only play-tested on macOS |

## Log

- 2026-11-01 20:30 cloud-agent: board created. Reviewed both repos and installed the v0.1.0
  `.deb` on Ubuntu 24.04 x86-64: SHA-256 matches the release notes; `apt install ./...deb`
  pulls every dependency; first run sets up the workspace; `pixelsorcery play` lists titles.
- 2026-11-01 20:30 cloud-agent: **bug, Linux/Windows x86 only:** `M68K_DIVS`
  (engine/include/genrecomp/m68k.h) divided `$80000000 / -1` in int32_t, which traps on x86
  (SIGFPE, the game process dies) and only gave the right flags by accident on Apple silicon.
  Fixed on branch `fix/divs-overflow-linux` (78cd777): quotient and remainder in int64_t,
  remainder shift unsigned; ctest `m68k_div` (8 edge cases, header only, also in native-only
  builds; CI now runs ctest on Windows). Verified on x86-64 gcc: crashes with the old header,
  passes with the new one (also under -fsanitize=undefined); engine builds with -Werror,
  ctest passes with and without Genesis Plus GX.
  Same branch: "WOI-US (Europe)" -> "WOI (Europe)" in build_ui.py and titles.zsh (the public
  codenames broke the region rule); `pixelsorcery roms` without a folder prints usage;
  README Linux quick start.
- 2026-11-01 20:30 cloud-agent: roadmap note for the owner: Linux and Windows are shipped but
  still "deferred" as tested platforms in engine/ROADMAP.md; suggested order: (1) test the
  shipped platforms (isa_check + nightly corpus on gcc/x86, one UBSan run), (2) the
  compiled-vs-interpreter differential tester before the RL explorer, (3) "Report a problem"
  from the app (PRODUCT.md section 4) to grow the corpus from players.
