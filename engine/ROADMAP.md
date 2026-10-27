# Roadmap

The project-wide list is `BACKLOG.md` at the repository root (owner priorities first:
robustness, then native ports and launchers, then the Mega CD disc). This file keeps the
engine-level view.

## Done since the fork (was "Next" / "Deferred" / "Out of scope" in genrecomp)

- **Audio.** Heard and checked: a native YM2612/PSG synth (`ps_synth.c`) fed by the
  recompiled Z80 sound driver; every self-check compares the audio write stream with the
  original's.
- **Z80 recompiled.** Sound drivers are translated to C (`tools/z80recomp/`, default
  `PS_Z80=recomp`); no Z80 core runs in a native port.
- **Native machine.** The shipped runtime (`genrecomp_native`) has its own VDP, renderer,
  synth and timing; Genesis Plus GX only runs in the reference/verification builds
  (`*_oracle`, the tracer) - see `docs/architecture.md`.
- **Conformance harness.** `tools/sidebyside.py` compares RAM at every controller poll
  against the original; `tools/robust.sh`, `tools/nightly.sh` (no-LLM CI) and
  `tools/kpi.py` (coverage KPIs, `docs/kpi/history.csv`) run it over every recording.
- **68K interpreter fallback** in every title build (`src/m68k_interp.c`): code the
  recompiler never found runs interpreted, with an exact hand-over to compiled code
  (checked with `PS_INTERP=random:PCT:SEED` and `PS_INTERP=all`).
- **Return points / task switchers**: handler-rewritten and orphan returns are followed
  (`func_table.c`: CYCJ/CYCR, `g_rts_orphan`).
- **H-interrupts** and raster splits; **machine snapshots** (`src/ps_state.c`: save/load in
  place, `tools/stateval.py`).
- **macOS** is the main platform (Metal presenter, launcher apps).

## Next

- **Coverage of converted code never executed** (KPI A is 25-56% per title): a
  compiled-vs-interpreter differential tester from random machine states, and the automatic
  coverage loop (branch `wip/coverage-loop`).
- **Parallel verification from snapshots**: long recordings split into segments that start
  from the original's state at poll K, run on all cores.
- **Unmerged engine fix**: SH1's VBla_Exit (branch `wip/vblank-exit-fix`).

## Deferred

- Timing-exact DMA and the mid-frame raster effects that depend on it.
- Windows and Linux as tested platforms (they build; play-testing happens on macOS).
- Mega CD (second 68000) as a native engine: in progress for the compilation disc; 32X
  (two SH-2s) for Knuckles' Chaotix.

## Out of scope

- Selling anything built with Genesis Plus GX (its licence forbids it, see `NOTICE`); and
  sharing built games at all - they contain code translated from the player's ROMs.
