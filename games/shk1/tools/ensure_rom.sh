#!/bin/zsh
# ensure_rom.sh ROMDIR — SHK1 has no cartridge: build ROMDIR/shk1.bin from the
# player's SH1 (REV00) and SHK2 dumps there, when it is missing or older than
# our code (k1.srec), the mapping (s2_to_s1.json) or the builder. Used by play.sh and launch.sh.
set -eu
g=${0:A:h:h}; roms=${1:A}
[[ -f $roms/sh1.bin && -f $roms/shk_sh2.bin ]] || exit 0     # dumps not there: nothing to do
out=$roms/shk1.bin
[[ -f $out && ! $g/k1.srec -nt $out && ! $g/s2_to_s1.json -nt $out && ! $g/tools/make_image.py -nt $out ]] && exit 0
$g/../../engine/.venv/bin/python -I $g/tools/make_image.py $roms/sh1.bin $roms/shk_sh2.bin $out
