/* MinGW default (ps_defaults archive member): no title hooks. See m68k_interp.c. */
#include <stdint.h>
const uint32_t *ps_m68k_hook_pc = 0;
uint32_t (*const *ps_m68k_hook_fn)(void) = 0;
int ps_m68k_nhooks = 0;
