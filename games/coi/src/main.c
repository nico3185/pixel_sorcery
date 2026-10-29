/*
 * COI -- statically recompiled.
 *
 * The game is compiled C (EA/Game Refuge, 1993). Its main loop runs inside
 * the reset vector's code and never returns; it waits for frames by polling
 * the VBlank counter its level-6 handler increments. So the frame is driven
 * from genrecomp's scanline clock: at VBlank it calls coi_vblank(),
 * which raises the game's VBlank interrupt and presents the frame.
 * Everything else is title-agnostic: entry, stack and VBlank come from the
 * ROM's vector table. How this title was brought up: docs/debugging.md.
 */

#include <genrecomp/genrecomp.h>
#include <genrecomp/bus.h>
#include <genrecomp/input.h>
#include <genrecomp/platform.h>
#include <genrecomp/func_table.h>
#include "recomp/recomp_funcs.h"
#include <stdio.h>
#include <stdlib.h>

#include <genrecomp/ps_vdp.h>
#include <genrecomp/ps_wide.h>
#include <genrecomp/ps_menu.h>

/* Widescreen hooks (RAM found by correlating the recorded play session with the scroll
 * registers): game state $FFF300 = 14 in gameplay (16 paused), level $FFFF44 and room
 * $FFFF46 select the map, camera X/Y in world pixels at $FFFA84/$FFFA80. */
static bool coi_paused(void) { return bus_ram_read16(0xF300) == 14 && bus_ram_read8(0xF41D) != 0; }   /* in play, PAUSE shown */
static bool coi_wide_active(void) { uint16_t s = bus_ram_read16(0xF300); return s == 14 || s == 16; }
static uint32_t coi_wide_map(void) { return (uint32_t)bus_ram_read16(0xFF44) << 8 | bus_ram_read16(0xFF46); }
static void coi_wide_camera(int *x, int *y) { *x = (int16_t)bus_ram_read16(0xFA84); *y = (int16_t)bus_ram_read16(0xFA80); }
/* Boss arenas: camera X limits $FAA2..$FAA6 locked within 192 px (tree 160..192, clown and
 * Mizrabel 192..192, frog and giant 0..192: rooms of 320..512 px; candy 3456..3520 is left
 * as is). */
/* Level map (found by matching plane A against RAM in every room): a byte per 16x16 block,
 * rows of $FFFA8E blocks, $FFFA8C rows, at the address in $FFFA88; block b's four cells
 * (TL TR BL BR) at $FFFA90 + 8b. Past the room's left/right ends the map is mirrored, so the
 * canopy and ground continue instead of plane B's sky showing through. */
static bool coi_world_cell(int cx, int cy, uint16_t *e) {
    uint32_t map = bus_ram_read32(0xFA88), blocks = bus_ram_read32(0xFA90);
    int rows = bus_ram_read16(0xFA8C), cols = bus_ram_read16(0xFA8E), w = cols * 2;
    if ((map >> 16) != 0xFF || (blocks >> 16) != 0xFF || cols <= 0 || rows <= 0 || cols * rows > 0xC000) return false;
    uint16_t flip = 0;
    if (cx < 0) { cx = -1 - cx; flip = 0x0800; }
    else if (cx >= w) { cx = 2 * w - 1 - cx; flip = 0x0800; }
    if (cx < 0 || cx >= w || cy < 0 || cy >= rows * 2) return false;
    uint8_t b = bus_ram_read8((uint16_t)(map + (uint32_t)(cy >> 1) * cols + (cx >> 1)));
    *e = bus_ram_read16((uint16_t)(blocks + b * 8u + ((cy & 1) * 2 + (cx & 1)) * 2)) ^ flip;
    return true;
}

/* The grey forest trunk of room 1 (tiles $161-$18D, palette 2): its roots and one 32-px
 * period of the trunk, captured there; the tree boss's walls (room 5 has no such tiles). */
