#!/bin/zsh
# z80_driver.sh ROM GAMEDIR - the game's Z80 sound driver as C (GAMEDIR/src/recomp/z80_driver.c), for a
# build without the self-check (build_ui.py's Build; recomp.sh makes it the same way before its checks).
# Without it the native port has no Z80 at all and every Z80-driven game is silent (SH1's music is 68K).
#   audio/driver.json without "capture": the driver is in the ROM (build_image.py).
#   "capture": true: the 68K unpacks it, so Z80 RAM is captured as the driver starts - with the trace
#   runner when the emulator core is in the tree (as recomp.sh), else with the native port itself
#   (PS_Z80_CAPTURE: an image at each /RESET release; the first with >= 256 non-zero bytes, as the trace
#   runner's PS_Z80_DUMP, or the last with "capture_last"). The native build then has no driver yet:
#   built first, run 600 frames headless, rebuilt by the caller with the driver.
# Nothing to do without audio/driver.json, or when z80_driver.c is newer than driver.json.
set -eu; setopt pipefail
self=$0; r=$1; g=$2
[[ $OSTYPE == (msys|cygwin)* ]] && { self=$(cygpath -u $self); r=$(cygpath -u $r); g=$(cygpath -u $g); }   # MSYS2: C:/... to /c/... before :A
eng=${self:A:h}/..
rom=${r:A}; game=${g:A}
cd $game
[[ -f audio/driver.json ]] || exit 0
out=src/recomp/z80_driver.c
[[ -f $out && ! audio/driver.json -nt $out ]] && exit 0
py=$eng/.venv/bin/python; [[ -x $py ]] || py=python3
win=; [[ $OSTYPE == (msys|cygwin)* ]] && win=1
mkdir -p build
if grep -q '"capture": true' audio/driver.json; then
  rm -f build/z80.bin
  if [[ -d $eng/ext/Genesis-Plus-GX/core ]]; then             # the emulator core: recomp.sh's way
    tr=build-trace/genrecomp_build/genrecomp_trace
    [[ -x $tr || -x $tr.exe ]] || { cmake -S . -B build-trace -G Ninja -DCMAKE_BUILD_TYPE=Release -DGENRECOMP_TRACE=ON >/dev/null
                                    ninja -C build-trace ${PS_JOBS:+-j $PS_JOBS} genrecomp_trace >/dev/null; }
    last=(); grep -q '"capture_last": true' audio/driver.json && last=(PS_Z80_DUMP_LAST=1)
    env $last PS_Z80_DUMP=build/z80.bin $tr --headless --frames 600 --trace-out /dev/null $rom >/dev/null 2>&1 || true
  else                                                        # native only: the port itself, no Z80 yet
    print "z80recomp: capturing the sound driver (600 frames)"
    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
    ninja -C build ${PS_JOBS:+-j $PS_JOBS} >/dev/null
    exe=$($py -c "import json,sys;sys.stdout.write(json.load(open('sidebyside.json')).get('exe',''))" 2>/dev/null || true)
    [[ -n $exe ]] || exe=build/${game:t}; [[ -n $win ]] && exe=$exe.exe
    cap=build/z80cap; rm -rf $cap; mkdir -p $cap
    nrom=$rom; [[ -n $win ]] && nrom=$(cygpath -m $rom)            # the native exe: a C:/... path
    PS_Z80_CAPTURE=$cap/%d.bin ./$exe --headless --frames 600 $nrom >/dev/null 2>&1 || true
    pick=$($py - $cap $(grep -q '"capture_last": true' audio/driver.json && print last || print first) <<'PY'
import os, sys
d, mode = sys.argv[1], sys.argv[2]
caps = sorted((int(f[:-4]), os.path.join(d, f)) for f in os.listdir(d) if f.endswith('.bin'))
full = [p for _, p in caps if sum(1 for b in open(p, 'rb').read() if b) >= 256]
print((full[-1] if mode == 'last' else full[0]) if full else '')
PY
)
    pick=${pick%$'\r'}                                         # Windows' Python: CRLF
    [[ -n $pick ]] && cp $pick build/z80.bin
  fi
  [[ -s build/z80.bin ]] || { print -u2 "no Z80 driver captured"; exit 1; }
else
  $py $eng/tools/z80recomp/build_image.py $rom audio/driver.json build/z80.bin >/dev/null
fi
codes=(); for c in $($py -c 'import json;print(" ".join(json.load(open("audio/driver.json"))["code"]))'); do codes+=(--code $c); done
$py $eng/tools/z80recomp/z80recomp.py --image build/z80.bin $codes --name game -o $out
print 'void (*ps_z80_game_driver)(PsZ80 *) = ps_z80_game_run;   /* registers the driver */' >> $out
$py - build/z80.bin audio/driver.json >> $out <<'PY'
import json, sys
img = open(sys.argv[1], 'rb').read()
end = max(int(r.split('-')[1], 0) for r in json.load(open(sys.argv[2]))['code']) + 1
print('/* code bytes the driver was recompiled from: checked against Z80 RAM at start */')
print('const unsigned char ps_z80_game_image_data[%d] = {%s};' % (end, ','.join(str(b) for b in img[:end])))
print('const unsigned char *ps_z80_game_image = ps_z80_game_image_data;')
print('unsigned int ps_z80_game_image_len = %d;' % end)
PY
print "z80recomp: $out"
