#!/bin/zsh
# recomp.sh ROM GAMEDIR [--frames N]
# Generate C from GAMEDIR/recomp.json, build the native game, then self-check:
# a headless run (GAMEDIR/selfcheck.args inputs) must show 0 dispatch misses,
# 0 bad returns, and 0 frames differing between the native renderer and GPGX; with a
# recompiled Z80 driver + native YM2612/PSG, the write stream (values + cycles) must equal
# GPGX's Z80 + chips, and the native synth's output must match GPGX's spectrally (librosa).
set -eu
eng=${0:A:h}/..; rom=${1:A}; game=${2:A}; shift 2
frames=3000; [[ ${1:-} == --frames ]] && frames=$2
py=$eng/.venv/bin/python
[[ -x $py ]] || { python3 -m venv $eng/.venv; $eng/.venv/bin/pip -q install capstone; }
cd $game
$py -I $eng/tools/recompiler/jumptables.py $rom recomp.json 2>&1 | tail -1
$py -I $eng/tools/recompiler/check_seeds.py $rom recomp.json --fix | grep -v " 0 start inside" || true
$py $eng/tools/recompiler/generate.py $rom -o src/recomp -c recomp.json | grep -E "Generated|functions" | tail -1
if [[ -f audio/driver.json ]]; then   # Z80 sound driver -> C (no Z80 emulation)
  mkdir -p build
  if grep -q '"capture": true' audio/driver.json; then   # unpacked by the 68K: Z80 RAM as it starts
    tr=build-trace/genrecomp_build/genrecomp_trace
    [[ -x $tr ]] || { cmake -S . -B build-trace -G Ninja -DCMAKE_BUILD_TYPE=Release -DGENRECOMP_TRACE=ON >/dev/null; ninja -C build-trace genrecomp_trace >/dev/null; }
    last=(); grep -q '"capture_last": true' audio/driver.json && last=(PS_Z80_DUMP_LAST=1)   # patched after upload
    rm -f build/z80.bin; env $last PS_Z80_DUMP=build/z80.bin $tr --headless --frames 600 --trace-out /dev/null $rom >/dev/null 2>&1 || true
    [[ -s build/z80.bin ]] || { print -u2 "no Z80 driver captured"; exit 1; }
  else
    $py $eng/tools/z80recomp/build_image.py $rom audio/driver.json build/z80.bin >/dev/null
  fi
  codes=(); for r in $($py -c 'import json;print(" ".join(json.load(open("audio/driver.json"))["code"]))'); do codes+=(--code $r); done
  $py $eng/tools/z80recomp/z80recomp.py --image build/z80.bin $codes --name game -o src/recomp/z80_driver.c
  print 'void (*ps_z80_game_driver)(PsZ80 *) = ps_z80_game_run;   /* registers the driver */' >> src/recomp/z80_driver.c
  $py - build/z80.bin audio/driver.json >> src/recomp/z80_driver.c <<'PY'
import json, sys
img = open(sys.argv[1], 'rb').read()
end = max(int(r.split('-')[1], 0) for r in json.load(open(sys.argv[2]))['code']) + 1
print('/* code bytes the driver was recompiled from: checked against Z80 RAM at start */')
print('const unsigned char ps_z80_game_image_data[%d] = {%s};' % (end, ','.join(str(b) for b in img[:end])))
print('const unsigned char *ps_z80_game_image = ps_z80_game_image_data;')
print('unsigned int ps_z80_game_image_len = %d;' % end)
PY
fi
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null && if [[ -n ${PS_BUILD_PROGRESS:-} ]]; then
  ninja -C build ${PS_JOBS:+-j $PS_JOBS}             # build UI: ninja's [done/total] lines on stdout
