#!/bin/zsh
# make_zip.sh [--rev REV] [--version V] [OUTDIR] - PixelSorcery-V-windows.zip (default in build/win): the
# Windows download, the same payload as the Linux .deb and the Mac app: the public snapshot of REV
# (codenames, no game data) without Genesis Plus GX, so native ports only, plus m68k_cycles.txt (the
# generator's cycle table, otherwise read from the core's sources). The player runs
# engine\tools\windows\install.ps1 once (MSYS2 + toolchain), then PixelSorcery.cmd. Beside it: its SHA-256.
set -eu; setopt pipefail
here=${0:A:h}; root=${here:h:h:h}; rev=HEAD; version=
[[ $OSTYPE == (msys|cygwin)* ]] && root=$(cygpath -m $root)
[[ ${1:-} == --rev ]] && { rev=$2; shift 2; }
[[ ${1:-} == --version ]] && { version=$2; shift 2; }
out=${1:-$root/build/win}; mkdir -p $out; out=$(cd $out && pwd)   # not :A (mangles C:/ paths)
[[ -n $version ]] || { version=$(git -C $root describe --tags --always $rev); version=${version#[rv]}; }
name=PixelSorcery-$version-windows; tmp=$(mktemp -d); pay=$tmp/$name
print "PixelSorcery $version (Windows)"
$root/engine/.venv/bin/python $root/engine/tools/public_export.py --rev $rev --out $pay > /dev/null
rm -rf $pay/engine/ext/Genesis-Plus-GX $pay/engine/patches/genesis-plus-gx.patch   # no core code
$root/engine/.venv/bin/python -c "import sys; sys.path.insert(0, sys.argv[1]); import generate as g
v = g.CYCLES; open(sys.argv[2], 'w', newline='\n').write(''.join(' '.join(map(str, v[i:i + 16])) + '\n' for i in range(0, len(v), 16)))" \
  $root/engine/tools/recompiler $pay/engine/tools/recompiler/m68k_cycles.txt
for g in $pay/games/*(/); do
  grep -q genrecomp_native $g/CMakeLists.txt 2>/dev/null || { print "  left out: ${g:t} (needs the emulator core)"; rm -rf $g; }
done
print -r -- $version > $pay/VERSION
gcc -O2 -s -mwindows -o $pay/PixelSorcery.exe $root/engine/tools/windows/pixelsorcery.c   # the entry point (no console)
rm -f $out/$name.zip; (cd $tmp && zip -qr $out/$name.zip $name)
(cd $out && sha256sum $name.zip > $name.zip.sha256)
# the single-file download: PixelSorcery.exe + the zip + its size (8 bytes LE) + PSPAYLD1 (pixelsorcery.c)
python -c "import sys,struct; e=open(sys.argv[1],'rb').read(); z=open(sys.argv[2],'rb').read(); open(sys.argv[3],'wb').write(e+z+struct.pack('<Q',len(z))+b'PSPAYLD1')" $pay/PixelSorcery.exe $out/$name.zip $out/$name.exe
(cd $out && sha256sum $name.exe > $name.exe.sha256)
rm -rf $tmp
print "$out/$name.zip ($(du -h $out/$name.zip | cut -f1)), $out/$name.exe ($(du -h $out/$name.exe | cut -f1))"
