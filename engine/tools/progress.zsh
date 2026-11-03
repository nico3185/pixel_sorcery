#!/bin/zsh
# progress.zsh LABEL: a one-line progress bar for a build step. Reads "[n/total] what" lines on stdin
# (ninja's own status lines, generate.sh's), counts them, and redraws "LABEL [####----] 42/120 what"
# in place; any other line (a compiler error, a warning) is printed as is above the bar. Without a
# terminal it prints nothing but those other lines and a closing "LABEL: done (N steps)".
label=${1:-building}; n=0; total=0; tty=0; [[ -t 1 ]] && tty=1
cols=${COLUMNS:-$(tput cols 2>/dev/null || print 80)}
while IFS= read -r l; do
  if [[ $l =~ '^\[([0-9]+)/([0-9]+)\] ?(.*)' ]]; then
    n=$((n + 1)); total=$match[2]; (( n > total )) && n=$total
    (( tty )) || continue
    w=$(( n * 30 / (total ? total : 1) ))
    what=${${match[3]##* }:t}; what=${what%.o}
    s="$label [${(l:$w::#:):-}${(l:$((30 - w)):: :):-}] $n/$total $what"
    printf '\r\e[K%s' "${s[1,cols-1]}"
  else
    (( tty && n )) && printf '\r\e[K'
    print -r -- "$l"
  fi
done
(( tty )) && printf '\r\e[K'
print "$label: done${total:+ ($total steps)}"
