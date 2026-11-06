#!/bin/zsh
# release.sh VERSION [--rev REV] [--repo OWNER/NAME] [--smoke CODE] [--no-upload] - the Windows release,
# run on a Windows machine in MSYS2 MinGW64 (zsh, gcc, gh logged in):
#   1. make_zip.sh: PixelSorcery-VERSION-windows.zip (+ .sha256) in build/win
#   2. smoke test from a fresh unzip, as a player: ROMs prepared from ../roms (the player's own dumps,
#      never in the package), the title CODE (default coi) generated + built, then 1200 headless
#      frames that must end cleanly. Nothing is uploaded if this fails.
#   3. the zip and its SHA-256 attached to release vVERSION of OWNER/NAME (default the public
#      repository), created on its main branch if it does not exist yet (the Mac DMG and the Linux
#      .deb are added to the same release by their own scripts).
set -eu; setopt pipefail
here=${0:A:h}; root=${here:h:h:h}
[[ $OSTYPE == (msys|cygwin)* ]] && root=$(cygpath -m $root)
version=${1:?usage: release.sh VERSION [--rev REV] [--repo OWNER/NAME] [--smoke CODE] [--no-upload]}; shift
rev=HEAD; repo=nico3185/pixel_sorcery; smoke=coi; upload=1
while (( $# )); do case $1 in
  --rev) rev=$2; shift 2;; --repo) repo=$2; shift 2;; --smoke) smoke=$2; shift 2;;
  --no-upload) upload=0; shift;; *) print -u2 "unknown option $1"; exit 2;; esac; done
name=PixelSorcery-$version-windows; out=$root/build/win; roms=$root/../roms
command -v gh >/dev/null || path+=("/c/Program Files/GitHub CLI")   # gh installed on the Windows side
[[ -z ${GH_CONFIG_DIR:-} && -d "${APPDATA:-/nonexistent}/GitHub CLI" ]] && export GH_CONFIG_DIR="$APPDATA/GitHub CLI"   # its Windows login
zsh $here/make_zip.sh --rev $rev --version $version $out
# 2. smoke test: next to ../roms, so the build screen finds the player's dumps there
st=$root/../.ps-smoke-$version
[[ -e $st ]] && { print -u2 "$st exists: remove it first"; exit 1; }
mkdir -p $st && (cd $st && unzip -q $out/$name.zip)
tree=$st/$name
mkdir -p $tree/engine/.venv/bin
printf '#!/bin/sh\nexport PYTHONUTF8=1\nexec /mingw64/bin/python "$@"\n' > $tree/engine/.venv/bin/python; chmod +x $tree/engine/.venv/bin/python
(cd $tree && zsh engine/tools/build_all.sh --roms $roms $smoke) | tail -3
exe=$(cd $tree/games/$smoke && python -c "import json,sys;sys.stdout.write(json.load(open('sidebyside.json'))['exe'])")
rom=$(cd $tree/games/$smoke && python -c "import json,sys;sys.stdout.write(json.load(open('sidebyside.json'))['rom'])")
(cd $tree/games/$smoke && timeout 300 ./$exe.exe --headless --frames 1200 $(cat selfcheck.args 2>/dev/null) $tree/../roms/$rom >/dev/null 2>&1) \
  || { print -u2 "smoke test FAILED: $smoke (tree kept in $st)"; exit 1; }
# sound: the same 1200 frames into a WAV, which must not be silent (a game built without its Z80 driver is)
(cd $tree/games/$smoke && PS_WAV=build/smoke.wav timeout 300 ./$exe.exe --headless --frames 1200 $(cat selfcheck.args 2>/dev/null) $tree/../roms/$rom >/dev/null 2>&1)
rms=$(python - $tree/games/$smoke/build/smoke.wav <<'PY'
import array, math, sys
d = open(sys.argv[1], 'rb').read(); i = d.find(b'data'); a = array.array('h'); a.frombytes(d[i + 8:][:(len(d) - i - 8) // 2 * 2])
print(int(math.sqrt(sum(x * x for x in a) / max(1, len(a)))))
PY
)
rms=${rms%$'\r'}
(( ${rms:-0} >= 100 )) || { print -u2 "smoke test FAILED: $smoke is silent (rms ${rms:-none}, tree kept in $st)"; exit 1; }
print "smoke test OK: $smoke built and ran 1200 frames from the unzipped download, sound rms $rms"
print "(the smoke-test tree is in $st: it holds code made from your ROM - delete it when done)"
(( upload )) || exit 0
# 3. the GitHub release
files=($out/$name.exe $out/$name.exe.sha256 $out/$name.zip $out/$name.zip.sha256)
if gh release view v$version -R $repo >/dev/null 2>&1; then
  gh release upload v$version -R $repo --clobber $files
else
  gh release create v$version -R $repo --target main --title "PixelSorcery $version - Windows app" \
    --notes "**Windows** (10/11, 64-bit): download **$name.exe** and run it. One file: the first run installs PixelSorcery in your user folder and the compiler toolchain (MSYS2, about 600 MB from its servers), adds it to the Start menu and opens the app - **Build games...** (your own dumps, recognised by checksum), then **Your games**: Play, Controller..., Collections, Share last session.... The .zip holds the same files. No game is included. Not affiliated with or endorsed by Sega or Disney." \
    $files
fi
print "released: $repo v$version ($name.exe, $name.zip)"