static const PsWideArt coi_arts[] = { { 0 << 8 | 1, 160, 448, 64, 48, 32 } };
static bool coi_wide_arena(PsWideArena *a) {
    int lo = (int16_t)bus_ram_read16(0xFAA2), hi = (int16_t)bus_ram_read16(0xFAA6);
    if (hi - lo > 192 || hi < lo) return false;
    uint32_t map = coi_wide_map();
    if (map == (3 << 8 | 9)) return false;                   /* candy boss: open level, no walls */
    a->x0 = lo; a->x1 = hi + 320;
    if (map == (2 << 8 | 3)) {            /* frog: the stone pillar (sprites, tiles $3ED-$40C) */
        a->wall_src[0] = 280; a->wall_w = 48; a->wall_tiles[0] = 0x3ED; a->wall_tiles[1] = 0x40C;
    }
    if (map == (4 << 8 | 3)) {            /* giant: the room's teal brick courses (4x4 cells) */
        a->brick[0] = 16; a->brick[1] = 432; a->brick[2] = 32; a->brick[3] = 32;
    }
    if (map == (5 << 8 | 0)) a->mirror = true;   /* Mizrabel: the coi walls continue */
    if (map == (0 << 8 | 5)) {            /* tree: a forest trunk at the left wall (the boss */
        a->art = 1; a->art_sides = 1; a->art_dy = 8;      /* blocks the right); the canopy */
        a->art_dx = 16;                    /* the trunk frames the screen from 16:10 on */
        a->mirror = true; a->mirror_w = 48;               /* and hedge continue */
    }
    return true;
}

static int s_frame_count = 0;

static void coi_vblank(void) {
    /* Input first, so the handler reads fresh pad state */
    if (!genrecomp_begin_frame()) {
        printf("\nWindow closed, exiting.\n");
        genrecomp_shutdown();
        exit(0);
    }
    genrecomp_vblank_irq();   /* the game's level-6 handler, context saved */
    genrecomp_end_frame();

    s_frame_count++;
    if (getenv("PS_VDP_DBG") && s_frame_count % 60 == 0)
        printf("Frame %d VDP reg1=$%02X SR=$%04X\n", s_frame_count, g_ps_vdp.reg[1], m68k_get_sr());
    /* PS_STACK_DBG=1: where is the main thread, every second */
    if (getenv("PS_STACK_DBG") && s_frame_count % 60 == 0) {
        printf("Frame %d (SP=$%08X) ", s_frame_count, g_m68k.a[7]);
        func_table_dump_stack(stdout);
        fflush(stdout);
    }
}

int main(int argc, char *argv[]) {
    argc = platform_parse_args(argc, argv);

    printf("COI -- Static Recompilation\n\n");
    if (!genrecomp_init("COI (Recompiled)", 3)) {
        fprintf(stderr, "Failed to initialize genrecomp\n");
        return 1;
    }

    const char *rom_path = (argc > 1) ? argv[1] : "coi.md";
    if (!genrecomp_load_rom(rom_path)) {
        fprintf(stderr, "Failed to load ROM: %s\n", rom_path);
        return 1;
    }

    recomp_register_all();
    bus_set_vblank_callback(coi_vblank);
    static const PsWideGame wide = { "coi", coi_wide_active, coi_wide_map, coi_wide_camera, coi_wide_arena,
                                     coi_arts, 1, coi_world_cell, {0}, true };   /* wide_x: full sprite x */
    ps_wide_set_game(&wide);
    {   /* the forest trunk for the tree boss's wall, when it has not been captured yet (a
         * level select straight to the boss): 0.6 s headless, level select to room 1 */
        static char *args[] = { "--headless", "--frames", "1700", "--press", "400:START", "--press", "700:START",
                                "--press", "900:START", "--press", "1300:START", NULL, NULL };
        static char *env[] = { "GENRECOMP_POKE=F300=A:FF42=1", "PS_LOGIC=0:0:0", NULL };
        args[11] = (char *)rom_path;
        ps_wide_scout_arts(argv[0], args, env);
    }
    /* settings menu: title folder, and the official Action Replay codes */
    ps_menu_set_title("coi");
    { void ps_render_allow_water(void); ps_render_allow_water(); }   /* waterfalls: modern water */
    ps_menu_set_pause_hook(coi_paused);
    ps_menu_add_toggle("INFINITE LIVES", 0xFFF325, 0x03, false);
    ps_menu_add_toggle("INFINITE POWER", 0xFFF323, 0x03, false);
    ps_menu_add_toggle("INFINITE ITEMS", 0xFFF321, 0x05, false);

    /* Reset: stack and entry from the vector table, interrupts masked */
    g_m68k.a[7] = bus_read32(0);
    g_m68k.pc = bus_read32(4);
    m68k_set_sr(0x2700);

    /* Through the dispatcher, so the entry code's tail jumps are taken */
    func_table_call(g_m68k.pc);

    printf("The game's entry point returned unexpectedly\n");
    genrecomp_shutdown();
    return 0;
}
