#!/bin/zsh
# make_shelf.sh CONFIG [ROMS] - the Linux game shelf (make_launcher.sh's .app on the Mac, windows/make_shelf.sh's
# folder on Windows): every title of launchers/CONFIG.txt that has a native build is copied, as ID, into
# launchers/build/linux/NAME/ beside ps_launcher, with a launcher.txt holding only those titles (a cartridge
# without a build is left off the shelf). SDL2 and zlib come from the system. Then the shelf opens.
# PixelSorcery's window calls it (build_ui.py --engine: shelf NAME).
set -eu
root=${0:A:h:h:h:h}
cfg=$root/launchers/${1:?usage: make_shelf.sh CONFIG [ROMS]}.txt; roms=${2:-$root/roms}; [[ -d $roms ]] || roms=$root/../roms
name=$(awk '$1 == "name" { $1 = ""; sub(/^ /, ""); print; exit }' $cfg)
out=$root/launchers/build/linux/$name; mkdir -p $out
ids=(${(f)"$(awk -F'|' '$1 ~ /^game / { sub(/^game /, "", $1); gsub(/ /, "", $1); p = $8; gsub(/ /, "", p); print $1 ":" (p == "" ? $1 : p) }' $cfg)"})
keep=(); launcher=
for entry in $ids; do
  id=${entry%%:*}; game=$root/games/${entry#*:}
  [[ -f $game/sidebyside.json ]] || continue
  exe=$game/$(python3 -c "import json,sys;sys.stdout.write(json.load(open(sys.argv[1])).get('exe',''))" $game/sidebyside.json)
  [[ -x $exe ]] || continue                                    # not built yet: not on the shelf
  cp -f $exe $out/$id; keep+=($id)
  [[ -n $launcher || ! -x $game/build/genrecomp_build/ps_launcher ]] || launcher=$game/build/genrecomp_build/ps_launcher
done
(( $#keep )) || { print -u2 "no title of $name is built yet: build one first"; exit 1; }
if [[ -z $launcher ]]; then                                    # a title's build has it, else the engine's own
  cmake -S $root/engine -B $root/engine/build -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
  ninja -C $root/engine/build ps_launcher >/dev/null; launcher=$root/engine/build/ps_launcher
fi
cp -f $launcher $out/ps_launcher
# the config with the built titles only (and their stacks reduced to them)
awk -v keep=" ${keep[*]} " -F'|' '
  $1 ~ /^game / { id = $1; sub(/^game /, "", id); gsub(/ /, "", id); if (index(keep, " " id " ")) print; next }
  $1 ~ /^stack / { n = split($0, w, " "); s = "stack"; c = 0; for (i = 2; i <= n; i++) if (index(keep, " " w[i] " ")) { s = s " " w[i]; c++ } if (c) print s; next }
  { print }' $cfg > $out/launcher.txt
print "shelf $name: ${#keep} titles in $out"
cd $out && exec ./ps_launcher launcher.txt . --roms $roms
