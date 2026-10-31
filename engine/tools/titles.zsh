# titles.zsh — title names shared by play.sh and build_all.sh (source it; ROOT = repo root).
# One short code per title (the codenames of the public snapshot), the game's name from the
# launcher shelves, and resolve: a folder name, a code, an old alias or a unique prefix of either.
typeset -gA ps_code=(
  sh1 sh1  sh2 sh2  shcd shcd  sh3 sh3  shk shk  sh3k sh3k  shk1 shk1  shk2 shk2
  shbs shbs  coi coi  qs qs  qs-r1 qs-r1
  woi woi  woi-us woi-us  dc dc
)
typeset -ga ps_order=(sh1 sh2 shcd sh3 shk sh3k shk2 shk1 shbs coi
  qs qs-r1 woi-us woi dc)
ps_titles() {   # the playable titles: games/ folders with a sidebyside.json, in shelf order
  local f d=(${(f)"$(cd $ROOT/games && for d in */; do [[ -f $d/sidebyside.json ]] && print ${d%/}; done)"})
  reply=(); for f in $ps_order; do (( ${d[(Ie)$f]} )) && reply+=($f); done
  for f in $d; do (( ${reply[(Ie)$f]} )) || reply+=($f); done
}
ps_name() {     # ps_name FOLDER: the game's name (launchers/all.txt; other versions from a sibling)
  local n b
  n=$(awk -F'|' -v t=$1 '$1 == "game " t { print $2; exit }' $ROOT/launchers/all.txt 2>/dev/null)
  if [[ -z $n ]]; then
    for b in ${(f)"$(awk -F'|' '$1 ~ /^game / { sub(/^game /, "", $1); print $1 }' $ROOT/launchers/all.txt 2>/dev/null)"}; do
      if [[ $1 == $b-* ]]; then                       # e.g. a later revision: the base name + (REV01)
        n="$(awk -F'|' -v t=$b '$1 == "game " t { print $2; exit }' $ROOT/launchers/all.txt) (${(U)${1#$b-}})"; break
      elif [[ $b == $1-* ]]; then                     # the shelf has the other region: Europe here
        n="$(awk -F'|' -v t=$b '$1 == "game " t { print $2; exit }' $ROOT/launchers/all.txt | sed 's/ (.*)$//') (Europe)"; break
      fi
    done
  fi
  print -r -- ${n:-$1}
}
ps_resolve() {  # ps_resolve NAME: the games/ folder; else the candidates on stderr, status 1
  local t=$1 f m=()
  ps_titles; local all=($reply)
  case $t in shk1|shk1) t=shk1 ;; sh3k) t=sh3k ;; esac      # older names
  (( ${all[(Ie)$t]} )) && { print $t; return; }
  for f in $all; do [[ ${ps_code[$f]:-} == $t ]] && { print $f; return; }; done
  for f in $all; do [[ $f == $t* || ${ps_code[$f]:-} == $t* ]] && m+=($f); done   # a unique prefix
  (( $#m == 1 )) && { print $m[1]; return; }
  print -u2 "  matching titles: ${m:-none}"; return 1
}
ps_list() {     # the titles, one a line: code, name, folder
  local f; ps_titles
  for f in $reply; do printf "  %-7s %-34s %s\n" ${ps_code[$f]:-$f} "$(ps_name $f)" "$([[ ${ps_code[$f]:-} != $f ]] && print "($f)")"; done
}
ps_tsv() {      # the titles for programs: folder TAB code TAB name, one a line
  local f; ps_titles
  for f in $reply; do print -r -- "$f	${ps_code[$f]:-$f}	$(ps_name $f)"; done
}
