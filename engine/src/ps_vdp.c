/*
 * PixelSorcery native VDP state: ports, registers, memories and DMA.
 * See include/genrecomp/ps_vdp.h for the model.
 */

#include "genrecomp/ps_vdp.h"

#include <string.h>

PsVdp g_ps_vdp;

static ps_vdp_clock_fn          s_clock;
static ps_vdp_peek16_fn         s_peek16;
static ps_vdp_dma_delayed_fn    s_dma_delayed;
static ps_vdp_vram_observer_fn  s_vram_observer;

/* Line timing (master clocks within a 3420-clock line), from the VDP's
 * documented H40/H32 behaviour: HBlank flag window and the line at which
 * the vertical interrupt becomes visible in the status register. */
#define MCLK_PER_LINE      3420
#define HBLANK_H40_START   228
#define HBLANK_H40_END     872
#define HBLANK_H32_START   280
#define HBLANK_H32_END     860

/* ------------------------------------------------------------------ */
/* Setup                                                              */
/* ------------------------------------------------------------------ */

void ps_vdp_init(ps_vdp_clock_fn clock, ps_vdp_peek16_fn peek16) {
    s_clock = clock;
    s_peek16 = peek16;
    ps_vdp_reset();
}

void ps_vdp_reset(void) {
    ps_vdp_vram_observer_fn keep = s_vram_observer;
    memset(&g_ps_vdp, 0, sizeof(g_ps_vdp));
    s_vram_observer = keep;
}

void ps_vdp_set_dma_delayed(ps_vdp_dma_delayed_fn fn) { s_dma_delayed = fn; }
void ps_vdp_set_vram_observer(ps_vdp_vram_observer_fn fn) { s_vram_observer = fn; }

/* ------------------------------------------------------------------ */
/* Mode queries                                                       */
/* ------------------------------------------------------------------ */

int  ps_vdp_width(void)            { return (g_ps_vdp.reg[12] & 0x01) ? 320 : 256; }
int  ps_vdp_height(void)           { return (g_ps_vdp.reg[1] & 0x08) ? 240 : 224; }
bool ps_vdp_display_enabled(void)  { return (g_ps_vdp.reg[1] & 0x40) != 0; }
bool ps_vdp_vint_enabled(void)     { return (g_ps_vdp.reg[1] & 0x20) != 0; }
bool ps_vdp_hint_enabled(void)     { return (g_ps_vdp.reg[0] & 0x10) != 0; }

/* Sprite table base and the address mask of the internal SAT cache. */
static uint32_t sat_base(void) {
    return (g_ps_vdp.reg[12] & 0x01) ? ((g_ps_vdp.reg[5] << 9) & 0xFC00)
                                     : ((g_ps_vdp.reg[5] << 9) & 0xFE00);
}
static uint32_t sat_base_mask(void) { return (g_ps_vdp.reg[12] & 0x01) ? 0xFC00 : 0xFE00; }
static uint32_t sat_addr_mask(void) { return (g_ps_vdp.reg[12] & 0x01) ? 0x03FF : 0x01FF; }

/* ------------------------------------------------------------------ */
/* Memory writes                                                      */
/* ------------------------------------------------------------------ */

static void vram_write_byte(uint32_t a, uint8_t v) {
    PsVdp *d = &g_ps_vdp;
    a &= 0xFFFF;
    if ((a & sat_base_mask()) == sat_base())
        d->sat[a & sat_addr_mask()] = v;
    if (d->vram[a] != v) {
        d->vram[a] = v;
        if (s_vram_observer) s_vram_observer(a, 1);
    }
}

/* One data-bus word to the current destination, then advance the address. */
static void bus_write(uint16_t data) {
    PsVdp *d = &g_ps_vdp;
    d->last_data = data;
    switch (d->code & 0x0F) {
    case 0x01: {                                   /* VRAM */
        uint32_t a = d->addr & 0xFFFE;
        if (d->addr & 1) data = (uint16_t)((data >> 8) | (data << 8));
        vram_write_byte(a, (uint8_t)(data >> 8));
        vram_write_byte(a + 1, (uint8_t)data);
        break;
    }
    case 0x03:                                     /* CRAM: BBB0GGG0RRR0 -> BBBGGGRRR */
        d->cram[(d->addr >> 1) & 0x3F] =
            (uint16_t)(((data & 0xE00) >> 3) | ((data & 0x0E0) >> 2) | ((data & 0x00E) >> 1));
        break;
    case 0x05: {                                   /* VSRAM */
        uint32_t i = (d->addr & 0x7E) >> 1;
        if (i < 40) d->vsram[i] = data & 0x7FF;
        break;
    }
    default:                                       /* read codes / invalid: no effect */
        break;
    }
    d->addr = (d->addr + d->reg[15]) & 0xFFFF;
}

