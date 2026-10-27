#!/bin/zsh
# build.sh OUTDIR: oldrun against Genesis Plus GX 8744c39 (Dec 2014), the core timing
# BizHawk 1.11 movies sync on (see oldrun.c). The checkout comes from the engine's
# GPGX submodule history (git worktree in OUTDIR/gpgx) and gets one hook line in
# io_ctrl.c, the same poll hook the engine's copy has. Output: OUTDIR/oldrun
set -eu
here=${0:A:h}; gpgx=$here/../../ext/Genesis-Plus-GX; out=${1:A}; commit=8744c39
mkdir -p $out/obj $out/inc
[[ -d $out/gpgx ]] || git -C $gpgx worktree add --detach $out/gpgx $commit >/dev/null
python3 -I - $out/gpgx/core/io_ctrl.c <<'PY'
import sys
p = sys.argv[1]; s = open(p, newline='').read()
if 'io_pad_write_hook' not in s:
    nl = '\r\n' if '\r\n' in s else '\n'
    fn = 'void io_68k_write(unsigned int offset, unsigned int data)'
    w = '      io_reg[offset] = data;' + nl + '      port[offset-1].data_w(data, io_reg[offset + 3]);'
    assert fn in s and w in s
    s = s.replace(fn, 'void (*io_pad_write_hook)(int port, unsigned int data, unsigned int ctrl) = 0;' + nl + nl + fn, 1)
    s = s.replace(w, '      if (io_pad_write_hook) io_pad_write_hook(offset - 1, data, io_reg[offset + 3]);' + nl + w, 1)
    open(p, 'w', newline='').write(s)
PY
cp $out/gpgx/sdl/config.h $out/inc/config.h
core=$out/gpgx/core
inc=(-I$here -I$out/inc ${(f)"$(find $core -type d ! -path '*tremor*' | sed 's/^/-I/')"})
for f in ${(f)"$(find $core -name '*.c' ! -path '*/tremor/*')"} $here/oldrun.c; do
  clang -c -O2 -w -DLSB_FIRST -DUSE_32BPP_RENDERING -DMAXROMSIZE=33554432 $inc $f -o $out/obj/${f:t:r}.o
done
clang -o $out/oldrun $out/obj/*.o -lz
print "built $out/oldrun (Genesis Plus GX $commit)"
