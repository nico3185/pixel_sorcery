/*
 * mcd.c -- the native Mega CD's clock: the sub CPU (12.5 MHz) run beside the main CPU line by line,
 * the gate array's 384-clock tick (CDC, CDD, timer, PCM) and the CDC's DMA every 6 clocks, and
 * the BIOS loaded from the player's own dump. Stepping after ares (ares/md/mcd/mcd.cpp at commit
 * dc4f4659d496; the ISC notice is in mcd.h); the main-CPU side and the line pacing are the port's.
 */
#include "mcd.h"
#include "genrecomp/s68k.h"
#include <stdio.h>
#include <stdlib.h>
#include "genrecomp/m68k.h"
#include <string.h>

/* the parts' clocks (mcd_cdc.c, mcd_cdd.c, mcd_pcm.c) */
void mcd_cdc_clock(void);
void mcd_cdd_clock(void);
void mcd_pcm_clock(void);
void mcd_cdc_dma(void);
void mcd_cdd_sample(void);
void mcd_cdc_power(void);
void mcd_cdd_power(void);
void mcd_parts_power(void);

static struct { unsigned divider, dma; unsigned long pcm; } s_counter;

/* the gate array's view of time passing: CLOCKS sub-CPU clocks */
void mcd_cdd_clocks(unsigned clocks), mcd_gpu_step(unsigned clocks);
static void mcd_step(unsigned clocks) {
    mcd_cdd_clocks(clocks);
    mcd_gpu_step(clocks);                                 /* the graphics ASIC */                               /* the drive's own 75 Hz clock */
    s_counter.divider += clocks;
    while (s_counter.divider >= 384) {
        s_counter.divider -= 384;
        mcd_cdc_clock(); mcd_cdd_clock(); mcd_timer_clock(); mcd_pcm_clock();
    }
    s_counter.dma += clocks;
    while (s_counter.dma >= 6) { s_counter.dma -= 6; mcd_cdc_dma(); }
    s_counter.pcm += clocks * 44100ul;                   /* CD-DA position at 44.1 kHz */
    while (s_counter.pcm >= 12500000ul) { s_counter.pcm -= 12500000ul; mcd_cdd_sample(); }
}

/* run the sub CPU for CLOCKS of its clocks (a main-CPU line is 488 x 7 master clocks; the sub
 * CPU's 12.5 MHz against the main CPU's 7.67 MHz gives about 795 sub clocks per line), in slices
 * that end at the next gate-array tick so its interrupts land between the right instructions */
/* PS_MCD_TRACE=1: once a second (60 frames of 262 lines), the main and sub CPUs' state on stderr */
uint8_t mcd_cdd_status(void);
unsigned long long g_mcd_clocks_total, g_mcd_runs;
static void trace(void) {
    static int on = -1; static long lines;
    if (on < 0) on = getenv("PS_MCD_TRACE") != NULL;
    if (!on || ++lines % (262 * 60)) return;
    extern M68kContext g_m68k, g_s68k; long s68k_interp_instructions(void);
    extern uint32_t g_m68k_pf;
    fprintf(stderr, "mcd: s%ld main pf %06X mask %d | sub %s pc %06X sr %d insns %ld | cdd %02X (hc %d) | cfm %02X cfs %02X\n",
            lines / (262 * 60), g_m68k_pf, g_m68k.int_mask, g_mcd.io.halt ? "held" : "run ", g_s68k.pc, g_s68k.int_mask,
            s68k_interp_instructions(), mcd_cdd_status() & 0xF, mcd_cdd_status() >> 4, g_mcd.comm.cfm, g_mcd.comm.cfs);
    if (lines / (262 * 60) == 3) {                        /* the code the sub CPU sits in */
        fprintf(stderr, "mcd: sub code at %06X:", g_s68k.pc & ~0xFu);
        for (uint32_t x = (g_s68k.pc & ~0xFu) - 16; x < (g_s68k.pc & ~0xFu) + 32; x += 2) fprintf(stderr, " %04X", s68k_read16(x));
        extern unsigned long g_mcd_irq_taken[8];
        fprintf(stderr, "\nmcd: irqs taken L1 %lu L2 %lu L3 %lu L4 %lu L5 %lu L6 %lu",
                g_mcd_irq_taken[1], g_mcd_irq_taken[2], g_mcd_irq_taken[3], g_mcd_irq_taken[4], g_mcd_irq_taken[5], g_mcd_irq_taken[6]);
        extern unsigned long g_mcd_cdd_commands; void mcd_cdd_debug(char *, int); char cd[64]; mcd_cdd_debug(cd, sizeof cd);
        extern unsigned long g_mcd_cdd_ticks;
        fprintf(stderr, "\nmcd: cdd ticks %lu commands %lu, %s, sub clocks %u", g_mcd_cdd_ticks, g_mcd_cdd_commands, cd, g_scyc / 7);
        { extern long ps_frames; fprintf(stderr, "\nmcd: frame %ld, runs %llu (%.2f per frame), sub clocks %llu (%.0f per frame)",
              ps_frames, g_mcd_runs, (double)g_mcd_runs / ps_frames, g_mcd_clocks_total, (double)g_mcd_clocks_total / ps_frames); }
        fprintf(stderr, "\nmcd: irq enables %d%d%d%d%d%d pending %d%d%d%d%d%d\n", g_mcd.gpu_irq.enable, g_mcd.external.irq.enable,
                g_mcd.timer.irq.enable, g_mcd.cdd_irq.enable, g_mcd.cdc_irq.enable, g_mcd.irq.subcode.enable, g_mcd.gpu_irq.pending,
                g_mcd.external.irq.pending, g_mcd.timer.irq.pending, g_mcd.cdd_irq.pending, g_mcd.cdc_irq.pending, g_mcd.irq.subcode.pending);
    }
}

