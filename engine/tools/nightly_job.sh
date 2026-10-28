#!/bin/zsh
# nightly_job.sh — what launchd runs each night: in its own worktree (../PixelSorcery-nightly,
# branch nightly) merge the local main, rebuild every title (recomp.sh self-check + tracer),
# then nightly.sh --commit (KPI numbers committed on branch nightly). Never pushes, never
# touches the main checkout. Recordings and the venv are linked from the main checkout.
# Log: ../PixelSorcery-nightly/nightly-job.log
set -u
export PATH=/opt/homebrew/bin:/usr/local/bin:$PATH
main=${0:A:h:h:h}; w=${main:h}/PixelSorcery-nightly
[[ -d $w ]] || git -C $main worktree add -b nightly $w main
cd $w || exit 1
exec >> $w/nightly-job.log 2>&1
print "== $(date '+%Y-%m-%d %H:%M') merging main ($(git -C $main rev-parse --short main))"
git checkout -q -- games 2>/dev/null                       # entry points robust.sh added last night
git merge -q --no-edit main || { git merge --abort; print "merge failed"; exit 1; }
git submodule update --init -q
[[ -e engine/.venv ]] || ln -s $main/engine/.venv engine/.venv
titles=(${(f)"$(cd games && for d in */; do [[ -f $d/sidebyside.json && -f $d/recomp.json ]] && print ${d%/}; done)"})
for t in $titles; do
  [[ -d $main/games/$t/recordings && ! -e games/$t/recordings ]] && ln -s $main/games/$t/recordings games/$t/recordings
done
for t in $titles; do
  ( rom=../roms/$(python3 -c "import json;print(json.load(open('games/$t/sidebyside.json')).get('rom',''))")
    [[ -f $rom ]] || { print "$t: no ROM"; exit 0; }
    engine/tools/heavy.sh zsh -c "engine/tools/recomp.sh $rom games/$t && cd games/$t && cmake -S . -B build-trace -G Ninja -DCMAKE_BUILD_TYPE=Release -DGENRECOMP_TRACE=ON >/dev/null && ninja -C build-trace genrecomp_trace >/dev/null" > games/$t/nightly-build.log 2>&1
    print "$t: build exit $?; $(grep -E 'SELF-CHECK' games/$t/nightly-build.log | tail -1)" ) &
done
wait
engine/tools/nightly.sh --commit
print "== done $(date '+%H:%M')"
