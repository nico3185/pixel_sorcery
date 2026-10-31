#!/bin/zsh
# make_dmg.sh [--rev REV] [--version V] [OUTDIR] - PixelSorcery-VERSION.dmg (default in build/dmg): the app as a
# standalone download, unsigned. Nothing from any game is inside: the app finds the player's own
# dumps and compiles the games on their Mac. Inside the app (Contents/Resources):
#   payload/  the public snapshot of REV (engine/tools/public_export.py: codenames, no game data),
#             without Genesis Plus GX - so only native ports, and no title that still needs the
#             emulator core (SHCD) - plus SDL2 (zlib licence) in engine/ext/sdl2
#   wheels/   capstone, cmake, ninja for Apple's python3, installed offline on first run
#   setup_workspace.sh, the icon (img/sorpix.png)
# The player needs Apple's Command Line Tools (the app opens their installer) and macOS 14 on Apple
# silicon. VERSION: --version, else the newest tag reachable from REV (git describe); REV defaults to HEAD.
set -eu; setopt pipefail
here=${0:A:h}; root=${here:h:h:h}; rev=HEAD
version=
[[ ${1:-} == --rev ]] && { rev=$2; shift 2; }
[[ ${1:-} == --version ]] && { version=$2; shift 2; }
out=${1:-$root/build/dmg}; mkdir -p $out
[[ -n $version ]] || { version=$(git -C $root describe --tags --always $rev); version=${version#r}; }
stage=$(mktemp -d)/PixelSorcery; mkdir -p $stage
print "PixelSorcery $version"
# the app, then made standalone: no checkout path, its version, the payload
$here/make_app.sh $stage > /dev/null
app=$stage/PixelSorcery.app; res=$app/Contents/Resources
plutil -remove PSRepo $app/Contents/Info.plist
plutil -replace CFBundleShortVersionString -string $version $app/Contents/Info.plist
print "  payload: public snapshot of $rev"
$root/engine/.venv/bin/python $root/engine/tools/public_export.py --rev $rev --out $res/payload > /dev/null
rm -rf $res/payload/engine/ext/Genesis-Plus-GX
# the recompiler's 68000 timing table (documented instruction timings), read from the core's header
$root/engine/.venv/bin/python -c "import sys; sys.path.insert(0, '$root/engine/tools/recompiler'); import generate as g
v = g.CYCLES; open(sys.argv[1], 'w').write(''.join(' '.join(map(str, v[i:i + 16])) + '\\n' for i in range(0, len(v), 16)))" \
  $res/payload/engine/tools/recompiler/m68k_cycles.txt
for g in $res/payload/games/*(/); do
  grep -q genrecomp_native $g/CMakeLists.txt 2>/dev/null || { print "  left out: ${g:t} (needs the emulator core)"; rm -rf $g; }
done
print "  SDL2"
sdl=$res/payload/engine/ext/sdl2; s2=$(brew --prefix sdl2-compat) s3=$(brew --prefix sdl3)
mkdir -p $sdl/lib; cp -R $s2/include $sdl/; cp -R $s2/lib/cmake $sdl/lib/; cp $s2/lib/*.a $sdl/lib/
cp $s2/lib/libSDL2-2.0.0.dylib $sdl/lib/; ln -sf libSDL2-2.0.0.dylib $sdl/lib/libSDL2.dylib; ln -sf libSDL2-2.0.0.dylib $sdl/lib/libSDL2-2.0.dylib
cp $s3/lib/libSDL3.0.dylib $sdl/lib/libSDL3.dylib                 # sdl2-compat loads @loader_path/libSDL3.dylib
chmod u+w $sdl/lib/*.dylib
codesign --remove-signature $sdl/lib/*.dylib      # re-signed below, after the new install names
install_name_tool -id @rpath/libSDL2-2.0.0.dylib $sdl/lib/libSDL2-2.0.0.dylib
install_name_tool -id @rpath/libSDL3.dylib $sdl/lib/libSDL3.dylib
codesign -s - -f $sdl/lib/*.dylib 2>/dev/null
cp $s3/share/doc/sdl3/LICENSE.txt $sdl/LICENSE.txt 2>/dev/null || cp $(dirname $(dirname $(readlink -f $s3/lib/libSDL3.0.dylib)))/LICENSE.txt $sdl/LICENSE.txt 2>/dev/null || true
print "  Python wheels"
cache=$HOME/Library/Caches/PixelSorcery/wheels; mkdir -p $cache
PIP_DISABLE_PIP_VERSION_CHECK=1 $root/engine/.venv/bin/python -m pip download -q --only-binary=:all: --platform macosx_11_0_arm64 \
  --python-version 3.9 --implementation cp -d $cache capstone cmake ninja
mkdir -p $res/wheels; cp $cache/*.whl $res/wheels/
cp $here/setup_workspace.sh $res/
codesign -s - --force --deep $app >/dev/null 2>&1 || true
# the disk image: the app, Applications, a read-me, the icon as the volume's
ln -s /Applications $stage/Applications
cat > $stage/README.txt <<EOF
PixelSorcery $version

Drag PixelSorcery to Applications, then open it.

The app is not signed. The first time, macOS blocks it: open System Settings > Privacy &
Security and click "Open Anyway" (or run: xattr -dr com.apple.quarantine /Applications/PixelSorcery.app).

PixelSorcery contains no game. Point it at the folder holding your own game dumps: it recognises
them, compiles each game for your Mac (Apple's Command Line Tools are needed, the app offers to
install them), and plays them. Everything it makes stays in
~/Library/Application Support/PixelSorcery.

Requires macOS 14 or later on Apple silicon. Free for non-commercial use (PolyForm Noncommercial).
EOF
cp $res/PixelSorcery.icns $stage/.VolumeIcon.icns
dmg=$out/PixelSorcery-$version.dmg; rm -f $dmg
tmp=${stage:h}/rw.dmg
hdiutil create -quiet -volname PixelSorcery -srcfolder $stage -ov -format UDRW $tmp
att=$(diskutil image attach --mountOptions nobrowse $tmp | grep -m1 '/Volumes/')   # /dev/diskN<TAB>hint<TAB>/Volumes/...
dev=${att%%[[:space:]]*}; mnt=/Volumes/${att#*/Volumes/}
SetFile -a C "$mnt"                                  # the volume shows .VolumeIcon.icns
diskutil eject $dev > /dev/null
hdiutil convert -quiet $tmp -format UDZO -o $dmg
rm -rf ${stage:h}
print "$dmg ($(du -h $dmg | cut -f1))"
