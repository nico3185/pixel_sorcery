/* s68k.h -- the Mega CD's sub CPU on the port's 68000 interpreter (src/s68k_interp.c) */
#ifndef GENRECOMP_S68K_H
#define GENRECOMP_S68K_H
#include <stdint.h>

/* the sub CPU's bus, provided by the Mega CD runtime */
uint8_t  s68k_read8(uint32_t addr);
uint16_t s68k_read16(uint32_t addr);
uint32_t s68k_read32(uint32_t addr);
void     s68k_write8(uint32_t addr, uint8_t val);
void     s68k_write16(uint32_t addr, uint16_t val);
void     s68k_write32(uint32_t addr, uint32_t val);

void s68k_reset(void);                  /* SSP and PC from the vectors at 0 */
void s68k_run(unsigned int end);        /* run until the cycle counter reaches end (68000 clocks x 7) */
void s68k_set_irq(int level);           /* level 1-7, autovectored */
void s68k_clear_irq(int level);
extern unsigned int g_scyc;             /* the sub CPU's cycle counter */
#endif
