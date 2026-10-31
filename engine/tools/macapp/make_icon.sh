#!/bin/zsh
# make_icon.sh OUT.icns - the app and disk image icon, from the PixelSorcery logo (img/sorpix.png).
set -eu
here=${0:A:h}; root=${here:h:h:h}; out=$1
set=$(mktemp -d)/PixelSorcery.iconset; mkdir -p $set
for s in 16 32 128 256 512; do
  sips -z $s $s $root/img/sorpix.png --out $set/icon_${s}x${s}.png >/dev/null
  sips -z $((s * 2)) $((s * 2)) $root/img/sorpix.png --out $set/icon_${s}x${s}@2x.png >/dev/null
done
iconutil -c icns $set -o $out
rm -r ${set:h}
