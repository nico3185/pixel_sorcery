/* SHCD -- the player's disc (.cue) on PixelSorcery. The disc's programs load at run time
 * (IPX, per-stage Word RAM images): the stage programs are compiled as overlays when generated
 * (games/shcd/src/recomp), the rest is interpreted.
 * The BIOS matching the disc's region comes from the player's BIOS folder. */
#include <genrecomp/genrecomp.h>
#include <genrecomp/bus.h>
#include <genrecomp/platform.h>
#include <genrecomp/func_table.h>
#include <genrecomp/ps_menu.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The Mega CD splash and BIOS menu are not shown (PS_CD_MENU=1 shows them): the boot runs
 * unseen, Start pressed for the menu, until the disc's first program after its boot code and
 * the licence screen (MDINIT) runs. */
extern int g_ps_boot_hidden;
static void on_vblank(void) {
    if (g_ps_boot_hidden) {
        const char *ov = func_table_last_overlay();      /* 2: Start for the menu, 1: only unseen */
        if (ov && !strncmp(ov, "IP", 2)) g_ps_boot_hidden = 1;
        if ((ov && strncmp(ov, "BIOS", 4) && strncmp(ov, "IP", 2) && strcmp(ov, "MDINIT")) || platform_frame_number() > 3600) {
            g_ps_boot_hidden = 0; fprintf(stderr, "boot: %s at frame %ld\n", ov ? ov : "-", platform_frame_number());
        }
    }
    if (!genrecomp_begin_frame()) { genrecomp_shutdown(); exit(0); }
    genrecomp_vblank_irq();
    genrecomp_end_frame();
}

int main(int argc, char *argv[]) {
    argc = platform_parse_args(argc, argv);
    if (!genrecomp_init("SHCD", 3)) return 1;
    if (argc < 2 || !genrecomp_load_rom(argv[1])) { fprintf(stderr, "usage: shcd DISC.cue\n"); return 1; }
#ifdef PS_OVERLAYS
    { void ps_overlays_register(void), ps_megacd_overlays(void); ps_overlays_register(); ps_megacd_overlays(); }
#endif
    g_ps_boot_hidden = !getenv("PS_CD_MENU") && !getenv("PS_POLL_REPLAY") && !getenv("GENRECOMP_MOVIE") ? 2 : 0;
    bus_set_vblank_callback(on_vblank);
    ps_menu_set_title("shcd");
    g_m68k.a[7] = bus_read32(0);
    g_m68k.pc = bus_read32(4);
    m68k_set_sr(0x2700);
    func_table_call(g_m68k.pc);
    genrecomp_shutdown();
    return 0;
}
