#!/bin/zsh
# make_ultimate.sh [ROMS] - Ultimate SH, native (BACKLOG 0.7.5): ONE executable holding every Sonic port that is
# generated here (games/ID/src/recomp: ./play.sh ID once) and the Mega CD compilation's game-select screen
# (launchers/ultimate.txt, the launcher's disc look). ROMS: where your dumps and menu.png are (default roms/,
# else ../roms). The screen's config keeps the games the executable holds; menu.png becomes its menu.bmp.
# Packaged for this OS: macOS launchers/build/Ultimate SH.app, Linux launchers/build/linux/Ultimate SH/,
# Windows launchers/build/windows/Ultimate SH/. Nothing of a ROM is in it but the code generated from it:
# keep it to yourself.
set -eu
root=${0:A:h:h:h:h}
roms=${1:-$root/roms}; [[ -d $roms ]] || roms=$root/../roms
roms=${roms:A}
cfg=$root/launchers/ultimate.txt
py=$root/engine/.venv/bin/python; [[ -x $py ]] || py=python3
titles=()
for t in sh1 shk1 sh2 shk2 sh3 shk sh3k shcd; do
  [[ -n "$(print -l $root/games/$t/src/recomp/*.c(N))" ]] && titles+=($t)
done
(( $#titles )) || { print -u2 "no Sonic port is generated yet: ./play.sh sh1 (or make build TITLE=sh1) first"; exit 1; }
b=$root/build/ultimate
print "1/3 one executable: ${titles[*]}"
cmake -S $root/engine/tools/ultimate -B $b -G Ninja -DCMAKE_BUILD_TYPE=Release "-DULTIMATE_TITLES=${(j:;:)titles}" >/dev/null
ninja -C $b ultimate_sh | grep -v "^\[" || true
exe=$b/ultimate_sh; [[ -x $exe ]] || exe=$b/ultimate_sh.exe
held=" ${(f)"$($exe --list)"} "; held=" ${held//$'\n'/ } "
print "2/3 the screen: $(echo $held | wc -w | tr -d ' ') titles held"
case $(uname -s) in
  Darwin) app="$root/launchers/build/Ultimate SH.app"; bin="$app/Contents/MacOS"; res="$app/Contents/Resources";;
  MINGW*|MSYS*) app="$root/launchers/build/windows/Ultimate SH"; bin=$app; res=$app;;
  *) app="$root/launchers/build/linux/Ultimate SH"; bin=$app; res=$app;;
esac
rm -rf "$app"; mkdir -p "$bin" "$res"
# the config with the held titles only (a stack reduced to them); soon/app cards kept
awk -v keep="$held" -F'|' '
  $1 ~ /^game / { p = $8; gsub(/ /, "", p); if (p == "") { p = $1; sub(/^game /, "", p); gsub(/ /, "", p) }
                  if (index(keep, " " p " ")) { print; id = $1; sub(/^game /, "", id); gsub(/ /, "", id); have = have " " id " " } next }
  $1 ~ /^stack / { n = split($0, w, " "); s = "stack"; c = 0; for (i = 2; i <= n; i++) if (index(have, " " w[i] " ")) { s = s " " w[i]; c++ } if (c > 1) print s; next }
  { print }' $cfg > "$res/ultimate.txt"
for m in $roms/menu.png $root/roms/menu.png; do                 # the player's picture, as a BMP for SDL
  [[ -f $m ]] || continue
  $py -c "import sys; from PIL import Image; Image.open(sys.argv[1]).convert('RGB').save(sys.argv[2])" $m "$res/menu.bmp" && break
done
print "3/3 package: $app"
case $(uname -s) in
  Darwin)
    cp $exe "$bin/ultimate_sh"
    mkdir -p "$app/Contents/Frameworks"
    sdl2=$(otool -L "$bin/ultimate_sh" | awk '/libSDL2/ {print $1}')
    if [[ -n $sdl2 ]]; then
      cp "$sdl2" "$app/Contents/Frameworks/"
      install_name_tool -change "$sdl2" "@executable_path/../Frameworks/${sdl2:t}" "$bin/ultimate_sh" 2>/dev/null
    fi
    sdl3=$(ls /opt/homebrew/opt/sdl3/lib/libSDL3.0.dylib 2>/dev/null || true)
    [[ -n $sdl3 ]] && cp "$sdl3" "$app/Contents/Frameworks/"
    codesign -f -s - "$bin/ultimate_sh" 2>/dev/null || true
    ico=$(mktemp -d)                                                # the icon: a blank cartridge, the name
    PS_LAUNCHER_ICON=$ico/cart.bmp PS_LAUNCHER_HOME=$ico "$bin/ultimate_sh" >/dev/null 2>&1 || true
    if [[ -f $ico/cart.bmp ]] && command -v iconutil >/dev/null; then
      mkdir -p $ico/AppIcon.iconset
      for sz in 16 32 64 128 256 512 1024; do sips -s format png -z $sz $sz $ico/cart.bmp --out $ico/s$sz.png >/dev/null 2>&1; done
      for sz in 16 32 128 256 512; do
        cp $ico/s$sz.png $ico/AppIcon.iconset/icon_${sz}x${sz}.png; cp $ico/s$((sz * 2)).png $ico/AppIcon.iconset/icon_${sz}x${sz}@2x.png
      done
      iconutil -c icns $ico/AppIcon.iconset -o "$res/AppIcon.icns" 2>/dev/null || true
    fi
    rm -rf $ico
    cat > "$bin/launch" <<SH
#!/bin/zsh
here=\${0:A:h}
export DYLD_FALLBACK_LIBRARY_PATH="\$here/../Frameworks"
mkdir -p "\$HOME/Library/Application Support/PixelSorcery"
exec "\$here/ultimate_sh" --roms "$roms" "\$@" 2> "\$HOME/Library/Application Support/PixelSorcery/ultimate.log"
SH
    chmod +x "$bin/launch"
    cat > "$app/Contents/Info.plist" <<PL
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleName</key><string>Ultimate SH</string>
  <key>CFBundleDisplayName</key><string>Ultimate SH</string>
  <key>CFBundleIdentifier</key><string>org.pixelsorcery.ultimate</string>
  <key>CFBundleExecutable</key><string>launch</string>
  <key>CFBundleIconFile</key><string>AppIcon</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>LSMinimumSystemVersion</key><string>14.0</string>
  <key>NSHighResolutionCapable</key><true/>
</dict></plist>
PL
    ;;
  MINGW*|MSYS*)
    cp $exe "$bin/ultimate_sh.exe"
    for d in SDL2.dll zlib1.dll libwinpthread-1.dll libgcc_s_seh-1.dll; do [[ -f /mingw64/bin/$d ]] && cp -f /mingw64/bin/$d "$bin/"; done
    printf '@echo off\r\ncd /d "%%~dp0"\r\nstart "" ultimate_sh.exe --roms "%s" %%*\r\n' "$(cygpath -w $roms)" > "$bin/Ultimate SH.cmd"
    ;;
  *)
    cp $exe "$bin/ultimate_sh"
    printf '#!/bin/sh\ncd "$(dirname "$0")" && exec ./ultimate_sh --roms "%s" "$@"\n' "$roms" > "$bin/ultimate-sh"
    chmod +x "$bin/ultimate-sh"
    ;;
esac
print "OUTPUT: $app"
