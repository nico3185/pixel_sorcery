/*
 * QS, REV01 ("GM 00004054-01", the 1.25 MB World v1.1
 * cartridge) -- statically recompiled on PixelSorcery. A separate build from REV00
 * (games/qs): the code moved everywhere. This is the revision BizHawk TAS movies
 * are made on (TASVideos 3323M), which makes it the end-to-end test of the port.
 * The frame is driven from the engine's scanline clock: at VBlank it raises the game's
 * VBlank interrupt and presents the frame. Entry, stack and vectors come from the ROM.
 */
#include <genrecomp/genrecomp.h>
#include <genrecomp/bus.h>
#include <genrecomp/input.h>
#include <genrecomp/platform.h>
#include <genrecomp/func_table.h>
#include <genrecomp/ps_menu.h>
#include <genrecomp/ps_wide.h>
#include "recomp/recomp_funcs.h"
#include <stdio.h>
#include <stdlib.h>

/* Widescreen hooks (RAM from the Mickey & Donald Collection's QS notes, MIT,
 * github.com/Darhkfox/MickeyAndDonaldCollection docs/re_qs_wide.md; checked here against
 * plane A's scroll on REV00 and REV01): main state $FFFF18 (low 15 bits) 5 = gameplay with
 * VBlank handler $FFFF1A = 1; location $FF8F34 (0..9) and area $FF8F74 select the map; the
 * camera shown is the plane A layer's previous-frame X/Y (16.16, words $FF852C/$FF8528).
 * Plane A streams 96 px past each edge, so 16:9 needs no level-map reader. */
static bool qs_paused(void) { return (bus_ram_read16(0xFF18) & 0x7FFF) == 8; }   /* state 8: the item menu (START) */
static bool qs_wide_active(void) { return (bus_ram_read16(0xFF18) & 0x7FFF) == 5 && bus_ram_read16(0xFF1A) == 1; }
static uint32_t qs_wide_map(void) { return (uint32_t)bus_ram_read16(0x8F34) << 8 | (bus_ram_read16(0x8F74) & 0xFF); }
static void qs_wide_camera(int *x, int *y) { *x = (int16_t)bus_ram_read16(0x852C); *y = (int16_t)bus_ram_read16(0x8528); }

static void qs_vblank(void) {
    if (!genrecomp_begin_frame()) {
        printf("\nWindow closed, exiting.\n");
        genrecomp_shutdown();
        exit(0);
    }
    genrecomp_vblank_irq();
    genrecomp_end_frame();
}

int main(int argc, char *argv[]) {
    argc = platform_parse_args(argc, argv);
    printf("QS-R1 -- Static Recompilation\n\n");
    if (!genrecomp_init("QS-R1 (Recompiled)", 3)) return 1;
    const char *rom_path = (argc > 1) ? argv[1] : "qs-r1.md";
    if (!genrecomp_load_rom(rom_path)) { fprintf(stderr, "Failed to load ROM: %s\n", rom_path); return 1; }
    recomp_register_all();
    bus_set_vblank_callback(qs_vblank);
    static const PsWideGame wide = { "qs-r1", qs_wide_active, qs_wide_map, qs_wide_camera, NULL, NULL, 0, NULL,
                                     { 79, 78, 239, 237 }, true };   /* status bar widened between its sections; full sprite x */
    ps_wide_set_game(&wide);
    ps_menu_set_pause_hook(qs_paused);
    ps_menu_set_title("qs-r1");

    g_m68k.a[7] = bus_read32(0);
    g_m68k.pc = bus_read32(4);
    m68k_set_sr(0x2700);
    func_table_call(g_m68k.pc);
    printf("The game's entry point returned unexpectedly\n");
    genrecomp_shutdown();
    return 0;
}
