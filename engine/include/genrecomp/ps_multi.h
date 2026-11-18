/* ps_multi.h — one executable for several titles (engine/tools/ultimate, the Ultimate SH front).
 *
 * A title's generated code defines its tables under fixed names (ps_m68k_cycles, the hooks, the Z80
 * driver, the cheats) that a single-title build resolves at link time. Built for the multi
 * executable (PS_MULTI), each title's names are renamed per title and the title's entry binds them
 * here before it runs; the engine reads them through these bindings. */
#ifndef GENRECOMP_PS_MULTI_H
#define GENRECOMP_PS_MULTI_H
#include <stdint.h>

typedef struct {
    const uint16_t *m68k_cycles;                 /* NULL: none (never interpret) */
    const uint32_t *hook_pc; uint32_t (*const *hook_fn)(void); int nhooks;
    void (*z80_driver)(void *);                  /* PsZ80 * */
    const unsigned char *z80_image; unsigned int z80_image_len;
    const char **cheat_names; int cheat_count;
} PsMultiTitle;

void ps_multi_bind(const PsMultiTitle *t);

#endif
