#!/bin/zsh
# new_title.sh NAME "Display Title": a title directory from nothing (games/NAME): CMake project,
# generic main.c (entry, stack and vectors from the ROM), recomp.json with no seeds,
# sidebyside.json. Discovery then fills recomp.json: jumptables.py, traced runs
# (decomp.sh, sidebyside.py), the explorer. No game knowledge goes in by hand.
set -eu
here=${0:A:h}; name=$1; title=${2:-$1}; id=${name//-/_}
dir=$here/../../games/$name
[[ -e $dir ]] && { print -u2 "games/$name exists"; exit 1; }
mkdir -p $dir/src
sed -e "s/qs_r1/$id/g" $here/../../games/qs-r1/CMakeLists.txt > $dir/CMakeLists.txt
cat > $dir/src/main.c <<C
/* $title -- statically recompiled on PixelSorcery (games/$name, made by new_title.sh).
 * Entry, stack and vectors come from the ROM; at VBlank the game's interrupt is raised
 * and the frame presented. */
#include <genrecomp/genrecomp.h>
#include <genrecomp/bus.h>
#include <genrecomp/input.h>
#include <genrecomp/platform.h>
#include <genrecomp/func_table.h>
#include <genrecomp/ps_menu.h>
#include "recomp/recomp_funcs.h"
#include <stdio.h>
#include <stdlib.h>

static void on_vblank(void) {
    if (!genrecomp_begin_frame()) { genrecomp_shutdown(); exit(0); }
    genrecomp_vblank_irq();
    genrecomp_end_frame();
}

int main(int argc, char *argv[]) {
    argc = platform_parse_args(argc, argv);
    if (!genrecomp_init("$title (Recompiled)", 3)) return 1;
    if (argc < 2 || !genrecomp_load_rom(argv[1])) { fprintf(stderr, "usage: $id ROM\n"); return 1; }
    recomp_register_all();
    bus_set_vblank_callback(on_vblank);
    ps_menu_set_title("$name");
    g_m68k.a[7] = bus_read32(0);
    g_m68k.pc = bus_read32(4);
    m68k_set_sr(0x2700);
    func_table_call(g_m68k.pc);
    genrecomp_shutdown();
    return 0;
}
C
printf '{\n  "title": "%s",\n  "seeds": {},\n  "hooks": {}\n}\n' "$title" > $dir/recomp.json
printf '{"exe": "build/%s", "mask": "FE00-10000"}\n' "$id" > $dir/sidebyside.json
print -- "--press 400:START --press 700:START --press 1000:START" > $dir/selfcheck.args
print "games/$name ready"
