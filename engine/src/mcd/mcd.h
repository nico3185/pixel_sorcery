/*
 * mcd.h -- the native Mega CD (BACKLOG priority 0): the sub CPU's world without the emulator core.
 * The sub CPU is the port's own 68000 interpreter (s68k_interp.c); this is its memory - BIOS,
 * PRG-RAM, Word RAM (2 Mbit and 1 Mbit modes), backup RAM - and the gate array's registers,
 * interrupts and timer. The CD controller (CDC), the drive (CDD), the PCM chip and the graphics
 * ASIC are separate parts behind the small interfaces at the end.
 *
 * Structure and register behaviour ported from ares (ares/md/mcd, commit dc4f4659d496):
 *   Copyright (c) 2004-2025 ares team, Near et al
 *   Permission to use, copy, modify, and/or distribute this software for any purpose with or
 *   without fee is hereby granted, provided that the above copyright notice and this permission
 *   notice appear in all copies.
 *   THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS
 *   SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 *   AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *   WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
 *   NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 *   PERFORMANCE OF THIS SOFTWARE.
 */
#ifndef PS_MCD_H
#define PS_MCD_H
#include <stdbool.h>
#include <stdint.h>

typedef struct { bool enable, pending; } McdIrq;

typedef struct {
    uint16_t bios[0x10000];          /* 128 KB BIOS ROM (words, big-endian values) */
    uint16_t pram[0x40000];          /* 512 KB program RAM */
    uint16_t wram[0x20000];          /* 256 KB Word RAM */
    uint8_t  bram[0x2000];           /* 8 KB backup RAM */
    uint8_t  cart[0x80000];          /* 512 KB backup RAM cartridge (main side $400000-$7FFFFF) */
    uint8_t  cart_prot;              /* its write-protect register: bit 0 = writes allowed */

    struct {
        bool run, request, halt;     /* main CPU's control of the sub CPU (reset, bus request) */
        bool wram_mode;              /* MODE: 0 = 2 Mbit, 1 = 1 Mbit */
        bool wram_switch_request, wram_switch, wram_select;
        uint8_t wram_priority;       /* 1 Mbit dot-mapped writes: 0 normal, 1 under, 2 over */
        uint8_t pram_bank, pram_protect;
        uint32_t vector_level4;
    } io;
    struct { bool red, green; } led;
    struct { uint8_t cfm, cfs; uint16_t command[8], status[8]; } comm;
    struct { McdIrq irq; uint8_t frequency; int32_t left; } timer;   /* left: sub clocks to the next expiry, 0 stopped */
    struct { McdIrq irq; } external;  /* level 2: the main CPU's request */
    struct { McdIrq reset, subcode; bool pending; } irq;
    McdIrq gpu_irq, cdd_irq, cdc_irq;
} Mcd;

extern Mcd g_mcd;

void mcd_power(bool reset);
void mcd_irq_sync(void);             /* the level the sub CPU sees, from every pending source */

/* the clock and power (mcd.c) */
void mcd_line(void);                  /* the sub CPU and the gate array to the end of this main-CPU line */
void mcd_sync_sub(unsigned rel);      /* ... up to REL master clocks into the line (a shared register access) */
void mcd_main_poll_detect(unsigned mask, unsigned cyc, unsigned pc); unsigned mcd_main_poll_wait(void);
void mcd_main_poll_sync(unsigned mask, unsigned rel);
void mcd_sub_poll_detect(unsigned mask); void mcd_sub_poll_sync(unsigned mask); void mcd_sub_wake(unsigned mask);
extern unsigned g_mcd_main_stopped, g_mcd_sub_stopped;
bool mcd_load_bios(const char *path);
void mcd_power_on(void);

/* the main CPU's side (mcd_main.c): $000000-$03FFFF, $200000-$23FFFF and $A12000-$A120FF */
uint16_t mcd_main_read(uint32_t a, uint16_t data, bool dma);
void     mcd_main_write(uint32_t a, bool upper, bool lower, uint16_t data);
uint16_t mcd_main_io_read(uint32_t a, uint16_t data);
void     mcd_main_io_write(uint32_t a, bool upper, bool lower, uint16_t data);

/* the parts (mcd_cdc.c, mcd_cdd.c, mcd_pcm.c, mcd_gpu.c) */
uint8_t  mcd_cdc_reg_read(void);     uint16_t mcd_cdc_status(void);
void     mcd_cdc_reg_write(uint8_t v); void mcd_cdc_ctrl_write(bool upper, bool lower, uint16_t v);
uint16_t mcd_cdc_host_read(void);    uint16_t mcd_cdc_dma_address(void);
void     mcd_cdc_dma_address_write(uint16_t v);
uint16_t mcd_cdc_stopwatch(void);    void mcd_cdc_stopwatch_reset(void);
uint16_t mcd_cdd_read(uint32_t address);      void mcd_cdd_write(uint32_t address, bool upper, bool lower, uint16_t v);
uint8_t  mcd_pcm_read(uint32_t offset);       void mcd_pcm_write(uint32_t offset, uint8_t v);
uint16_t mcd_gpu_read(uint32_t address);      void mcd_gpu_write(uint32_t address, bool upper, bool lower, uint16_t v);
#endif
