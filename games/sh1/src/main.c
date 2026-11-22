/* SH1 -- statically recompiled on PixelSorcery (games/sh1, made by new_title.sh).
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

/* ------------------------------------------------------------------ SPIN DASH (a new move; off by default)
 * A spin dash in SH1 (SHCD's look: Sonic spins as a ball while charging; revved by presses), an option
 * in the pause menu (SPIN DASH) or PS_SPINDASH=1. It wraps Sonic's
 * normal mode (Sonic_MdNormal, $12E18: jump $133EA, slope $134D4, move $12EB0, roll $13384, level bounds $13304,
 * SpeedToPos $D5DE, AnglePos $145EE, slope repel $13546). While ducking ($1C anim 8) and standing still, a jump
 * button starts it; each press revs it up ($200, at most $800), the charge decays by 1/32 a frame; letting go of
 * Down launches a roll ($800 + $80 per $100 of charge) - the rest is SH1's own roll code. Not SH2's spin dash:
 * no crouched frames, no dust, no rev sound (planned as a second style). */
static gen_func_t s_mdnormal;
static int s_sd_on, s_sd_charge;
static long s_sd_frame;
static bool spindash_enabled(void) {
    static int env = -1;
    if (env < 0) { const char *e = getenv("PS_SPINDASH"); env = e && *e == '1'; }
    return env || ps_menu_toggle("SPIN DASH");
}
static void sh_mdnormal(void) {
    extern long ps_frames;
    uint32_t a0 = g_m68k.a[0];
    if (s_sd_on && s_sd_frame != ps_frames - 1) s_sd_on = 0;          /* interrupted: hurt, a new level */
    if (spindash_enabled()) {
        uint8_t hold = bus_read8(0xFFF602), press = bus_read8(0xFFF603);
        if (!s_sd_on && bus_read8(a0 + 0x1C) == 8 && (press & 0x70) && bus_read16(a0 + 0x14) == 0) {
            s_sd_on = 1; s_sd_charge = 0;
            bus_write8(a0 + 0x1C, 2);                                    /* the roll animation */
            bus_write8(0xFFF00B, 0xBE);                                  /* the roll sound (PlaySound_Special) */
        } else if (s_sd_on && (hold & 2)) {                             /* charging */
            if (press & 0x70) { s_sd_charge += 0x200; if (s_sd_charge > 0x800) s_sd_charge = 0x800; bus_write8(0xFFF00B, 0xBE); }
            s_sd_charge -= s_sd_charge >> 5;
            bus_write8(a0 + 0x1C, 2); bus_write8(a0 + 0x1E, 0);          /* keep spinning */
        } else if (s_sd_on) {                                            /* released: off as a roll */
            int speed = 0x800 + (s_sd_charge >> 8) * 0x80;
            uint8_t st = bus_read8(a0 + 0x22);
            bus_write8(a0 + 0x22, st | 4);                               /* rolling */
            bus_write8(a0 + 0x16, 0x0E); bus_write8(a0 + 0x17, 0x07); bus_write8(a0 + 0x1C, 2);
            bus_write16(a0 + 0x0C, bus_read16(a0 + 0x0C) + 5);
            bus_write16(a0 + 0x14, (uint16_t)((st & 1) ? -speed : speed));
            bus_write8(0xFFF00B, 0xBE);
            s_sd_on = 0;
        }
        if (s_sd_on || (bus_read8(a0 + 0x22) & 4)) {                     /* charging, or just launched: */
            s_sd_frame = ps_frames;                                      /* stay in place this frame */
            func_table_call(0x013304); g_m68k.a[0] = a0;                 /* level bounds */
            func_table_call(0x0145EE); g_m68k.a[0] = a0;                 /* AnglePos */
            return;
        }
    }
    s_mdnormal();
}

int main(int argc, char *argv[]) {
    argc = platform_parse_args(argc, argv);
    if (!genrecomp_init("SH1 (Recompiled)", 3)) return 1;
    if (argc < 2 || !genrecomp_load_rom(argv[1])) { fprintf(stderr, "usage: sh1 ROM\n"); return 1; }
    recomp_register_all();
    s_mdnormal = func_table_lookup(0x012E18);                           /* SPIN DASH wraps Sonic's normal mode */
    if (s_mdnormal) func_table_register(0x012E18, sh_mdnormal);
    bus_set_vblank_callback(on_vblank);
    static const PsWideGame wide = PS_WIDE_SONIC1("sh1");
    ps_wide_set_game(&wide);
    ps_menu_set_pause_hook(sh_paused);
    ps_menu_set_title("sh1");
    ps_menu_add_feature("SPIN DASH");
    /* Labyrinth, Scrap Brain 3: see-through water - f_water, v_waterpos1, camera y, v_pal_dry */
    { void ps_render_set_water_ram(uint16_t, uint16_t, uint16_t, uint16_t); ps_render_set_water_ram(0xF64C, 0xF646, 0xF704, 0xFB00); }
    { void ps_render_allow_water(void); ps_render_allow_water(); }   /* waterfalls drawn as screen doors, in water levels */
    g_m68k.a[7] = bus_read32(0);
    g_m68k.pc = bus_read32(4);
    m68k_set_sr(0x2700);
    func_table_call(g_m68k.pc);
    genrecomp_shutdown();
    return 0;
}