/* ------------------------------------------------------------------ */
/* DMA                                                                */
/* ------------------------------------------------------------------ */

static uint32_t dma_length(void) {
    uint32_t len = (g_ps_vdp.reg[20] << 8) | g_ps_vdp.reg[19];
    return len ? len : 0x10000;
}

static void dma_done(uint32_t src_words) {
    PsVdp *d = &g_ps_vdp;
    d->reg[19] = d->reg[20] = 0;
    d->reg[21] = (uint8_t)src_words;
    d->reg[22] = (uint8_t)(src_words >> 8);
}

/* 68000 memory -> VRAM/CRAM/VSRAM. The source walks a 128 KB window. */
uint32_t g_ps_vdp_dma68k_words;   /* last 68K-bus DMA, for the CPU stall (bus.c) */
bool     g_ps_vdp_dma68k_vram;

static void dma_from_68k(void) {
    PsVdp *d = &g_ps_vdp;
    uint32_t len = dma_length();
    if (s_dma_delayed && s_dma_delayed(d->reg[23]) && len > 1) {   /* Word RAM: one word late */
        d->addr = (d->addr + d->reg[15]) & 0xFFFF;
        len--;
    }
    g_ps_vdp_dma68k_words = len;
    g_ps_vdp_dma68k_vram = (d->code & 0x0F) == 0x01;
    uint32_t src = (d->reg[22] << 8) | d->reg[21];          /* in words */
    uint32_t high = (uint32_t)(d->reg[23] & 0x7F) << 17;
    while (len--) {
        uint16_t w = s_peek16 ? s_peek16(high | ((src << 1) & 0x1FFFE)) : 0;
        bus_write(w);
        src = (src + 1) & 0xFFFF;
    }
    dma_done(src);
}

/* VRAM -> VRAM byte copy (the VDP addresses VRAM bytes with A0 swapped). */
uint32_t g_ps_vdp_slow_len;    /* last VRAM fill/copy DMA (bytes) and kind (2 fill, 3 copy): busy time */
int      g_ps_vdp_slow_type;

static void dma_copy(void) {
    PsVdp *d = &g_ps_vdp;
    uint32_t len = dma_length();
    g_ps_vdp_slow_len = len; g_ps_vdp_slow_type = 3;
    uint32_t src = (d->reg[22] << 8) | d->reg[21];
    if (!(d->code & 0x10)) return;   /* CD4 clear: the real chip locks up */
    while (len--) {
        vram_write_byte(d->addr ^ 1, d->vram[(src ^ 1) & 0xFFFF]);
        src = (src + 1) & 0xFFFF;
        d->addr = (d->addr + d->reg[15]) & 0xFFFF;
    }
    dma_done(src);
}

/* Fill, armed by the control write and run after the next data write. */
static void dma_fill(void) {
    PsVdp *d = &g_ps_vdp;
    uint32_t len = dma_length();
    g_ps_vdp_slow_len = len; g_ps_vdp_slow_type = 2;
    switch (d->code & 0x0F) {
    case 0x01: {
        uint8_t v = (uint8_t)(d->last_data >> 8);
        while (len--) {
            vram_write_byte(d->addr ^ 1, v);
            d->addr = (d->addr + d->reg[15]) & 0xFFFF;
        }
        break;
    }
    case 0x03:
    case 0x05:
        while (len--) bus_write(d->last_data);
        break;
    }
    d->reg[19] = d->reg[20] = 0;
}

/* ------------------------------------------------------------------ */
/* Ports                                                              */
/* ------------------------------------------------------------------ */

static void reg_write(int r, uint8_t v) {
    if (r < 24) g_ps_vdp.reg[r] = v;
}

static void ctrl_write(uint16_t data) {
    PsVdp *d = &g_ps_vdp;
    if (!d->pending) {
        d->addr = (d->addr & 0xC000) | (data & 0x3FFF);
        d->code = (uint8_t)((d->code & 0x3C) | ((data >> 14) & 0x03));
        if ((data & 0xC000) == 0x8000)
            reg_write((data >> 8) & 0x1F, (uint8_t)data);
        else
            d->pending = (d->reg[1] & 0x04) != 0;   /* two-word commands: Mode 5 only */
        return;
    }
    d->pending = false;
    d->addr = ((uint32_t)(data & 3) << 14) | (d->addr & 0x3FFF);
    d->code = (uint8_t)((d->code & 0x03) | ((data >> 2) & 0x3C));
    if ((d->code & 0x20) && (d->reg[1] & 0x10)) {      /* CD5 + DMA enabled */
        switch (d->reg[23] >> 6) {
        case 2:  d->dma_fill = true; break;
        case 3:  dma_copy(); break;
        default: dma_from_68k(); break;
        }
    }
}

