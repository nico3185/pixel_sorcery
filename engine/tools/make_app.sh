#!/bin/zsh
# make_app.sh TITLE [OUTDIR] — package a title's native port as a macOS app (default OUTDIR:
# games/TITLE/build). TITLE is a directory in games/ (or a unique prefix). The app's name is
# recomp.json's "title". The ROM is never bundled: on first launch the app asks for it once
# and keeps a copy in ~/Library/Application Support/PixelSorcery/TITLE/. SDL is bundled.
# Every launch is recorded there (sessions/), like ./play.sh does.
set -eu
root=${0:A:h}/../..
titles=(${(f)"$(cd $root/games && for d in */; do [[ -f $d/sidebyside.json ]] && print ${d%/}; done)"})
[[ $# -ge 1 ]] || { print "usage: make_app.sh TITLE [OUTDIR]; titles: $titles"; exit 2; }
title=$1
if (( ! ${titles[(Ie)$title]} )); then
  m=(${(M)titles:#$title*}); (( $#m == 1 )) || { print -u2 "unknown or ambiguous title '$title'"; exit 2; }
  title=$m[1]
fi
game=$root/games/$title
name=$(python3 -c "import json;print(json.load(open('$game/recomp.json'))['title'])")
exe=$(python3 -c "import json;print(json.load(open('$game/sidebyside.json')).get('exe','build/$title'))")
romname=$(python3 -c "import json;print(json.load(open('$game/sidebyside.json')).get('rom','$title.bin'))")
out=${2:-$game/build}
[[ -d $game/src/recomp && -n "$(ls $game/src/recomp 2>/dev/null)" ]] || {
  print -u2 "$title is not generated yet: run ./play.sh $title once (it generates and builds)"; exit 1; }
cmake -S $game -B $game/build -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
ninja -C $game/build >/dev/null
bin=$game/$exe

app="$out/$name.app"
rm -rf "$app"; mkdir -p "$app/Contents/MacOS" "$app/Contents/Frameworks" "$app/Contents/Resources"
cp $bin "$app/Contents/MacOS/game-bin"

# SDL2 (sdl2-compat) and the SDL3 it loads at runtime
sdl2=$(otool -L $bin | awk '/libSDL2/ {print $1}')
cp "$sdl2" "$app/Contents/Frameworks/"
install_name_tool -change "$sdl2" "@executable_path/../Frameworks/${sdl2:t}" "$app/Contents/MacOS/game-bin"
sdl3=$(ls /opt/homebrew/opt/sdl3/lib/libSDL3.0.dylib 2>/dev/null || true)
[[ -n $sdl3 ]] && cp "$sdl3" "$app/Contents/Frameworks/"
codesign --force -s - "$app/Contents/Frameworks/"*.dylib "$app/Contents/MacOS/game-bin" >/dev/null 2>&1 || true

cat > "$app/Contents/MacOS/launch" <<SH
#!/bin/zsh
# Finds the ROM (ROM=path, or the copy kept from the first launch), else asks for it.
here=\${0:A:h}
store="\$HOME/Library/Application Support/PixelSorcery/$title"
mkdir -p "\$store"
rom=\${ROM:-\$store/$romname}
if [[ ! -f \$rom ]]; then
  pick=\$(osascript -e 'POSIX path of (choose file with prompt "Choose your $name ROM (.md / .bin / .gen)")' 2>/dev/null) || exit 0
  cp "\$pick" "\$store/$romname"; rom="\$store/$romname"
fi
mkdir -p "\$store/sessions"; stamp=\$(date +%Y%m%d-%H%M%S)
export DYLD_FALLBACK_LIBRARY_PATH="\$here/../Frameworks"
export PS_POLL_RECORD="\$store/sessions/\$stamp.polls" PS_RENDERER=native PS_Z80=recomp
# open -a "$name.app" --args --resume [SESSION.polls] MINUTE
if [[ \${1:-} == --resume ]]; then
  if [[ \$# -ge 3 && \$2 == *.polls ]]; then sess=\$2; min=\$3; shift 3
  else sess=\$(ls -t "\$store"/sessions/*.polls | head -1); min=\$2; shift 2; fi
  export PS_RESUME="\$sess:\$(( \${min} * 60 * 59.9227 ))"; PS_RESUME=\${PS_RESUME%.*}
fi
exec "\$here/game-bin" "\$@" "\$rom" 2> "\$store/sessions/\$stamp.log"
SH
chmod +x "$app/Contents/MacOS/launch"

cat > "$app/Contents/Info.plist" <<PL
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleName</key><string>$name</string>
  <key>CFBundleDisplayName</key><string>$name</string>
  <key>CFBundleIdentifier</key><string>org.pixelsorcery.$title</string>
  <key>CFBundleExecutable</key><string>launch</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>0.1</string>
  <key>LSMinimumSystemVersion</key><string>13.0</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>LSApplicationCategoryType</key><string>public.app-category.games</string>
</dict></plist>
PL
codesign --force -s - "$app" >/dev/null 2>&1 || true
print "built $app"
