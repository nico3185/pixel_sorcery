#!/bin/zsh
# coverage_sweep.sh ROM [FRAMES] — drive every level/room with bot inputs and the Action
# Replay codes on (infinite lives/power/items), list functions the recompiled build lacks.
# Stage (0-27, every level and room) through the game's own level select: $FFFF42 set on
# the title screen ($FFF300=$0A); Start then loads that stage directly.
# Output: build/coverage/misses.txt (unique addresses), per-run logs.
set -u
here=${0:A:h}/..; rom=${1:A}; frames=${2:-12000}
cd $here; out=build/coverage; mkdir -p $out; : > $out/misses.txt
start="--press 400:START --press 700:START --press 900:START --press 1300:START"
bot() {  # pattern -> --press list from frame 1500 to the end
  local a=() f
  case $1 in
    run)   a+=(--press 1500:RIGHT:$frames); for ((f=1600; f<frames; f+=90)); do a+=(--press $f:C:12); done;;
    throw) a+=(--press 1500:RIGHT:$frames); for ((f=1600; f<frames; f+=60)); do a+=(--press $f:B:6 --press $((f+30)):C:20); done;;
    back)  for ((f=1500; f<frames; f+=600)); do a+=(--press $f:RIGHT:400 --press $((f+400)):LEFT:200); done
           for ((f=1550; f<frames; f+=70)); do a+=(--press $f:C:25); done;;
  esac
  print -- ${a[@]}
}
jobs_run=0
for N in {0..27}; do for P in run throw back; do
  ( GENRECOMP_POKE="F300=A:FF42=$(printf %X $N)" PS_LOGIC=0:0:7 \
    ./build/coi --headless --frames $frames ${=start} ${=$(bot $P)} $rom > $out/S${N}_$P.log 2>&1
    grep -o 'no function at \$[0-9A-F]*' $out/S${N}_$P.log | sed 's/.*\$//' | sort -u | sed "s/\$/ S${N}_$P/" >> $out/misses.txt ) &
  (( ++jobs_run % 8 == 0 )) && wait
done; done; wait
print "coverage sweep: $(cut -d' ' -f1 $out/misses.txt | sort -u | wc -l | tr -d ' ') missing functions in $(cut -d' ' -f2 $out/misses.txt | sort -u | wc -l | tr -d ' ') runs"
