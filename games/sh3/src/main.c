/* SH3 -- statically recompiled on PixelSorcery (games/sh3, made by new_title.sh).
 * Entry, stack and vectors come from the ROM; at VBlank the game's interrupt is raised
 * and the frame presented. */
#include <genrecomp/genrecomp.h>
#include <genrecomp/bus.h>
#include <genrecomp/input.h>
#include <genrecomp/platform.h>
#include <genrecomp/func_table.h>
#include <genrecomp/ps_menu.h>
#include <genrecomp/ps_wide_sh.h>   /* widescreen: camera, level map */
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
    if (!genrecomp_init("SH3 (Recompiled)", 3)) return 1;
    if (argc < 2 || !genrecomp_load_rom(argv[1])) { fprintf(stderr, "usage: sh3 ROM\n"); return 1; }
    recomp_register_all();
    bus_set_vblank_callback(on_vblank);
    static const PsWideGame wide = PS_WIDE_SONIC3("sh3");
    ps_wide_set_game(&wide);
    ps_menu_set_pause_hook(sh_paused);
    ps_menu_set_title("sh3");
    g_m68k.a[7] = bus_read32(0);
    g_m68k.pc = bus_read32(4);
    m68k_set_sr(0x2700);
    func_table_call(g_m68k.pc);
    genrecomp_shutdown();
    return 0;
}
