#!/bin/zsh
# ./play.sh TITLE [options] — play the latest build of a title: native renderer + recompiled Z80 sound driver.
# TITLE: its code (sh2, shk2, coi, ...), its games/ folder or a unique prefix; ./play.sh alone lists them.
# Builds first if needed. ROM: $ROM, else roms/ (then ../roms/): the "rom" in
# games/TITLE/sidebyside.json (SHCD: the disc's .cue under roms/cd/), else TITLE.{md,bin,gen}.
# Extra args go to the game (e.g. --fullscreen). The native port has no emulator: compare against
# the original with engine/tools/sidebyside.py (it runs the GPGX-based *_oracle / tracer builds).
# Play a stored release instead (engine/tools/release.sh; no build, so it works while the code
# changes): ./play.sh TITLE --release [TAG] (default the latest); ./play.sh TITLE --releases lists them.
# Resume a recorded session: ./play.sh TITLE --resume [SESSION.polls] MINUTE
#   replays that session up to MINUTE at full speed, then you play on (latest session if
#   only a minute is given). The new session file contains the replayed part too.
set -e; setopt pipefail
here=${0:A:h}
ROOT=$here; source $here/engine/tools/titles.zsh
if [[ $# -eq 0 ]]; then
  print "usage: ./play.sh TITLE [options]   (./play.sh TITLE --help)"; print "titles (the code, a name or a unique prefix):"
  ps_list; exit 0
fi
title=$(ps_resolve $1) || { print -u2 "unknown or ambiguous title '$1'; ./play.sh lists them"; exit 2; }
shift
cfg=$here/games/$title/sidebyside.json
exe=$(python3 -c "import json;print(json.load(open('$cfg')).get('exe','build/$title'))")
romdef=$(python3 -c "import json;print(json.load(open('$cfg')).get('rom',''))")
if [[ ${1:-} == (-h|--help) ]]; then cat <<'HELP'
Usage: ./play.sh TITLE [--resume [SESSION.polls] MINUTE] [game options]

  (no option)          play; builds first if needed. Every run is recorded in
                       games/$title/sessions/<date>.{polls,log,meta}
  --resume S.polls M   replay session S up to minute M at full speed, then you play on
                       (S.meta settings - widescreen enemies, cheats, RAM codes - are
                       applied exactly as recorded; your own settings take over after)
  --resume M           same, latest session
  --list               list recorded sessions (script and app) with duration, logic
                       settings and errors
  --fullscreen         start fullscreen
  -h, --help           this help

Controls: arrows, Z/X/C = A/B/C, Return = Start, Esc quits; gamepads by position.
Settings menu: F1 / Tab / gamepad Minus or Home / C while paused.
Environment: ROM=path (default roms/<"rom" in sidebyside.json>), PS_WIDE=4:3|16:9|21:9|32:9.
HELP
exit 0; fi
if [[ ${1:-} == --list ]]; then
  for d in $here/games/$title/sessions "$HOME/Library/Application Support/PixelSorcery/$title/sessions"; do
    [[ -d $d ]] || continue
    print "== $d"
    for f in $(cd $d && ls -t *.polls 2>/dev/null); do
      b=${f%.polls}; n=$(wc -l < "$d/$f"); s=$(( n * 1000 / 59922 ))
      logic=all-off; [[ -f $d/$b.meta ]] && logic=$(awk '/^logic/{o=$3;c=$4;t=$5} END{print (o||c||t) ? "enemies="o" cheats="c" codes="t : "all-off"}' "$d/$b.meta") || logic="all-off?(no .meta)"
      err=$(grep -c "func_table: no function\|bad return" "$d/$b.log" 2>/dev/null || true)
      printf "  %s  %3d:%02d  %-32s %s\n" $b $((s / 60)) $((s % 60)) "$logic" "$([[ ${err:-0} -gt 0 ]] && print "$err error(s)")"
    done
  done
  exit 0
fi
releases=${PS_RELEASES:-$HOME/Library/Application Support/PixelSorcery/releases}
if [[ ${1:-} == --releases ]]; then
  for d in "$releases"/r*(N/) "$releases"/*(N/); do
    [[ -e $d/$title ]] || continue; print "  ${d:t}  $(sed -n 's/^date //p' $d/manifest.txt)"
  done | sort -u; exit 0
fi
release=
if [[ ${1:-} == --release ]]; then
  rel=latest; [[ -n ${2:-} && -d $releases/${2:-} ]] && { rel=$2; shift; }; shift
  release=$(print -l "$releases"/$rel/$title/*(N.) | head -1)
  [[ -n $release ]] || { print -u2 "no release of $title in $releases/$rel (engine/tools/release.sh makes one)"; exit 1; }
  r=$releases/$rel; print "release ${r:A:t}: $release"
fi
resume=
if [[ ${1:-} == --resume ]]; then
  if [[ $# -ge 3 && $2 == *.polls ]]; then sess=${2:A}; min=$3; shift 3
  else sess=$(ls -t $here/games/$title/sessions/*.polls | head -1); min=$2; shift 2; fi
  resume="$sess:$(python3 -c "print(int(float('$min') * 60 * 59.9227))")"
  print "resuming $sess at minute $min"
fi
game=$here/games/$title
rom=${ROM:-}
romdir=$here/roms; [[ -d $romdir ]] || romdir=$here/../roms
[[ -z $rom && $title == shk1 ]] && $game/tools/ensure_rom.sh $romdir   # built from the dumps
if [[ -z $rom ]]; then
  for c in $romdef $title.md $title.bin $title.gen; do [[ -n $c && -f $romdir/$c ]] && { rom=$romdir/$c; break; }; done
fi
[[ -f $rom ]] || { print -u2 "ROM not found for $title (set ROM=path)"; exit 1; }
if [[ -n $release ]]; then exe=$release            # a stored release: nothing to build
elif [[ ! -f $game/recomp.json ]]; then             # a disc (SHCD): its own generator, then build
  [[ -x $game/tools/generate.sh ]] && $game/tools/generate.sh $rom
  cmake -S $game -B $game/build -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
  NINJA_STATUS="[%f/%t] " ninja -C $game/build | $here/engine/tools/progress.zsh "$(ps_name $title 2>/dev/null || print $title): building"
elif [[ ! -d $game/src/recomp ]] || [[ -z "$(ls $game/src/recomp 2>/dev/null)" ]]; then
  $here/engine/tools/recomp.sh $rom $game          # generate + build + self-check
else
  # the C is regenerated when recomp.json or the recompiler changed since it was generated
  py=$here/engine/.venv/bin/python
  if $py -c "import sys; sys.argv = ['x']; sys.path.insert(0, '$here/engine/tools'); import sidebyside; sys.exit(0 if sidebyside.generated_stale('$game') else 1)"; then
    print "regenerating $title (recomp.json or the recompiler changed)"
    $py $here/engine/tools/recompiler/generate.py $rom -o $game/src/recomp -c $game/recomp.json >/dev/null
  fi
  cmake -S $game -B $game/build -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
  NINJA_STATUS="[%f/%t] " ninja -C $game/build | $here/engine/tools/progress.zsh "$(ps_name $title 2>/dev/null || print $title): building"
fi
cd $game
# Every session is replayable: inputs per controller poll + full log (misses, bad returns).
mkdir -p sessions; stamp=$(date +%Y%m%d-%H%M%S)
export PS_POLL_RECORD=${PS_POLL_RECORD:-sessions/$stamp.polls}
exec 2> >(tee sessions/$stamp.log >&2)
print "session: $game/sessions/$stamp.{polls,log}"
[[ -n $resume ]] && export PS_RESUME=$resume
exec env PS_RENDERER=${PS_RENDERER:-native} PS_Z80=${PS_Z80:-recomp} ${exe:A} "$@" $rom
