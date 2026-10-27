#!/bin/zsh
# heavy.sh COMMAND... — run a heavy job (recomp.sh, sidebyside.py, divergence.sh, ninja) through one
# of 5 shared slots so the parallel agents don't oversubscribe the machine; waits for a free slot.
# Slots are directories (atomic mkdir); a slot whose holder died is reclaimed.
dir=${TMPDIR:-/tmp}/pixelsorcery-heavy-slots; mkdir -p $dir   # shared by every caller
while true; do
  for s in 1 2 3 4 5; do
    if mkdir $dir/$s 2>/dev/null; then
      print $$ > $dir/$s/pid
      trap "rm -rf $dir/$s" EXIT INT TERM
      nice -n 5 "$@"; rc=$?
      rm -rf $dir/$s; trap - EXIT INT TERM
      exit $rc
    fi
    p=$(cat $dir/$s/pid 2>/dev/null); [[ -n $p ]] && ! kill -0 $p 2>/dev/null && rm -rf $dir/$s
  done
  sleep 5
done
