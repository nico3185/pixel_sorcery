#!/bin/zsh
# make_launcher.sh CONFIG [OUTDIR] — package a shelf of titles (launchers/sh.txt, launchers/dc.txt)
# as one macOS app: the launcher (tools/launcher/launcher.c), every title's native port and SDL.
# The app's name is CONFIG's "name"; default OUTDIR: launchers/build. No ROM is bundled: the shelf
# asks for each ROM once (the same copy as make_app.sh's apps, ~/Library/Application Support/
# PixelSorcery/TITLE/) and captures each cartridge's label and attract clip from it there.
set -eu
root=${0:A:h}/../..
[[ $# -ge 1 ]] || { print "usage: make_launcher.sh CONFIG [OUTDIR]"; exit 2; }
cfg=${1:A}
name=$(awk '$1 == "name" { $1 = ""; sub(/^ /, ""); print; exit }' $cfg)
out=${2:-$root/launchers/build}
# per cartridge: ID and the port it runs (the optional 8th field: a lock-on cartridge runs another
# title's port with its own ROM, e.g. SHBS = shk + the SHK / SH1 lock-on)
ids=(${(f)"$(awk -F'|' '$1 ~ /^game / { sub(/^game /, "", $1); gsub(/ /, "", $1); p = $8; gsub(/ /, "", p); print $1 ":" (p == "" ? $1 : p) }' $cfg)"})

app="$out/$name.app"
rm -rf "$app"; mkdir -p "$app/Contents/MacOS" "$app/Contents/Frameworks" "$app/Contents/Resources"
first=
for entry in $ids; do
  id=${entry%%:*}; game=$root/games/${entry#*:}
  [[ ! -f $game/recomp.json || -d $game/src/recomp && -n "$(ls $game/src/recomp 2>/dev/null)" ]] || {
    print -u2 "$id is not generated yet: run ./play.sh $id once (it generates and builds)"; exit 1; }
  cmake -S $game -B $game/build -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
  ninja -C $game/build >/dev/null
  exe=$(python3 -c "import json;print(json.load(open('$game/sidebyside.json')).get('exe','build/${game:t}'))")
  cp $game/$exe "$app/Contents/MacOS/$id"
  [[ -n $first ]] || first=$game
done
ninja -C $first/build ps_launcher >/dev/null
cp $first/build/genrecomp_build/ps_launcher "$app/Contents/MacOS/ps_launcher"
cp $cfg "$app/Contents/Resources/launcher.txt"

# SDL2 (sdl2-compat) and the SDL3 it loads at runtime, shared by the launcher and the titles
sdl2=$(otool -L "$app/Contents/MacOS/ps_launcher" | awk '/libSDL2/ {print $1}')
cp "$sdl2" "$app/Contents/Frameworks/"
for b in "$app/Contents/MacOS/"*; do
  [[ -x $b ]] || continue
  s=$(otool -L "$b" | awk '/libSDL2/ {print $1}')
  [[ -n $s ]] && install_name_tool -change "$s" "@executable_path/../Frameworks/${sdl2:t}" "$b"
done
sdl3=$(ls /opt/homebrew/opt/sdl3/lib/libSDL3.0.dylib 2>/dev/null || true)
[[ -n $sdl3 ]] && cp "$sdl3" "$app/Contents/Frameworks/"

# the icon: a blank cartridge with the family's name (nothing from a ROM)
ico=$(mktemp -d)
PS_LAUNCHER_ICON=$ico/cart.bmp PS_LAUNCHER_HOME=$ico "$app/Contents/MacOS/ps_launcher" $cfg "$app/Contents/MacOS" >/dev/null 2>&1 || true
if [[ -f $ico/cart.bmp ]] && command -v iconutil >/dev/null; then
  mkdir -p $ico/AppIcon.iconset
  for sz in 16 32 64 128 256 512 1024; do
    sips -s format png -z $sz $sz $ico/cart.bmp --out $ico/s$sz.png >/dev/null 2>&1
  done
  for sz in 16 32 128 256 512; do
    cp $ico/s$sz.png $ico/AppIcon.iconset/icon_${sz}x${sz}.png
    cp $ico/s$((sz * 2)).png $ico/AppIcon.iconset/icon_${sz}x${sz}@2x.png
  done
  iconutil -c icns $ico/AppIcon.iconset -o "$app/Contents/Resources/AppIcon.icns" 2>/dev/null || true
fi
rm -rf $ico

cat > "$app/Contents/MacOS/launch" <<'SH'
#!/bin/zsh
here=${0:A:h}
export DYLD_FALLBACK_LIBRARY_PATH="$here/../Frameworks"
mkdir -p "$HOME/Library/Application Support/PixelSorcery"
exec "$here/ps_launcher" "$here/../Resources/launcher.txt" "$here" "$@" \
  2> "$HOME/Library/Application Support/PixelSorcery/launcher.log"
SH
chmod +x "$app/Contents/MacOS/launch"
id=org.pixelsorcery.launcher.${${cfg:t:r}:l}
cat > "$app/Contents/Info.plist" <<PL
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleName</key><string>$name</string>
  <key>CFBundleDisplayName</key><string>$name</string>
  <key>CFBundleIdentifier</key><string>$id</string>
  <key>CFBundleExecutable</key><string>launch</string>
  <key>CFBundleIconFile</key><string>AppIcon</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>0.1</string>
  <key>LSMinimumSystemVersion</key><string>13.0</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>LSApplicationCategoryType</key><string>public.app-category.games</string>
</dict></plist>
PL
codesign --force -s - "$app/Contents/Frameworks/"*.dylib "$app/Contents/MacOS/"* >/dev/null 2>&1 || true
codesign --force -s - "$app" >/dev/null 2>&1 || true
print "built $app"
