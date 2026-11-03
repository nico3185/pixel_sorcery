/*
 * mcd_parts.c -- placeholders for the native Mega CD's PCM chip (RF5C164) and graphics ASIC until
 * steps D and E: the PCM's 64 KB wave RAM and registers are kept (the BIOS and games write and read
 * them back) but no sound is made; the ASIC's registers are kept, nothing is drawn and its
 * interrupt never fires. The graphics ASIC is mcd_gpu.c. The port's own code.
 */
#include "mcd.h"
#include <string.h>

static struct { uint8_t ram[0x10000], reg[0x20]; uint8_t bank; bool on; } s_pcm;

uint8_t mcd_pcm_read(uint32_t o) {
    o &= 0x1FFF;
    if (o >= 0x1000) return s_pcm.ram[(uint32_t)s_pcm.bank << 12 | (o & 0xFFF)];
    if (o >= 0x10 && o < 0x20) return 0;         /* channel positions: silent, at 0 */
    return s_pcm.reg[o & 0x1F];
}
void mcd_pcm_write(uint32_t o, uint8_t v) {
    o &= 0x1FFF;
    if (o >= 0x1000) { s_pcm.ram[(uint32_t)s_pcm.bank << 12 | (o & 0xFFF)] = v; return; }
    s_pcm.reg[o & 0x1F] = v;
    if (o == 7) { s_pcm.on = v >> 7 & 1; if (!(v & 0x40)) s_pcm.bank = v & 0xF; }   /* CTRL: bank select when MOD = 0 */
}
void mcd_pcm_clock(void) {}

void mcd_gpu_power(void);
void mcd_parts_power(void) { memset(&s_pcm, 0, sizeof s_pcm); mcd_gpu_power(); }
