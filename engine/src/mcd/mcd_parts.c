/*
 * mcd_parts.c -- power-on of the native Mega CD's parts: the PCM chip (mcd_pcm.c) and the graphics
 * ASIC (mcd_gpu.c). The port's own code.
 */
#include "mcd.h"

void mcd_pcm_power(void);
void mcd_gpu_power(void);
void mcd_parts_power(void) { mcd_pcm_power(); mcd_gpu_power(); }
