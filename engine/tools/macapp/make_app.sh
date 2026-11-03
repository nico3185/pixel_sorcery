#!/bin/zsh
# make_app.sh [OUTDIR] — package the build app: "PixelSorcery.app" (default in
# launchers/build/), a native window over build_ui.py --engine for this checkout. Then:
# open "launchers/build/PixelSorcery.app" (build_all.sh with no arguments does it).
set -eu
here=${0:A:h}; root=${here:h:h:h}; out=${1:-$root/launchers/build}
app="$out/PixelSorcery.app"; rm -rf "$out/PixelSorcery Build.app"   # its old name
rm -rf "$app"; mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
swiftc -swift-version 5 -O -parse-as-library -target arm64-apple-macos14 \
  $here/PixelSorceryBuild.swift -o "$app/Contents/MacOS/PixelSorceryBuild"
cat > "$app/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleName</key><string>PixelSorcery</string>
  <key>CFBundleIconFile</key><string>PixelSorcery</string>
  <key>CFBundleIdentifier</key><string>org.pixelsorcery.build</string>
  <key>CFBundleExecutable</key><string>PixelSorceryBuild</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>1.0</string>
  <key>LSMinimumSystemVersion</key><string>14.0</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>PSRepo</key><string>$root</string>
</dict></plist>
EOF
$here/make_icon.sh "$app/Contents/Resources/PixelSorcery.icns"
codesign -s - --force "$app" >/dev/null 2>&1 || true
print "$app"
