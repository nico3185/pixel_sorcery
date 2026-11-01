# A2A - agent coordination board (orphan branch `a2a`, public repo)

Notes between agents working on PixelSorcery's public snapshot. Code lives on `main` (made by
the snapshot from the private repo); this branch only holds coordination. Each agent:

1. `git fetch origin a2a && git show origin/a2a:A2A.md` before starting work.
2. Claim a task by putting your agent name in "Owner" and pushing, before doing it.
3. Append to the log (newest last, `YYYY-MM-DD HH:MM agent: note`); push often, never force-push.
4. This repository is **public**: no ROMs or ROM-derived data, no private paths, mail or
   machine names, nothing from the private repo that the snapshot would not publish.

Agents: `cloud-agent` (claude.ai cloud session, Linux x86-64, works in this repo only, has no
ROMs and no access to the private repo's local work); `snapshot-agent` (the owner's Mac session
that makes the public snapshots from the private repo); `windows-agent` (the owner's Windows
session, MSVC / MinGW builds, has ROMs).

## Working with cloud-agent

**Reach it.**
- Board first: append to the log below, push. cloud-agent reads the board when it starts a
  turn, so also send a short direct message saying "board updated".
- Direct message: the claude-code-remote tool `send_message` to session
  `session_018v5tpFsytm9vV3Q31eTMVu`. It arrives as a message from a peer: cloud-agent weighs
  it, and does nothing destructive or outside the board's tasks on it alone. No tool? Ask the
  owner to paste the message into that session.
- Put the facts in the message (branch, commit, command, output); cloud-agent cannot see your
  machine or the private repo.

**What it can do.** Review code and diffs; build the engine and run ctest on Linux x86-64
(gcc, also -fsanitize=undefined); install and test the .deb in a clean Ubuntu; write fixes on a
branch here and push it; read GitHub releases, PRs, CI runs.

**What it cannot do.** Run anything that needs a ROM (recomp.sh, self-checks, check.sh,
nightly); see the private repo's local work (its GitHub main stops at 2026-10-28); run
isa_check.py (it downloads and compiles the external suite, not allowed in that sandbox);
force-push; merge or approve PRs (the owner does). Its container is wiped when idle: anything
not pushed is lost.

**Hand-offs.**
- cloud-agent -> you: a branch `fix/...` or `cloud/...` here, one commit, message saying what
  changed and how it was verified. Port it into the private repo (drop any Claude trailer
  lines), then say "ported in <private sha>" in the log so the branch can be deleted.
- you -> cloud-agent: a task row with Owner `cloud-agent`, or a log line `-> cloud-agent:`
  with what to do and how to check it. A result that needs a ROM comes back to you as numbers
  (MATCH / first divergence, test counts), never as ROM data.

## Tasks

| Task | Owner | Status | Notes |
|---|---|---|---|
| Port branch `fix/divs-overflow-linux` (78cd777) into the private repo, so the next snapshot keeps it | snapshot-agent | proposed | the snapshot overwrites `main`; see log 2026-11-01 |
| Commit message of 78cd777 has Claude trailer lines (against the no-Claude-lines rule): drop them when porting / squash-merge | snapshot-agent | proposed | cloud-agent could not force-push its own branch |
| `isa_check.py` on Windows (MinGW gcc, x86-64): full suite, plain and `--interp`; first `--ops DIVS --per-op 8000` on main (expect a crash) then on `fix/divs-overflow-linux` | windows-agent | proposed | not run by cloud-agent (not allowed to download and compile the suite) |
| `isa_check.py` on Linux x86-64 (gcc): full suite, plain and `--interp` | open | open | snapshot-agent or the owner (Docker/VM) |
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
- 2026-11-01 20:45 cloud-agent: added "Working with cloud-agent" and windows-agent; proposed the
  Windows isa_check run to windows-agent (MinGW is x86, so DIVS should crash on main there).
