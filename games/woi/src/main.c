/*
 * WOI -- statically recompiled
 * on PixelSorcery. The frame is driven from the engine's scanline clock: at VBlank it
 * calls woi_vblank(), which raises the game's VBlank interrupt and presents the frame.
 * Entry, stack and vectors come from the ROM's vector table.
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

/* Widescreen hooks (RAM from the Mickey & Donald Collection's WOI notes, MIT,
 * github.com/Darhkfox/MickeyAndDonaldCollection docs/re_woi_wide.md; checked here against
 * plane A's scroll on the USA and European dumps): main state $FFFFD0 (low 7 bits) 2 level
 * load, 3 gameplay, 10 attract demo, 12 pause; world $FFFFDA and act $FFFFDC select the map;
 * the camera shown is plane A's previous-frame X/Y ($FF8520/$FF8548). Plane A streams 96 px
 * past each edge, so 16:9 needs no level-map reader. */
static bool woi_paused(void) { return (bus_ram_read16(0xFFD0) & 0x8000) != 0; }   /* game state bit 15: paused */
static bool woi_wide_active(void) { unsigned s = bus_ram_read16(0xFFD0) & 0x7F; return s == 2 || s == 3 || s == 10 || s == 12; }
static uint32_t woi_wide_map(void) { return (uint32_t)bus_ram_read16(0xFFDA) << 8 | (bus_ram_read16(0xFFDC) & 0xFF); }
static void woi_wide_camera(int *x, int *y) { *x = (int16_t)bus_ram_read16(0x8520); *y = (int16_t)bus_ram_read16(0x8548); }

/* Level map (MDC notes, checked against plane A): plane A's layer struct at $FF8500 holds
 * the chunk layout (+0, a byte per 256x256 chunk, 32 per row), the chunk blocks (+4, a word
 * per 16x16 block, 16x16 per chunk, low 9 bits) and the block tiles (+8, four name-table
 * words per block); the room spans X 0..max+320 ($FF852C) and Y 0..max+224 ($FF8554). */
static bool woi_world_cell(int cx, int cy, uint16_t *e) {
    uint32_t lay = bus_ram_read32(0x8500), blk = bus_ram_read32(0x8504), til = bus_ram_read32(0x8508);
    int x = cx * 8, y = cy * 8;
    int w = (int16_t)bus_ram_read16(0x852C) + 320, h = (int16_t)bus_ram_read16(0x8554) + 224;
    if (!lay || !blk || !til || x < 0 || y < 0 || x >= w || y >= h || (x >> 8) >= 32) return false;
    uint8_t chunk = bus_read8(lay + (uint32_t)(y >> 8) * 32 + (x >> 8));
    uint16_t block = bus_read16(blk + ((uint32_t)chunk * 256 + ((y >> 4) & 15) * 16 + ((x >> 4) & 15)) * 2) & 0x1FF;
    *e = bus_read16(til + ((uint32_t)block * 4 + ((y >> 3) & 1) * 2 + ((x >> 3) & 1)) * 2);
    return true;
}

static void woi_vblank(void) {
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
    printf("WOI -- Static Recompilation\n\n");
    if (!genrecomp_init("WOI (Recompiled)", 3)) return 1;
    const char *rom_path = (argc > 1) ? argv[1] : "woi.md";
    if (!genrecomp_load_rom(rom_path)) { fprintf(stderr, "Failed to load ROM: %s\n", rom_path); return 1; }
    recomp_register_all();
    bus_set_vblank_callback(woi_vblank);
    static const PsWideGame wide = { "woi", woi_wide_active, woi_wide_map, woi_wide_camera, NULL, NULL, 0, woi_world_cell, {0}, true };   /* full sprite x */
    ps_wide_set_game(&wide);
    ps_menu_set_pause_hook(woi_paused);
    ps_menu_set_title("woi");

    g_m68k.a[7] = bus_read32(0);
    g_m68k.pc = bus_read32(4);
    m68k_set_sr(0x2700);
    func_table_call(g_m68k.pc);
    printf("The game's entry point returned unexpectedly\n");
    genrecomp_shutdown();
    return 0;
}
