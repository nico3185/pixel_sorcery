/* ps_synth.h — PixelSorcery's own YM2612 + SN76489 (see ps_synth.c). Times are master clocks
 * relative to the current frame; ps_synth_end_frame rebases. */
#ifndef PS_SYNTH_H
#define PS_SYNTH_H
#include <stdint.h>
void ps_synth_init(int out_rate);
void ps_synth_reset(unsigned int cycles);
void ps_synth_fm_write(unsigned int cycles, unsigned int a, unsigned int v);
unsigned int ps_synth_fm_read(unsigned int cycles, unsigned int a);
void ps_synth_psg_write(unsigned int cycles, unsigned int v);
int ps_synth_end_frame(unsigned int cycles, int16_t *out, int max_frames);   /* stereo frames */
#endif