else ninja -C build ${PS_JOBS:+-j $PS_JOBS} >/dev/null; fi   # PS_JOBS: cores (build_all.sh -j)
native=$(find build -maxdepth 1 -type f -perm -u+x ! -name '*.*' ! -name '*_oracle' | head -1)   # shipped port
exe=${native}_oracle                         # same game code on the GPGX-backed runtime (compare modes)
args=(--headless --frames $frames); [[ -f selfcheck.args ]] && args+=(${(z)"$(<selfcheck.args)"})
log=build/selfcheck.log
PS_DIFF_LOG=1 PS_RENDERER=compare $exe $args $rom >$log 2>&1 || true
misses=$(grep -c "no function at" $log || true); rts=$(grep -c "RTS to" $log || true)
diffline=$(grep "\[compare\] frame" $log | tail -1 | sed 's/.*| //')
print "self-check over $frames frames: misses=$misses bad_returns=$rts video=${diffline:-n/a}"
[[ $misses == 0 && $rts == 0 && $diffline == "0/"* ]] || { print -u2 "SELF-CHECK FAILED (see $log)"; exit 1; }
if [[ -f audio/driver.json ]]; then
  # 1. recompiled Z80 + native chips vs GPGX Z80 + GPGX chips: identical FM/PSG write streams
  PS_Z80=recomp PS_AUDIO=native PS_FMLOG=build/fm_recomp.log $exe $args $rom >build/selfcheck_z80.log 2>&1 || true
  PS_Z80=gpgx   PS_AUDIO=gpgx   PS_FMLOG=build/fm_gpgx.log   $exe $args $rom >/dev/null 2>&1 || true
  zmiss=$(grep -c "dispatch miss" build/selfcheck_z80.log || true)
  if [[ $zmiss == 0 ]] && cmp -s build/fm_recomp.log build/fm_gpgx.log; then
    print "audio: Z80 recomp + native chips write stream identical ($(wc -l < build/fm_gpgx.log | tr -d ' ') FM/PSG writes)"
  else
    print -u2 "SELF-CHECK FAILED: Z80 misses=$zmiss, FM/PSG streams differ (build/fm_*.log)"; exit 1
  fi
  # 1b. the native port (no emulator code) produces the same stream
  PS_WIDE_OBJECTS=0 PS_FMLOG=build/fm_native.log $native $args $rom >/dev/null 2>&1 || true
  cmp -s build/fm_native.log build/fm_gpgx.log && print "audio: native port write stream identical" \
    || { print -u2 "SELF-CHECK FAILED: native port FM/PSG stream differs (build/fm_native.log)"; exit 1; }
  # 2. native synth output vs GPGX output, numerically (librosa; never listened to)
  if $py -c "import librosa" 2>/dev/null; then
    PS_AUDIO=compare PS_WAV=build/audio_native.wav PS_WAV_REF=build/audio_gpgx.wav $exe $args $rom >/dev/null 2>&1 || true
    $py $eng/tools/audiocmp.py build/audio_native.wav build/audio_gpgx.wav 2>/dev/null > build/audiocmp.txt
    mel=$(awk '/^mel/{print $4}' build/audiocmp.txt); chroma=$(awk '/^chroma/{print $3}' build/audiocmp.txt)
    print "audio: native synth vs GPGX: mel diff ${mel} dB, chroma ${chroma}"
    (( mel < 2.5 && chroma > 0.98 )) || { print -u2 "SELF-CHECK FAILED: native synth drifted (build/audiocmp.txt)"; exit 1; }
  fi
fi
# 3. native port vs the original, side by side: the scripted inputs recorded per controller
# poll on the GPGX tracer, replayed on both; 68K RAM must match at every poll
tracer=build-trace/genrecomp_build/genrecomp_trace
[[ -x $tracer ]] || { cmake -S . -B build-trace -G Ninja -DCMAKE_BUILD_TYPE=Release -DGENRECOMP_TRACE=ON >/dev/null; ninja -C build-trace genrecomp_trace >/dev/null; }
PS_POLL_RECORD=build/selfcheck.polls $tracer $args --trace-out /dev/null $rom >/dev/null 2>&1 || true
if [[ -f sidebyside.json && -s build/selfcheck.polls ]]; then
  sbs=$($py $eng/tools/sidebyside.py $rom . build/selfcheck.polls --max-iter 1 2>&1 | tail -1)
  print "native port: $sbs"
  [[ $sbs == MATCH* ]] || { print -u2 "SELF-CHECK FAILED: native port diverges from the original"; exit 1; }
fi
print "SELF-CHECK OK"
