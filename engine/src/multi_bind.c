/* multi_bind.c — the per-title tables of a multi-title executable (PS_MULTI, ps_multi.h): the
 * definitions a single-title build gets from the title's generated code or the weak defaults. */
#include "genrecomp/ps_multi.h"
#include <stddef.h>

static const uint16_t s_no_cycles[0x10000];
static const char *s_no_cheats[] = { 0 };
const uint16_t *ps_m68k_cycles_p = s_no_cycles;
const uint32_t *ps_m68k_hook_pc = NULL;
uint32_t (*const *ps_m68k_hook_fn)(void) = NULL;
int ps_m68k_nhooks = 0;
void (*ps_z80_game_driver)(void *) = NULL;
const unsigned char *ps_z80_game_image = NULL;
unsigned int ps_z80_game_image_len = 0;
const char **ps_cheat_names_p = s_no_cheats;
int ps_cheat_count = 0;

void ps_multi_bind(const PsMultiTitle *t) {
    ps_m68k_cycles_p = t->m68k_cycles ? t->m68k_cycles : s_no_cycles;
    ps_m68k_hook_pc = t->hook_pc; ps_m68k_hook_fn = t->hook_fn; ps_m68k_nhooks = t->hook_pc ? t->nhooks : 0;
    ps_z80_game_driver = t->z80_driver;
    ps_z80_game_image = t->z80_image; ps_z80_game_image_len = t->z80_image ? t->z80_image_len : 0;
    ps_cheat_names_p = t->cheat_names ? t->cheat_names : s_no_cheats;
    ps_cheat_count = t->cheat_names ? t->cheat_count : 0;
}
