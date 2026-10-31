#!/bin/zsh
# build_all.sh [-j N] [--check] [TITLE...] — build every title's native port (default: all titles
# with a sidebyside.json; TITLE as play.sh takes it: sh2, shk2, coi...), as play.sh does before playing: the C regenerated where recomp.json or
# the recompiler changed (N titles at a time: the generator is single-threaded), then each title
# compiled with ninja -j N. N: cores to use, default all.
# --check: recomp.sh instead (regenerate, build, self-check against the original): mostly one core
# per title, so P titles at once (-p P, default N/2), each compiling with N/P cores.
# ROMs: ../roms/<"rom" in sidebyside.json> (or TITLE.md/.bin/.gen). Logs: games/TITLE/build_all.log
# --roms DIR: first find the games in DIR (any names, zips too) and put them in ../roms.
# No arguments in a terminal: the build app (macOS: PixelSorcery Build.app, made by
# engine/tools/macapp/make_app.sh; elsewhere the terminal screen build_ui.py); --tui: the
# terminal screen; --menu: the plain menu. Progress: a bar per title.
set -u
here=${0:A:h:h:h}; jobs=$(sysctl -n hw.ncpu 2>/dev/null || nproc); check=0; titles=(); par=; romdir=
menu=0
if (( $# == 0 )) && [[ -t 0 && -t 1 ]]; then   # the build app (macOS), else the terminal screen
  app="$here/launchers/build/PixelSorcery Build.app"
  if [[ $(uname) == Darwin ]] && { [[ -d $app && ! $here/engine/tools/macapp/PixelSorceryBuild.swift -nt $app ]] || $here/engine/tools/macapp/make_app.sh >/dev/null; }; then
    open "$app"; exit 0
  fi
  exec $here/engine/.venv/bin/python $here/engine/tools/build_ui.py
fi
[[ ${1:-} == --tui ]] && exec $here/engine/.venv/bin/python $here/engine/tools/build_ui.py
[[ ${1:-} == --menu ]] && { menu=1; shift; }
ROOT=$here; source $here/engine/tools/titles.zsh
while (( $# )); do
  case $1 in
    -j) jobs=$2; shift ;;
    -j*) jobs=${1#-j} ;;
    --check) check=1 ;;
    -p) par=$2; shift ;;
    --roms) romdir=$2; shift ;;
    -h|--help) sed -n 2,10p $0; exit 0 ;;
    *) t=$(ps_resolve $1) || { print -u2 "unknown or ambiguous title '$1'"; exit 2; }; titles+=($t) ;;
  esac
  shift
done
all=(${(f)"$(cd $here/games && for d in */; do [[ -f $d/sidebyside.json && -f $d/recomp.json ]] && print ${d%/}; done)"})
shelf() {   # a launcher shelf's titles (their PORT, else ID), those that are recompilations
  local t; for t in ${(f)"$(awk -F'|' '$1 ~ /^game / { sub(/^game /, "", $1); p = $8; gsub(/ /, "", p); gsub(/ /, "", $1); print (p == "" ? $1 : p) }' $here/launchers/$1.txt)"}; do
    (( ${all[(Ie)$t]} && ! ${reply[(Ie)$t]} )) && reply+=($t); done
}
if (( menu )); then
  print "PixelSorcery - build the native ports\n"
  codes() { local f o=(); for f in $reply; do o+=(${ps_code[$f]:-$f}); done; print -r -- $o; }
  print "  1) every title (${#all})"; reply=(); shelf sh; print "  2) the sh shelf: $(codes)"
  reply=(); shelf dc; print "  3) the dc shelf: $(codes)"; print "  4) choose titles"
  read "c?titles [1]: "
  case ${c:-1} in
    2) reply=(); shelf sh; titles=($reply) ;;
    3) reply=(); shelf dc; titles=($reply) ;;
    4) for i in {1..$#all}; do printf "  %2d) %-7s %s\n" $i ${ps_code[$all[$i]]:-$all[$i]} "$(ps_name $all[$i])"; done
       read "c?numbers (e.g. 1 3 5): "; for i in ${=c}; do [[ $i == <-> && -n ${all[$i]:-} ]] && titles+=($all[$i]); done ;;
    *) titles=($all) ;;
  esac
  read "c?cores [$jobs of $(sysctl -n hw.ncpu 2>/dev/null || nproc)]: "; [[ -n $c ]] && jobs=$c
  print "  1) build (regenerate the C where stale, compile)\n  2) build + self-check against the original (slower)"
  read "c?mode [1]: "; [[ $c == 2 ]] && check=1
  (( check )) && { read "c?titles at once [$(( jobs / 2 > 0 ? jobs / 2 : 1 ))]: "; [[ -n $c ]] && par=$c; }
  print
