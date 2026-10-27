# Changelog

All notable changes to genrecomp. Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/);
versions follow [SemVer](https://semver.org/).

## PixelSorcery fork, 2026-10-11 to 2026-10-24 (summary; details in `git log`)

### Added
- Native runtime `genrecomp_native`: own VDP/renderer (widescreen), YM2612/PSG synth, Z80
  sound drivers recompiled to C; no emulator in a shipped port.
- 68000 interpreter fallback with an exact compiled/interpreted hand-over; `PS_INTERP=
  all|random:PCT:SEED|ADDR,...` to test it.
- Machine snapshots (`ps_state.c`): instant save/load in place; `tools/stateval.py`.
- Verification without play: `sidebyside.py` (RAM per poll vs the original), `robust.sh`,
  `nightly.sh` (no-LLM CI), `kpi.py` + `docs/kpi/history.csv` (coverage KPIs),
  `divergence.sh`, the tracer's `PS_PCCOVER` coverage bitmap, `heavy.sh` (shared throttle).
- Titles: COI, QS, QS-R1, WOI, WOI-US, SH1,
  2, 3, SHK, SH3K, SHK2, SHK1 (our own); launcher apps.

### Fixed (each found by replaying recordings against the original, none title-specific)
- ST Dn cycle count (+2 counted twice).
- A function's fall-through landing inside an instruction (jump-table target mid-instruction).
- PC-relative source of MOVEM <ea>,list and BTST #n,<ea> (Capstone's PC base off by 2).
- Return-address lookup under PS_INTERP; interrupt return PC and handler-rewritten returns at
  the hand-over; VDP status open-bus bits in interpreted reads; orphan returns.
- A runtime cheat's cheat-off path never compiled (analysis ran on the patched image).
- `check_seeds.py` refuses to drop seeds against a blank (unmirrored) ROM dump.

## [Unreleased]

### Added
- `--headless`, `--record out.mp4`, `--frames N`, `--press F:BTN[:LEN]` and
  `--ram-dump DIR[:N]` for every title via `platform_parse_args()`. (#2)
- `genrecomp_ref`: the reference runner, which plays a ROM on GPGX's 68K
  interpreter with the same flags, as ground truth to diff a recomp against. (#2)
- `func_table_dump_stack()`: a shadow call stack of 68K addresses. (#2)
- Scanline clock: bus accesses advance time, each line runs the Z80, VBlank
  calls an optional title callback, and each frame mixes audio. (#3)
- `func_table_tail()`: a tail-jump trampoline, so looping JMP/BRA chains don't
  grow the C stack. (#4)
- `tests/test_runtime.c`, run by `ctest`. (#1, #4)
- `docs/recomp-runtime.md`, `NOTICE`, `CONTRIBUTING.md`, `ROADMAP.md`, CI,
  and `Setup.cmd` / `setup.sh`. (#5)
- `func_table_jsr` / `func_table_rts`: calls push real return addresses and
  RTS follows the 68K stack, including into pushed continuations. (#6)
- `recomp_m68k_interrupt` / `genrecomp_vblank_irq`: interrupts with the
  full context restored, SR masking and VDP IE0. (#6)
- `tools/recompiler/`: the shared 68K recompiler (from Pigskin's generator)
  with per-title `recomp.json`, compiled-switch recognition, overlap
  resolution, PC-relative call following, and CMPM/MOVEP/CHK/BCD
  translation; `docs/recompiler.md`. (#7)
- `M68K_ABCD`, `M68K_SBCD`, `M68K_NBCD`. (#7)
- `GENRECOMP_WATCH=<addr>`: log every write to an address with the call
  stack. (#7)
- Windows (MSVC/vcpkg) native build: `engine/tools/setup.ps1` installs VS2022 Build
  Tools and zlib when missing and imports the vcvarsall environment before building;
  `engine/src/ps_portable.h` (`PS_ALTNAME` via `/alternatename`, `PS_INITIALIZER` via
  `.CRT$XCU`) stands in for GCC/Clang's `__attribute__((weak))` /
  `__attribute__((constructor))`, which MSVC has no equivalent for.
- A Windows crash handler (`SetUnhandledExceptionFilter` + `StackWalk64`, seeded from the
  exception's own `CONTEXT` rather than the handler's current stack): a symbolized
  backtrace with file:line instead of a bare access violation.

### Fixed
- 8/16-bit ALU macros cleared the upper bits of the destination register. (#1)
- Large VDP DMAs never completed, and the VDP dropped writes behind them. (#3)
- Stale FIFO timestamps and unmapped accesses skipped hundreds of frames. (#3)
- No controller input reached games: `input.system` was set after
  `system_init()`. (#3)
- Mid-frame H32/H40 switches were ignored. (#3)
- Stale pixels were left at the right edge after switching from H40 to H32. (#5)
- Interrupts clobbered the interrupted code's flags and ran while masked. (#6)
- `M68K_ABCD`/`M68K_SBCD` and most arithmetic macros (`ADD`/`SUB`/`ADDX`/`SUBX`/`CMP`/
  `NEG`/`NEGX`/`MULU`/`MULS`/`DIVU`/`DIVS`) declared internal temps named `_s`/`_d`/`_r`/
  `_res`/`_x`/`_q`/`_rem` — exactly the names the recompiler's own generated locals
  commonly use (e.g. `M68K_ABCD(_t, _s)` for a predecrement BCD chain). `unsigned _s =
  (uint8_t)(src)` with `src` being the caller's own `_s` self-initialized from its own
  uninitialized value instead of the caller's, silently corrupting the operation (2951
  ABCD/SBCD call sites in COI alone hit this). Found via an MSVC warning
  (C4700); it's a plain C hygiene bug, not compiler-specific. Renamed every macro's
  internal temps to a `_m68_` prefix.
- `genrecomp_native` unconditionally linked `m` (Unix libm); MSVC's CRT has it built in
  and there's no `m.lib` to link against.
- `M_PI` was used in `ps_synth.c` without being defined under MSVC, whose `math.h` only
  defines it with `_USE_MATH_DEFINES`.
- `vswhere` without `-products *` never finds a Visual-Studio-Build-Tools-only install
  (only Community/Professional/Enterprise by default).
- `PS_ITRACE`'s trace file was fully buffered, so a crash lost exactly the instructions
  you'd want to see.

### Removed
- Unused VDP/YM2612/PSG/Z80 wrapper modules, the debug framework and its
  GDB "stub" (which only printed), the debug runner, and the unused ymfm
  submodule. (#5)
- `io_read`, `io_write`, `genrecomp_io_init` and `genrecomp_io_shutdown`.
  Recompiled code reaches the I/O chip through the bus. (#5)

## Initial implementation - 2026-03-15 (untagged)

M68K CPU context, bus over the GPGX memory map, function table, SDL2
platform layer.
