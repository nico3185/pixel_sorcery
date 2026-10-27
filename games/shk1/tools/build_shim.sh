#!/bin/zsh
# build_shim.sh - assemble k1.s (our own 68000 code) into k1.srec + k1.sym, which are committed so
# building the image needs no 68000 toolchain. Each ".section .pXXXXXX" goes at $XXXXXX (a patch
# over SH1), ".k1" at $80000. Needs m68k-elf binutils (brew install m68k-elf-binutils).
set -eu
g=${0:A:h:h}; t=$(mktemp -d); trap "rm -rf $t" EXIT
py=$g/../../engine/.venv/bin/python
$py $g/tools/make_image.py --syms $g/k1_reloc.inc
cd $g
m68k-elf-as -m68000 --register-prefix-optional -o $t/k1.o k1.s
starts=(--section-start=.k1=0x80000)
for s in $(grep -o '^\s*\.section \.p[0-9a-f]\{6\}' k1.s | awk '{print $2}'); do starts+=(--section-start=$s=0x${s#.p}); done
m68k-elf-ld -o $t/k1.elf $starts -e 0 --no-warn-rwx-segments $t/k1.o 2>/dev/null || m68k-elf-ld -o $t/k1.elf $starts -e 0 $t/k1.o
sha=$(shasum k1.s | cut -c1-16)
m68k-elf-objcopy -O srec --srec-forceS3 $t/k1.elf $t/k1.srec
# S0 header: "k1.s <sha>" so make_image.py can tell a stale k1.srec
hdr=$(print -n "k1.s $sha" | xxd -p | tr -d '\n' | tr a-f A-F); n=$(( ${#hdr} / 2 + 3 ))
line=$(printf "%02X0000%s" $n $hdr); sum=0; for ((i=1;i<=${#line};i+=2)); do (( sum += 16#${line:$i-1:2} )); done
print "S0${line}$(printf %02X $(( 255 - (sum & 255) )))" > k1.srec
grep -v '^S0' $t/k1.srec >> k1.srec
m68k-elf-nm $t/k1.elf | grep ' [TtAa] K_' > k1.sym
print "k1.srec: $(grep -c '^S3' k1.srec) records, $(wc -l < k1.sym | tr -d ' ') symbols"
python3 ${0:A:h}/sync_seeds.py
