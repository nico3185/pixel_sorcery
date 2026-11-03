/*
 * PixelSorcery native VDP state.
 *
 * The recompiled game talks to the video hardware through the VDP's data,
 * control and HV-counter ports ($C00000-$C0000F). This module keeps the
 * state those ports manipulate -- VRAM, CRAM, VSRAM, registers, the address
 * and code latches, DMA -- as plain native data that the renderer
 * (ps_render) draws from. It is not a cycle-stepped chip emulator: DMA
 * completes instantly and timing reads (status, HV counter) are derived
 * from the bus's scanline clock.
 *
 * Memory is stored in logical big-endian byte order: vram[a] is the byte
 * the 68000 sees at VRAM address a, and a word is (vram[a] << 8) | vram[a+1].
 */
#ifndef PS_VDP_H
#define PS_VDP_H

#include <stdint.h>
#include <stdbool.h>

typedef struct PsVdp {
    uint8_t  vram[0x10000];
    uint16_t cram[64];      /* 9-bit packed colour: BBBGGGRRR */
    uint16_t vsram[40];     /* 11-bit vertical scroll values */
    uint8_t  reg[32];
    uint8_t  sat[0x400];    /* internal sprite-table cache (Y, size, link) */

    uint32_t addr;          /* 16-bit VRAM/CRAM/VSRAM address */
    uint8_t  code;          /* CD5..CD0 */
    bool     pending;       /* first half of a two-word command written */
    bool     dma_fill;      /* fill armed, waits for the next data write */
    uint16_t last_data;     /* last word written to the data port */
    uint16_t read_buf;      /* data-port read buffer */
} PsVdp;

/* Global state (the game has exactly one VDP). */
extern PsVdp g_ps_vdp;
extern uint32_t g_ps_vdp_dma68k_words;   /* set by a 68K->VDP DMA; bus.c turns it into a CPU stall */
extern bool     g_ps_vdp_dma68k_vram;
extern uint32_t g_ps_vdp_slow_len;    /* VRAM fill/copy: bus.c keeps the busy flag for its duration */
extern int      g_ps_vdp_slow_type;

/* Bus clock the status/HV reads are derived from; installed by bus.c. */
typedef void (*ps_vdp_clock_fn)(int *line, int *cycle_in_line, int *lines_per_frame);
/* Side-effect-free 68000 bus read used by 68K->VDP DMA and status prefetch bits. */
typedef uint16_t (*ps_vdp_peek16_fn)(uint32_t addr);
/* Mega CD: true when a 68000-bus DMA with this source register 23 reads Word RAM, whose data
 * reaches the VDP one word late (the first slot gets the word left on the bus, the last source
 * word is dropped; GPGX vdp_ctrl.c) */
typedef int (*ps_vdp_dma_delayed_fn)(unsigned reg23);
void ps_vdp_set_dma_delayed(ps_vdp_dma_delayed_fn fn);

void     ps_vdp_init(ps_vdp_clock_fn clock, ps_vdp_peek16_fn peek16);
void     ps_vdp_reset(void);

/* Port access ($C00000-$C0000F). `offset` is (address & 0x1F). */
void     ps_vdp_write16(uint32_t offset, uint16_t data);
void     ps_vdp_write8(uint32_t offset, uint8_t data);
uint16_t ps_vdp_read16(uint32_t offset, uint32_t pc);
uint8_t  ps_vdp_read8(uint32_t offset, uint32_t pc);

/* Active display size from the mode registers (H40/H32, V28/V30). */
int      ps_vdp_width(void);
int      ps_vdp_height(void);
bool     ps_vdp_display_enabled(void);
bool     ps_vdp_vint_enabled(void);
bool     ps_vdp_hint_enabled(void);

/* Observer for nametable/VRAM writes (used by the 16:9 look-ahead cache).
 * Called after every VRAM byte range [addr, addr+len) changes. */
typedef void (*ps_vdp_vram_observer_fn)(uint32_t addr, uint32_t len);
void     ps_vdp_set_vram_observer(ps_vdp_vram_observer_fn fn);

#endif /* PS_VDP_H */
