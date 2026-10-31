#!/bin/zsh
# ./launch.sh [sh|dc|all] [--rebuild] — open launcher shelves (default: sh and dc, one
# app each; all = every game on one shelf). One version of each game per shelf.
# Builds an app first when it is missing, when its launchers/<shelf>.txt is newer, or with
# --rebuild (engine/tools/make_launcher.sh: every title's native port + SDL in one .app).
# ROMs: $ROMS, else roms/ (then ../roms) (passed as --roms; the app also keeps the ones you pick).
set -e
here=${0:A:h}
shelves=(); rebuild=0
for a in "$@"; do
  case $a in
    --rebuild) rebuild=1 ;;
    -h|--help) sed -n 2,6p $0; exit 0 ;;
    *) shelves+=($a) ;;
  esac
done
(( $#shelves )) || shelves=(sh dc)
roms=${ROMS:-$here/roms}; [[ -d $roms ]] || roms=${here:h}/roms
for shelf in $shelves; do
  cfg=$here/launchers/$shelf.txt
  [[ -f $cfg ]] || { print -u2 "no shelf '$shelf' (launchers: ${${(f)"$(cd $here/launchers && ls *.txt)"}//.txt/})"; exit 2; }
  name=$(awk '$1 == "name" { $1 = ""; sub(/^ /, ""); print; exit }' $cfg)
  app="$here/launchers/build/$name.app"
  if (( rebuild )) || [[ ! -d $app || $cfg -nt $app ]]; then
    print "building $name.app (a few minutes the first time)"
    $here/engine/tools/make_launcher.sh $cfg
  fi
  # SHK1 has no cartridge: its ROM is built from the player's SH1 and SHK2 dumps
  grep -q '^game shk1' $cfg && $here/games/shk1/tools/ensure_rom.sh $roms
  open -n "$app" --args --roms "$roms"
  print "opened $name ($roms)"
done
