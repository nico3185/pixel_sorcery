#!/bin/zsh
# make_deb.sh [--rev REV] [--version V] [OUTDIR] - pixelsorcery_V_all.deb (default in build/deb): the
# Linux download. Like the Mac app it holds no game: the public snapshot of REV (codenames, no game
# data) without Genesis Plus GX - so native ports only, and no title still needing the emulator core
# (SHCD) - installed in /usr/share/pixelsorcery. The `pixelsorcery` command copies it to
# ~/.local/share/pixelsorcery/workspace on first run (and each new version), then opens the build
# screen (build_ui.py) or plays a title (play.sh). The compiler, cmake, ninja, SDL2, Python and
# capstone come from the distribution (Depends), so the package is the same for every architecture.
# dpkg-deb runs in a Debian container (Docker) when this machine has none.
set -eu; setopt pipefail
here=${0:A:h}; root=${here:h:h:h}; rev=HEAD; version=
[[ ${1:-} == --rev ]] && { rev=$2; shift 2; }
[[ ${1:-} == --version ]] && { version=$2; shift 2; }
out=${1:-$root/build/deb}; mkdir -p $out; out=${out:A}
[[ -n $version ]] || { version=$(git -C $root describe --tags --always $rev); version=${version#r}; }
pkg=$(mktemp -d)/pixelsorcery_${version}_all; share=$pkg/usr/share/pixelsorcery
mkdir -p $pkg/DEBIAN $pkg/usr/bin $share $pkg/usr/share/applications $pkg/usr/share/icons/hicolor/256x256/apps
print "pixelsorcery $version"
$root/engine/.venv/bin/python $root/engine/tools/public_export.py --rev $rev --out $share/payload > /dev/null
rm -rf $share/payload/engine/ext/Genesis-Plus-GX
$root/engine/.venv/bin/python -c "import sys; sys.path.insert(0, '$root/engine/tools/recompiler'); import generate as g
v = g.CYCLES; open(sys.argv[1], 'w').write(''.join(' '.join(map(str, v[i:i + 16])) + '\n' for i in range(0, len(v), 16)))" \
  $share/payload/engine/tools/recompiler/m68k_cycles.txt
for g in $share/payload/games/*(/); do
  grep -q genrecomp_native $g/CMakeLists.txt 2>/dev/null || { print "  left out: ${g:t} (needs the emulator core)"; rm -rf $g; }
done
print -r -- $version > $share/VERSION
cat > $pkg/usr/bin/pixelsorcery <<'EOF'
#!/bin/sh
# pixelsorcery            the build screen: finds your game dumps, compiles the native ports
# pixelsorcery play CODE  play a built title (codes: pixelsorcery play)
# pixelsorcery roms DIR   find the games in DIR and link them in, without the screen
set -e
ws=${XDG_DATA_HOME:-$HOME/.local/share}/pixelsorcery/workspace
ver=$(cat /usr/share/pixelsorcery/VERSION)
if [ "$(cat "$ws/.version" 2>/dev/null)" != "$ver" ]; then
  echo "pixelsorcery $ver: setting up $ws"
  mkdir -p "$ws"
  rsync -a /usr/share/pixelsorcery/payload/ "$ws/"     # earlier builds and sessions stay
  rm -rf "$ws/engine/.venv"
  python3 -m venv --system-site-packages "$ws/engine/.venv"   # capstone: python3-capstone
  echo "$ver" > "$ws/.version"
fi
mkdir -p "$ws/../roms"
cd "$ws"
case "${1:-}" in
  play) shift; exec ./play.sh "$@" ;;
  roms) exec engine/.venv/bin/python engine/tools/build_ui.py --prepare "$2" ;;
  *) exec engine/.venv/bin/python engine/tools/build_ui.py "$@" ;;
esac
EOF
chmod 755 $pkg/usr/bin/pixelsorcery
cat > $pkg/usr/share/applications/pixelsorcery.desktop <<EOF
[Desktop Entry]
Type=Application
Name=PixelSorcery
Comment=Build and play native ports of your own Mega Drive / Genesis game dumps
Exec=pixelsorcery
Icon=pixelsorcery
Terminal=true
Categories=Game;
EOF
sips -z 256 256 $root/img/sorpix.png --out $pkg/usr/share/icons/hicolor/256x256/apps/pixelsorcery.png >/dev/null
cat > $pkg/DEBIAN/control <<EOF
Package: pixelsorcery
Version: ${version//[^0-9A-Za-z.+~]/.}
Architecture: all
Maintainer: Nicolas Renard <nico3185@users.noreply.github.com>
Depends: zsh, rsync, python3, python3-venv, python3-capstone, cmake, ninja-build, gcc, libc6-dev, pkg-config, libsdl2-dev, zlib1g-dev
Section: games
Priority: optional
Homepage: https://nico3185.github.io/pixel_sorcery/
Description: native ports of your own Mega Drive / Genesis game dumps
 PixelSorcery statically recompiles Mega Drive / Genesis games (68000 and Z80
 code) to C and builds native programs with their own video and sound. No game
 is included: point it at your own dumps and it compiles them on your machine.
 Free for non-commercial use (PolyForm Noncommercial).
EOF
deb=$out/pixelsorcery_${version}_all.deb
if command -v dpkg-deb >/dev/null; then dpkg-deb --root-owner-group --build $pkg $deb >/dev/null
else docker run --rm -v ${pkg:h}:/pkg -v $out:/out debian:stable-slim dpkg-deb --root-owner-group --build /pkg/${pkg:t} /out/${deb:t} >/dev/null
fi
rm -rf ${pkg:h}
print "$deb ($(du -h $deb | cut -f1))"