static void data_write(uint16_t data) {
    PsVdp *d = &g_ps_vdp;
    d->pending = false;
    bus_write(data);
    if (d->dma_fill) {
        d->dma_fill = false;
        dma_fill();
    }
}

static uint16_t data_read(void) {
    PsVdp *d = &g_ps_vdp;
    uint16_t v = 0;
    d->pending = false;
    switch (d->code & 0x1F) {
    case 0x00: v = (uint16_t)((d->vram[d->addr & 0xFFFE] << 8) | d->vram[(d->addr & 0xFFFE) + 1]); break;
    case 0x04: { uint32_t i = (d->addr & 0x7E) >> 1; v = (i < 40 ? d->vsram[i] : d->vsram[0]) & 0x7FF; break; }
    case 0x08: { uint16_t c = d->cram[(d->addr >> 1) & 0x3F];
                 v = (uint16_t)(((c & 0x1C0) << 3) | ((c & 0x038) << 2) | ((c & 0x007) << 1)); break; }
    case 0x0C: v = d->vram[(d->addr ^ 1) & 0xFFFF]; break;
    default: break;
    }
    d->addr = (d->addr + d->reg[15]) & 0xFFFF;
    return v;
}

static void clock_now(int *line, int *cyc, int *lpf) {
    *line = 0; *cyc = 0; *lpf = 262;
    if (s_clock) s_clock(line, cyc, lpf);
}

static uint16_t status_read(uint32_t pc) {
    PsVdp *d = &g_ps_vdp;
    int line, cyc, lpf;
    clock_now(&line, &cyc, &lpf);
    uint16_t s = 0x0200;                                  /* FIFO empty: writes complete instantly */
    if (line >= ps_vdp_height() || !ps_vdp_display_enabled()) s |= 0x08;
    bool h40 = (d->reg[12] & 1) != 0;
    int hs = h40 ? HBLANK_H40_START : HBLANK_H32_START;
    int he = h40 ? HBLANK_H40_END   : HBLANK_H32_END;
    if (cyc >= hs && cyc < he) s |= 0x04;
    d->pending = false;
    /* Bits 15-10 are open bus: the next prefetched opcode word. */
    if (s_peek16) s |= s_peek16(pc & 0xFFFFFE) & 0xFC00;
    return s;
}

static uint16_t hv_read(void) {
    int line, cyc, lpf;
    clock_now(&line, &cyc, &lpf);
    int vmax = (ps_vdp_height() == 240) ? 0x10A : 0xEA;   /* NTSC V28 jumps $EA -> $E5 */
    int vc = line > vmax ? line - lpf : line;
    /* H counter: 0..$B6 then $E4..$FF in H40 (0..$93, $E9..$FF in H32), spread over the line. */
    bool h40 = (g_ps_vdp.reg[12] & 1) != 0;
    int steps = h40 ? 210 : 171;
    int n = (int)((long)cyc * steps / MCLK_PER_LINE);
    int hc = h40 ? (n <= 0xB6 ? n : n + (0xE4 - 0xB7)) : (n <= 0x93 ? n : n + (0xE9 - 0x94));
    return (uint16_t)(((vc & 0xFF) << 8) | (hc & 0xFF));
}

void ps_vdp_write16(uint32_t offset, uint16_t data) {
    switch (offset & 0x1C) {
    case 0x00: data_write(data); break;
    case 0x04: ctrl_write(data); break;
    default: break;   /* PSG, test register: not video */
    }
}

void ps_vdp_write8(uint32_t offset, uint8_t data) {
    ps_vdp_write16(offset, (uint16_t)((data << 8) | data));
}

uint16_t ps_vdp_read16(uint32_t offset, uint32_t pc) {
    switch (offset & 0x1C) {
    case 0x00: return data_read();
    case 0x04: return status_read(pc);
    case 0x08:
    case 0x0C: return hv_read();
    default:   return 0xFFFF;
    }
}

uint8_t ps_vdp_read8(uint32_t offset, uint32_t pc) {
    uint16_t w = ps_vdp_read16(offset & ~1u, pc);
    return (offset & 1) ? (uint8_t)w : (uint8_t)(w >> 8);
}

void ps_state_blob(void *p, unsigned long n);
void ps_vdp_state(void) {
#define B(v) ps_state_blob(&(v), sizeof (v))
    B(g_ps_vdp); B(g_ps_vdp_dma68k_words); B(g_ps_vdp_dma68k_vram); B(g_ps_vdp_slow_len); B(g_ps_vdp_slow_type);
#undef B
}
