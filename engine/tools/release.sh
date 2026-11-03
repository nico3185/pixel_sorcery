#!/bin/zsh
# release.sh [--tag NAME] [--keep N] [TITLE...] — tag this version and keep a copy of its builds, so
# the last good version stays playable (./play.sh TITLE --release [TAG]) while games/*/build changes.
#   1. each title (default: every title that has a build; TITLE as play.sh takes it) is brought up
#      to date as play.sh does (its C regenerated when stale, then ninja); a failure stops the release
#   2. HEAD gets an annotated tag (default rYYYY.MM.DD-N), pushed to origin; tracked changes that
#      are not committed stop it (the copy must be the tagged code). Not pushed to public.
#   3. the executables are copied (APFS clones: no space until a rebuild replaces them) to
#      $PS_RELEASES (default ~/Library/Application Support/PixelSorcery/releases)/TAG/TITLE/,
#      with TAG/manifest.txt; "latest" points at it. The builds hold code made from your ROMs:
#      they stay on this machine, never in git. The N newest releases are kept (default 8).
set -eu; setopt pipefail
here=${0:A:h:h:h}; ROOT=$here; source $here/engine/tools/titles.zsh
store=${PS_RELEASES:-$HOME/Library/Application Support/PixelSorcery/releases}
tag=; keep=8; titles=()
while (( $# )); do
  case $1 in
    --tag) tag=$2; shift ;;
    --keep) keep=$2; shift ;;
    -h|--help) sed -n 2,11p $0; exit 0 ;;
    *) t=$(ps_resolve $1) || { print -u2 "unknown or ambiguous title '$1'"; exit 2; }; titles+=($t) ;;
  esac
  shift
done
exe_of() { python3 -c "import json,sys; print(json.load(open(sys.argv[1])).get('exe', 'build/' + sys.argv[2]))" $here/games/$1/sidebyside.json $1; }
if (( ! $#titles )); then
  ps_titles; for t in $reply; do [[ -f $here/games/$t/$(exe_of $t) ]] && titles+=($t); done
fi
(( $#titles )) || { print -u2 "nothing built yet"; exit 1; }
cd $here
dirty=$(git status --porcelain --untracked-files=no | grep -v ' engine/ext/' | grep -v ' docs/kpi/history.csv' || true)
[[ -z $dirty ]] || { print -u2 "uncommitted changes (commit them first, the release is the tagged code):\n$dirty"; exit 1; }
py=$here/engine/.venv/bin/python
rom_of() {   # as play.sh finds it: sidebyside.json's "rom", else TITLE.{md,bin,gen}, in roms/ or ../roms
  local c d def=$(python3 -c "import json,sys; print(json.load(open(sys.argv[1])).get('rom', ''))" $here/games/$1/sidebyside.json)
  for d in $here/roms $here/../roms; do for c in $def $1.md $1.bin $1.gen; do
    [[ -n $c && -f $d/$c ]] && { print $d/$c; return; }; done; done
}
for t in $titles; do
  [[ -f games/$t/build/build.ninja ]] || { print -u2 "$t: no build (./play.sh $t builds it)"; exit 1; }
  if [[ -f games/$t/recomp.json ]]; then
    if $py -c "import sys; sys.argv = ['x']; sys.path.insert(0, '$here/engine/tools'); import sidebyside; sys.exit(0 if sidebyside.generated_stale('$here/games/$t') else 1)"; then
      r=$(rom_of $t); [[ -n $r ]] || { print -u2 "$t: no ROM to regenerate its C"; exit 1; }
      print "$t: regenerating its C"; $py engine/tools/recompiler/generate.py $r -o games/$t/src/recomp -c games/$t/recomp.json > /dev/null
    fi
  elif [[ -x games/$t/tools/generate.sh ]]; then
    r=$(rom_of $t); [[ -n $r ]] || { print -u2 "$t: no disc"; exit 1; }
    games/$t/tools/generate.sh $r
  fi
  NINJA_STATUS="[%f/%t] " ninja -C games/$t/build | engine/tools/progress.zsh "$t: up to date?"
done
if [[ -z $tag ]]; then
  d=r$(date +%Y.%m.%d); n=1; while git rev-parse -q --verify "refs/tags/$d-$n" >/dev/null; do n=$((n + 1)); done; tag=$d-$n
fi
git rev-parse -q --verify "refs/tags/$tag" >/dev/null && { print -u2 "tag $tag exists"; exit 1; }
dest=$store/$tag; mkdir -p $dest
{ print "tag $tag"; print "commit $(git rev-parse HEAD)"; print "date $(date '+%Y-%m-%d %H:%M')"; } > $dest/manifest.txt
for t in $titles; do
  e=$(exe_of $t); mkdir -p $dest/$t
  cp -c games/$t/$e $dest/$t/ 2>/dev/null || cp games/$t/$e $dest/$t/
  print "title $t ${e:t} $(shasum -a 256 games/$t/$e | cut -c1-16)" >> $dest/manifest.txt
done
git tag -a $tag -m "Release $tag: ${(j:, :)${titles}}"
git push -q origin $tag
ln -sfn $tag $store/latest
old=(${(f)"$(cd $store && /bin/ls -1d r*(N/) 2>/dev/null | sort -t- -k1,1r -k2,2nr | tail -n +$((keep + 1)))"})
for o in $old; do [[ -n $o && -d $store/$o ]] && rm -r -- "$store/$o"; done
print "release $tag: $#titles titles in $dest (./play.sh TITLE --release)"
