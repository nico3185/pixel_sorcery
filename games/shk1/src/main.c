/* SHK1 -- SH1 with Knuckles playable (games/shk1: image built by tools/make_image.py).
 * Entry, stack and vectors come from the ROM; at VBlank the game's interrupt is raised
 * and the frame presented. */
#include <genrecomp/genrecomp.h>
#include <genrecomp/bus.h>
#include <genrecomp/input.h>
#include <genrecomp/platform.h>
#include <genrecomp/func_table.h>
#include <genrecomp/ps_menu.h>
#include <genrecomp/ps_wide_sh.h>   /* widescreen: camera, level map */
#include <genrecomp/ps_render.h>
#include "recomp/recomp_funcs.h"
#include <stdio.h>
#include <stdlib.h>

/* Knuckles' colours (KCOL_ROM $95FFE, k1.s): 1 the palette swap (the console image's default), 0 SH1's
 * palette with the nearest colours, 2 extended - the native default (owner: palette expansion for the
 * Mac/PC builds): his own-colour art, SH1's palette untouched, and the renderer gives his tiles (Sonic's
 * art, VRAM tile $780 up) his colours from K_PALTAB ($95800: count - 1, then per palette and its water
 * version SH1's colours in his 4 slots - KCOL_SLOTS $957F8 - and his), so nothing else sharing them changes */
#define KCOL_ROM 0x095FFE
#define K_PALTAB 0x095800
static bool kcol_extended(void) { return bus_read16(KCOL_ROM) == 2; }
/* the signpost (object $0D) and the monitors share VRAM $D000: his face there is the signpost's
 * (tiles $6A0-$6B7) while one stands, else the 1-up monitor's ($69C-$69F) */
static bool signpost(void) {
    for (uint32_t a = 0xFFD000; a < 0xFFF000; a += 0x40) if (bus_read8(a) == 0x0D) return true;
    return false;
}
static bool no_signpost(void) { return !signpost(); }
static void kcol_init(void) {
    static uint16_t pairs[64][3]; int n = 0, e = bus_read16(K_PALTAB) + 1;
    for (int i = 0; i < e && n + 4 <= 64; i++)
        for (int k = 0; k < 4; k++) {
            uint32_t at = K_PALTAB + 2 + (uint32_t)i * 16;
            pairs[n][0] = (uint16_t)(bus_read16(0x0957F8 + 2 * (uint32_t)k) / 2);   /* KCOL_SLOTS: his 4 slots */
            pairs[n][1] = bus_read16(at + 2 * k); pairs[n][2] = bus_read16(at + 8 + 2 * k); n++;
        }
    ps_render_set_sprite_colours(0x780, 0x800, (const uint16_t (*)[3])pairs, n, kcol_extended);   /* him, his lives icon */
    ps_render_add_sprite_tiles(0x6A0, 0x6B8, signpost);
    ps_render_add_sprite_tiles(0x69C, 0x6A0, no_signpost);
    extern uint16_t ps_hw_rom_patch16(uint32_t addr, uint16_t value);
    const char *e_ = getenv("PS_KCOL");                        /* tests: another mode from the start */
    uint16_t mode = (uint16_t)(e_ ? atoi(e_) : 2), old = bus_read16(KCOL_ROM);
    ps_hw_rom_patch16(KCOL_ROM, mode);                          /* SH1 checks the ROM's sum at boot: */
    ps_hw_rom_patch16(0x18E, (uint16_t)(bus_read16(0x18E) + mode - old));   /* the header's follows */
}

static void on_vblank(void) {
    if (!genrecomp_begin_frame()) { genrecomp_shutdown(); exit(0); }
    genrecomp_vblank_irq();
    genrecomp_end_frame();
}

int main(int argc, char *argv[]) {
    argc = platform_parse_args(argc, argv);
    if (!genrecomp_init("SHK1 (Recompiled)", 3)) return 1;
    if (argc < 2 || !genrecomp_load_rom(argv[1])) { fprintf(stderr, "usage: shk1 IMAGE (tools/make_image.py SONIC1 KIS2 IMAGE)\n"); return 1; }
    recomp_register_all();
    bus_set_vblank_callback(on_vblank);
    static const PsWideGame wide = PS_WIDE_SONIC1("shk1");
    ps_wide_set_game(&wide);
    ps_menu_set_pause_hook(sh_paused);
    ps_menu_set_title("shk1");
    kcol_init();                                                   /* extended colours by default */
    ps_menu_add_toggle("KNUCKLES 16 COLOURS", KCOL_ROM, 0, true);   /* the console's palette, nearest colours */
    ps_menu_add_toggle("KNUCKLES PALETTE SWAP", KCOL_ROM, 1, true); /* his colours in Sonic's slots, as on a console */
    g_m68k.a[7] = bus_read32(0);
    g_m68k.pc = bus_read32(4);
    m68k_set_sr(0x2700);
    func_table_call(g_m68k.pc);
    genrecomp_shutdown();
    return 0;
}