void mcd_cdc_dma_line(void);
unsigned g_mcd_line_scyc;                                 /* g_scyc when this line's sub CPU run began */
static unsigned s_debt;                                   /* clocks the last instruction ran past the previous line */
void mcd_run(unsigned clocks) {
    g_mcd_line_scyc = g_scyc;
    if (s_debt >= clocks) { s_debt -= clocks; return; }
    clocks -= s_debt; s_debt = 0;
    g_mcd_clocks_total += clocks; g_mcd_runs++;
    trace();
    mcd_cdc_dma_line();                                   /* the CDC's DMA catches up, once a line */
    while (clocks) {
        unsigned slice = 384 - s_counter.divider % 384;
        if (slice > clocks) slice = clocks;
        if (g_mcd.io.halt) mcd_step(slice);                    /* held in reset or bus request */
        else {
            if (g_mcd.irq.reset.pending) { g_mcd.irq.reset.pending = false; s68k_reset(); }
            unsigned start = g_scyc;
            s68k_run(start + slice * 7);                       /* the interpreter counts clocks x 7 */
            unsigned ran = (g_scyc - start) / 7;
            mcd_step(ran > slice ? ran : slice);
            if (ran > slice) {                                 /* past this slice: the excess counts against what is left */
                if (ran >= clocks) { s_debt += ran - clocks; clocks = 0; } else clocks -= ran;
                continue;
            }
        }
        clocks -= slice;
    }
}

/* the player's BIOS dump (Model 1, 128 KB) into g_mcd.bios; false when missing or the wrong size */
bool mcd_load_bios(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t b[0x20000];
    size_t n = fread(b, 1, sizeof b, f);
    fclose(f);
    if (n != sizeof b) return false;
    for (size_t i = 0; i < 0x10000; i++) g_mcd.bios[i] = (uint16_t)(b[2 * i] << 8 | b[2 * i + 1]);
    return true;
}

/* power on: memory cleared (the BIOS kept), registers to their reset state, the sub CPU held in
 * reset until the main CPU releases it through $A12000 */
/* backup RAM with Sega's standard directory header when new (the BIOS and games otherwise stop at
 * "RAM is not initialized"); as ps_megacd.c formats it for the reference player */
static void bram_format(uint8_t *ram, uint32_t size) {
    static const char tail[0x20] = "SEGA_CD_ROM\0\1\0\0\0RAM_CARTRIDGE___";
    if (!memcmp(ram + size - 0x20, tail, 0x20)) return;
    memset(ram, 0, size);
    uint8_t *h = ram + size - 0x40;
    memset(h, '_', 11); h[0x0F] = 0x40;
    unsigned blocks = size / 64 - 3;
    for (int i = 0; i < 4; i++) { h[0x10 + 2 * i] = (uint8_t)(blocks >> 8); h[0x11 + 2 * i] = (uint8_t)blocks; }
    memcpy(h + 0x20, tail, 0x20);
}

void mcd_power_on(void) {
    bram_format(g_mcd.bram, sizeof g_mcd.bram);
    memset(g_mcd.pram, 0, sizeof g_mcd.pram);
    memset(g_mcd.wram, 0, sizeof g_mcd.wram);
    memset(&s_counter, 0, sizeof s_counter);
    mcd_power(false);
    mcd_cdc_power(); mcd_cdd_power(); mcd_parts_power();
    g_scyc = 0;
}