fi
[[ $jobs == <-> && $jobs -gt 0 ]] || { print -u2 -- "-j wants a number of cores"; exit 2; }
(( $#titles )) || titles=($all)
py=$here/engine/.venv/bin/python
if [[ -n $romdir ]]; then   # --roms DIR: find the games there by content and put them in ../roms first
  print "ROMs from $romdir:"; $py $here/engine/tools/build_ui.py --prepare $romdir || print "(some ROMs could not be prepared)"
fi
rom_of() {
  local t=$1 c def=$(python3 -c "import json;print(json.load(open('$here/games/$t/sidebyside.json')).get('rom',''))")
  [[ $t == shk1 ]] && $here/games/shk1/tools/ensure_rom.sh $here/../roms >/dev/null 2>&1
  for c in $def $t.md $t.bin $t.gen; do [[ -n $c && -f $here/../roms/$c ]] && { print $here/../roms/$c; return; }; done
}
typeset -A rom result
for t in $titles; do
  [[ -f $here/games/$t/recomp.json ]] || { print -u2 "no title '$t'"; exit 2; }
  rom[$t]=$(rom_of $t)
done
t0=$SECONDS
stages() {   # stages CODE LOG: recomp.sh's output into LOG, its milestones on screen with the time
  local line s=$SECONDS; : > $2
  printf '  %-7s %3d:%02d  %s\n' $1 0 0 "analysing the ROM, generating C..."
  while IFS= read -r line; do
    print -r -- $line >> $2
    case $line in
      Generated*|z80recomp:*|self-check*|audio:*|native\ port:*|SELF-CHECK*|*error:*|*FAILED*)
        printf '  %-7s %3d:%02d  %s\n' $1 $(( (SECONDS - s) / 60 )) $(( (SECONDS - s) % 60 )) $line
        [[ $line == z80recomp:* ]] && printf '  %-7s %3d:%02d  %s\n' $1 $(( (SECONDS - s) / 60 )) $(( (SECONDS - s) % 60 )) "compiling, then 3000 frames against the original..." ;;
    esac
  done
}
if (( check )); then
  [[ -n $par ]] || par=$(( jobs / 2 > 0 ? jobs / 2 : 1 )); (( par > $#titles )) && par=$#titles
  each=$(( jobs / par > 0 ? jobs / par : 1 ))
  print "regenerate, build, self-check: $par titles at once, $each cores each"
  pids=()
  for t in $titles; do
    [[ -n ${rom[$t]} ]] || { result[$t]="no ROM"; continue; }
    while (( $#pids >= par )); do   # a free place: the next title starts as soon as one ends
      sleep 1; pids=(${(f)"$(for p in $pids; do kill -0 $p 2>/dev/null && print $p; done)"})
    done
    { PS_JOBS=$each $here/engine/tools/recomp.sh ${rom[$t]} $here/games/$t 2>&1; print "recomp-exit $?"; } | stages ${ps_code[$t]:-$t} $here/games/$t/build_all.log &
    pids+=($!)
  done
  wait
  for t in $titles; do
    [[ -n ${result[$t]:-} ]] && continue
    result[$t]=$(grep -a -E 'SELF-CHECK' $here/games/$t/build_all.log | tail -1); [[ -n ${result[$t]} ]] || result[$t]="FAILED (games/$t/build_all.log)"
  done
else
  # 1. the C, N titles at a time: missing (first build) or stale
  gen() {
    local t=$1 g=$here/games/$1
    if [[ ! -d $g/src/recomp || -z "$(ls $g/src/recomp 2>/dev/null)" ]] ||
       $py -c "import sys; sys.argv = ['x']; sys.path.insert(0, '$here/engine/tools'); import sidebyside; sys.exit(0 if sidebyside.generated_stale('$g') else 1)"; then
      local s=$SECONDS; print "== $t: generating C"
      $py $here/engine/tools/recompiler/generate.py ${rom[$t]} -o $g/src/recomp -c $g/recomp.json > $g/build_all.log 2>&1 && print "   $t: C generated ($(( SECONDS - s )) s)" || print "   $t: generation FAILED (games/$t/build_all.log)"
    fi
  }
  running=0
  for t in $titles; do
    [[ -n ${rom[$t]} ]] || { result[$t]="no ROM"; continue; }
    gen $t &
    (( ++running >= jobs )) && { wait; running=0; }
  done
  wait
  # 2. compile each with all N cores: a bar from ninja's [done/total], errors as they come
  bar() {   # bar TITLE LOG: ninja's output in, one updating line out
    local line n m w=30 f e=""
    while IFS= read -r line; do
      print -r -- $line >> $2
      if [[ $line =~ '^\[([0-9]+)/([0-9]+)\]' ]]; then
        n=$match[1]; m=$match[2]; f=$(( n * w / m ))
        [[ -t 1 ]] && printf '\r  %-24s [%s%s] %3d%%  %d/%d ' $1 ${(l:f::#:)e} ${(l:w-f::.:)e} $(( n * 100 / m )) $n $m
      elif [[ $line == *error:* || $line == FAILED:* ]]; then printf '\n  %s\n' $line
      fi
    done
    [[ -t 1 ]] && printf '\r%80s\r' ''
  }
  for t in $titles; do
    [[ -n ${result[$t]:-} ]] && continue
    g=$here/games/$t; s=$SECONDS
    print "== $t: compiling (-j $jobs)"
    if cmake -S $g -B $g/build -G Ninja -DCMAKE_BUILD_TYPE=Release >> $g/build_all.log 2>&1 &&
       { ninja -C $g/build -j $jobs 2>&1; print "ninja-exit $?"; } | bar $t $g/build_all.log && [[ $(tail -1 $g/build_all.log) == 'ninja-exit 0' ]]
    then result[$t]="OK ($(( SECONDS - s )) s)"; print "   $t: OK ($(( SECONDS - s )) s)"
    else result[$t]="FAILED (games/$t/build_all.log)"; print "   $t: FAILED"; fi
  done
fi
print "\n| title | result |\n|---|---|"
for t in $titles; do print "| $t | ${result[$t]:-not built} |"; done
print "\n$#titles titles, $(( SECONDS - t0 )) s, $jobs cores"
[[ -z ${(M)${(v)result}:#*FAILED*} ]]
