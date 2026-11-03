#!/bin/zsh
# z80_drivers.sh DISC.cue: SHCD's Z80 sound drivers -> C for the native build (no Z80 emulation).
# The native build runs the BIOS and the disc to the first stage (stage_movie.py: BIOS menu Start,
# CD-ROM; the stage select's first entry) and saves Z80 RAM at every /RESET release whose RAM changed
# (PS_Z80_CAPTURE); z80_drivers.py recompiles each driver (the BIOS's, the game's) into
# src/recomp/z80_*.c (git-ignored: from the player's BIOS and disc), then shcd_native is rebuilt
# with them. No emulator core involved (the Mac/Linux apps run it).
g=${0:A:h:h}; cue=${1:?usage: z80_drivers.sh DISC.cue}; cue=${cue:A}
cd $g || exit 1
py=${g:h:h}/engine/.venv/bin/python; [[ -x $py ]] || py=python3
[[ -f build/build.ninja ]] || cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null || exit 1
ninja -C build shcd_native >/dev/null || exit 1
cap=build/z80cap.$$; mkdir -p $cap
$py tools/stage_movie.py 0 3000 $cap/movie.txt
GENRECOMP_MOVIE=$cap/movie.txt PS_Z80_CAPTURE=$cap/img%d.bin \
  build/shcd_native --headless --frames 3000 $cue 2>&1 | grep 'z80 capture'
$py tools/z80_drivers.py $cap src/recomp || exit 1
ninja -C build shcd_native >/dev/null || exit 1
print "z80 drivers: src/recomp/z80_*.c, shcd_native rebuilt (captures in $cap)"
