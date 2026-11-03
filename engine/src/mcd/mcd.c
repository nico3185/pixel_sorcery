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
enum { MCD_LINE_CLOCKS = 796 };                          /* sub clocks per main-CPU line (bus.c) */
void s68k_end_run(void); int s68k_irq_ready(void); uint32_t s68k_cur_pc(void);

/* the gate array's view of time passing: CLOCKS sub-CPU clocks */
void mcd_cdd_clocks(unsigned clocks), mcd_gpu_step(unsigned clocks);
void mcd_timer_step(unsigned clocks);
static void mcd_step(unsigned clocks) {
    mcd_timer_step(clocks);
    mcd_cdd_clocks(clocks);
    mcd_gpu_step(clocks);                                 /* the graphics ASIC */                               /* the drive's own 75 Hz clock */
    s_counter.divider += clocks;
    while (s_counter.divider >= 384) {
        s_counter.divider -= 384;
        mcd_cdc_clock(); mcd_cdd_clock(); mcd_pcm_clock();
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
    { extern unsigned long g_mcd_pcm_loud; unsigned mcd_pcm_channels_on(void);
      extern unsigned long g_mcd_pcm_writes, g_mcd_pcm_reg_writes;
      extern unsigned long g_mcd_irq_taken[8];
      fprintf(stderr, "mcd: timer freq %u enable %d pending %d, L3 taken %lu, L4 %lu, L5 %lu\n", g_mcd.timer.frequency,
              g_mcd.timer.irq.enable, g_mcd.timer.irq.pending, g_mcd_irq_taken[3], g_mcd_irq_taken[4], g_mcd_irq_taken[5]);
      fprintf(stderr, "mcd: pcm non-silent samples %lu, channels on %02X, writes %lu (registers %lu)\n", g_mcd_pcm_loud,
              mcd_pcm_channels_on(), g_mcd_pcm_writes, g_mcd_pcm_reg_writes);
      { extern unsigned long g_mcd_gpu_ops; fprintf(stderr, "mcd: graphics operations %lu\n", g_mcd_gpu_ops); } }
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
static unsigned s_pos;                                    /* sub clocks of this line already run */
static unsigned s_slice_pos, s_slice_scyc;                /* the running slice: its position in the line, g_scyc at its start */
static bool s_in_slice, s_line_open;
static void run_clocks(unsigned clocks) {
    if (!s_line_open) {                                   /* the line's first run */
        s_line_open = true; g_mcd_line_scyc = g_scyc;
        trace();
        mcd_cdc_dma_line();                               /* the CDC's DMA catches up, once a line */
    }
    unsigned pos = s_pos; s_pos += clocks;
    if (s_debt >= clocks) { s_debt -= clocks; return; }
    clocks -= s_debt; pos += s_debt; s_debt = 0;
    g_mcd_clocks_total += clocks; g_mcd_runs++;
    while (clocks) {
        unsigned slice = 384 - s_counter.divider % 384;
        if (g_mcd.timer.left > 0 && (unsigned)g_mcd.timer.left < slice) slice = (unsigned)g_mcd.timer.left;   /* the timer's expiry ends a slice */
        if (slice > clocks) slice = clocks;
        if (g_mcd_sub_stopped && s68k_irq_ready()) g_mcd_sub_stopped = 0;   /* an interrupt ends a poll idle */
        if (g_mcd.io.halt || g_mcd_sub_stopped) mcd_step(slice);   /* held in reset or bus request, or idle polling */
        else {
            if (g_mcd.irq.reset.pending) { g_mcd.irq.reset.pending = false; s68k_reset(); }
            unsigned start = g_scyc;
            s_slice_pos = pos; s_slice_scyc = start; s_in_slice = true;
            s68k_run(start + slice * 7);                       /* the interpreter counts clocks x 7 */
            s_in_slice = false;
            unsigned ran = (g_scyc - start) / 7;
            mcd_step(ran > slice ? ran : slice);
            if (ran > slice) {                                 /* past this slice: the excess counts against what is left */
                if (ran >= clocks) { s_debt += ran - clocks; clocks = 0; } else { clocks -= ran; pos += ran; }
                continue;
            }
        }
        clocks -= slice; pos += slice;
    }
}
/* the rest of this line's sub CPU and gate array, at the line's end (bus.c) */
void mcd_line(void) {
    if (s_pos < MCD_LINE_CLOCKS) run_clocks(MCD_LINE_CLOCKS - s_pos);
    s_pos = 0; s_line_open = false;
}

/* The two CPUs kept in step as the reference build's Mega CD (Genesis Plus GX) keeps them - own code
 * after its behaviour: a main-CPU access to a shared register first runs the sub CPU up to that moment
 * of the line; a CPU found reading the same register at the same instruction twice within a short
 * time (120 of its clocks for the main CPU, 98 for the sub CPU) idles until the other side writes it:
 * the main CPU to the line's end, the sub CPU through its run. REL: master clocks into the line. */
unsigned g_mcd_main_stopped, g_mcd_sub_stopped;           /* the registers (bit masks) each CPU idles on */
static struct { unsigned detected, cycle, pc; } s_mpoll, s_spoll;
static unsigned s_main_resume; static bool s_main_resumed;
static unsigned to_sub(unsigned rel) { unsigned t = rel * MCD_LINE_CLOCKS / 3420; return t > MCD_LINE_CLOCKS ? MCD_LINE_CLOCKS : t; }
unsigned mcd_slice_elapsed(void) { return s_in_slice ? (g_scyc - s_slice_scyc) / 7 : 0; }
static unsigned sub_pos(void) { return s_in_slice ? s_slice_pos + (g_scyc - s_slice_scyc) / 7 : s_pos; }
void mcd_sync_sub(unsigned rel) {
    if (g_mcd_sub_stopped) return;
    unsigned t = to_sub(rel);
    if (!s_in_slice && t > s_pos) run_clocks(t - s_pos);
}
void mcd_main_poll_detect(unsigned mask, unsigned cyc, unsigned pc) {
    if (s_mpoll.detected & mask) {
        if ((int)(cyc - s_mpoll.cycle) <= 0) {
            if (pc == s_mpoll.pc) {
                if (s_mpoll.detected & 1) g_mcd_main_stopped = mask;
                else { s_mpoll.detected |= 1; s_mpoll.cycle = cyc + 840; }
            }
            return;
        }
    } else s_mpoll.detected = mask;
    s_mpoll.cycle = cyc + 840; s_mpoll.pc = pc;
}
/* the main CPU idles: the sub CPU runs to the line's end; returns the master clock (into the line) the
 * main CPU goes on from - where the sub CPU wrote the polled register, else the line's end */
unsigned mcd_main_poll_wait(void) {
    s_main_resumed = false;
    if (s_pos < MCD_LINE_CLOCKS) run_clocks(MCD_LINE_CLOCKS - s_pos);
    g_mcd_main_stopped = 0;
    return s_main_resumed ? s_main_resume : 3420;
}
/* a main-CPU write to a register the sub CPU may poll (REL: its moment), before the write lands */
void mcd_main_poll_sync(unsigned mask, unsigned rel) {
    unsigned t = to_sub(rel);
    if (!g_mcd_sub_stopped) { if (!s_in_slice && t > s_pos) run_clocks(t - s_pos); }
    if (g_mcd_sub_stopped & mask) {
        if (!s_in_slice && t > s_pos) run_clocks(t - s_pos);   /* the gate array goes on to that moment */
        g_mcd_sub_stopped = 0;
    }
    s_spoll.detected &= ~mask; s_mpoll.detected &= ~mask;
}
void mcd_sub_poll_detect(unsigned mask) {
    unsigned cyc = g_scyc, pc = s68k_cur_pc();            /* clocks x 7: the window is 98 clocks */
    if (s_spoll.detected & mask) {
        if ((int)(cyc - s_spoll.cycle) <= 0) {
            if (pc == s_spoll.pc) {
                if (s_spoll.detected & 1) { g_mcd_sub_stopped = mask; s68k_end_run(); }
                else { s_spoll.detected |= 1; s_spoll.cycle = cyc + 98 * 7; }
            }
            return;
        }
    } else s_spoll.detected = mask;
    s_spoll.cycle = cyc + 98 * 7; s_spoll.pc = pc;
}
/* a sub-CPU write to a register the main CPU may poll */
void mcd_sub_poll_sync(unsigned mask) {
    if (g_mcd_main_stopped & mask) {
        unsigned p = sub_pos(); if (p > MCD_LINE_CLOCKS) p = MCD_LINE_CLOCKS;
        s_main_resume = p * 3420 / MCD_LINE_CLOCKS; s_main_resumed = true;
        g_mcd_main_stopped = 0;
    }
    s_spoll.detected &= ~mask; s_mpoll.detected &= ~mask;
}
/* the end of a graphics operation (mask $100) or of a CDC DMA ($10) wakes a sub CPU polling its status */
void mcd_sub_wake(unsigned mask) { if (g_mcd_sub_stopped & mask) g_mcd_sub_stopped = 0; }
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
    memset(g_mcd.cart, 0, sizeof g_mcd.cart);                /* the RAM cartridge: new, writable */
    bram_format(g_mcd.cart, sizeof g_mcd.cart);
    g_mcd.cart_prot = 1;
    memset(g_mcd.pram, 0, sizeof g_mcd.pram);
    memset(g_mcd.wram, 0, sizeof g_mcd.wram);
    memset(&s_counter, 0, sizeof s_counter);
    mcd_power(false);
    mcd_cdc_power(); mcd_cdd_power(); mcd_parts_power();
    g_scyc = 0;
}

/* ps_state.c: the whole native Mega CD - gate array and memories, the line clock, poll idling,
 * every part, the sub CPU - after the main hardware (ps_hw_state), only when a disc runs */
void ps_state_blob(void *p, unsigned long n);
void mcd_cdc_state(void), mcd_cdd_state(void), mcd_gpu_state(void), mcd_pcm_state(void), s68k_state(void);
void mcd_state(void) {
#define B(v) ps_state_blob(&(v), sizeof (v))
    extern uint32_t g_ps_mcd_mode_writes, g_ps_wram_code_writes;
    B(g_mcd); B(s_counter); B(s_debt); B(s_pos); B(s_line_open); B(g_mcd_line_scyc);
    B(g_mcd_main_stopped); B(g_mcd_sub_stopped); B(s_mpoll); B(s_spoll);
    B(g_ps_mcd_mode_writes); B(g_ps_wram_code_writes);
#undef B
    mcd_cdc_state(); mcd_cdd_state(); mcd_gpu_state(); mcd_pcm_state(); s68k_state();
}
