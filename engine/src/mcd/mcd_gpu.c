/*
 * mcd_gpu.c -- the native Mega CD's graphics ASIC: stamp-map rotation and scaling into Word RAM
 * (2 Mbit mode), one image line every 5 x hdots sub-CPU clocks, level 1 when the image is done;
 * the font registers. Ported from ares (ares/md/mcd at commit dc4f4659d496: gpu.cpp and the
 * $FF804C-$FF8066 registers of io-internal.cpp); the ISC notice is in mcd.h. Timing is ares's;
 * the reference player's may differ (checked by the stage starts).
 */
#include "mcd.h"
#include <string.h>

static struct {
    struct { uint8_t bg, fg; uint16_t data; } font;
    struct { bool repeat, tile_size, map_size; uint32_t map_base, map_address; } stamp;
    struct { uint32_t base, address; uint8_t offset, vcells, vdots; uint16_t hdots; } image;
    struct { uint32_t base, address; } vector;
    bool active;
    unsigned counter, period;
} g;

static uint8_t nib_read(uint32_t a) { a &= 0x7FFFF; unsigned lo = 12 - ((a & 3) << 2); return (uint8_t)(g_mcd.wram[(a >> 2) & 0x1FFFF] >> lo & 0xF); }
static void nib_write(uint32_t a, uint8_t v) {
    a &= 0x7FFFF; unsigned lo = 12 - ((a & 3) << 2); uint16_t *w = &g_mcd.wram[(a >> 2) & 0x1FFFF];
    *w = (uint16_t)((*w & ~(0xFu << lo)) | (v & 0xFu) << lo);
}

/* one line of the image: WIDTH dots from the trace vector's start point and steps */
static void render(uint32_t address, unsigned width) {
    unsigned stamp_shift = 4, map_shift = 4u << g.stamp.map_size, index_mask = 0x7FF, pix_mask = 0x0F;
    if (g.stamp.tile_size) { stamp_shift++; map_shift--; index_mask &= ~3u; pix_mask |= 0x10; }
    uint32_t image_width = (uint32_t)(g.image.vcells + 1) << 6;
    uint32_t map_mask = !g.stamp.map_size ? 0x07FFFF : 0x7FFFFF;
    const uint16_t *wram = g_mcd.wram;
    uint32_t x = (uint32_t)wram[g.vector.address++ & 0x1FFFF] << 8;   /* 13.3 -> 13.11 */
    uint32_t y = (uint32_t)wram[g.vector.address++ & 0x1FFFF] << 8;
    int16_t xstep = (int16_t)wram[g.vector.address++ & 0x1FFFF], ystep = (int16_t)wram[g.vector.address++ & 0x1FFFF];
    while (width--) {
        if (g.stamp.repeat) { x &= map_mask; y &= map_mask; }
        x &= 0xFFFFFF; y &= 0xFFFFFF;
        uint8_t out = 0;
        if (!((x | y) & ~map_mask)) {
            uint32_t xt = x >> 11, yt = y >> 11;
            uint32_t entry = wram[(g.stamp.map_address + ((yt >> stamp_shift) << map_shift) + (xt >> stamp_shift)) & 0x1FFFF];
            unsigned index = entry & index_mask & 0x7FF;
            if (index) {
                if (entry >> 15 & 1) xt = ~xt;                                    /* h flip */
                if (entry >> 14 & 1) { xt = ~xt; yt = ~yt; }                      /* 180 degrees */
                if (entry >> 13 & 1) { uint32_t t = xt; xt = ~yt; yt = t; }       /* 90 degrees */
                unsigned xp = xt & pix_mask, yp = yt & pix_mask;
                out = nib_read((uint32_t)index << 8 | (xp & ~7u) << stamp_shift | yp << 3 | (xp & 7));
            }
        }
        uint8_t in = nib_read(address);
        switch (g_mcd.io.wram_priority) { case 1: out = in ? in : out; break; case 2: out = out ? out : in; break; case 3: out = in; break; }
        nib_write(address, out);
        if (!(++address & 7)) address += image_width - 8;
        x += (uint32_t)(int32_t)xstep; y += (uint32_t)(int32_t)ystep;
    }
}

static void start(void) {
    if (g_mcd.io.wram_mode) return;                        /* 2 Mbit Word RAM only */
    g.active = true;
    g.period = 5u * g.image.hdots;
    g.counter = 0;
    g.image.address = (g.image.base << 1) + g.image.offset;
    g.vector.address = g.vector.base >> 1;
    uint32_t m = g.stamp.map_base >> 1;
    if (!g.stamp.map_size && !g.stamp.tile_size) m &= 0x1FF00;
    if (!g.stamp.map_size && g.stamp.tile_size) m &= 0x1FFC0;
    if (g.stamp.map_size && !g.stamp.tile_size) m &= 0x10000;
    if (g.stamp.map_size && g.stamp.tile_size) m &= 0x1C000;
    g.stamp.map_address = m;
}

/* the gate array's clock (mcd.c) */
void mcd_gpu_step(unsigned clocks) {
    if (!g.active || !g.period) return;
    g.counter += clocks;
    while (g.counter >= g.period) {
        g.counter -= g.period;
        render(g.image.address, g.image.hdots);
        g.image.address += 8;
        if (!--g.image.vdots) {
            g.active = false;
            McdIrq *q = &g_mcd.gpu_irq;
            if (!q->pending) { q->pending = true; mcd_irq_sync(); }
            break;
        }
    }
}

uint16_t mcd_gpu_read(uint32_t a) {
    switch (a) {
    case 0xFF804C: return (uint16_t)(g.font.bg | g.font.fg << 4);
    case 0xFF804E: return g.font.data;
    case 0xFF8058: return (uint16_t)(g.stamp.repeat | g.stamp.tile_size << 1 | g.stamp.map_size << 2 | g.active << 15);
    case 0xFF805A: return (uint16_t)(((g.stamp.map_base >> 9) & 0x1FF) << 7);
    case 0xFF805C: return g.image.vcells & 0x1F;
    case 0xFF805E: return (uint16_t)(((g.image.base >> 5) & 0x1FFF) << 3);
    case 0xFF8060: return g.image.offset & 0x3F;
    case 0xFF8062: return g.image.hdots & 0x1FF;
    case 0xFF8064: return g.image.vdots;
    case 0xFF8066: return (uint16_t)(((g.vector.base >> 3) & 0x7FFF) << 1);
    }
    if (a >= 0xFF8050 && a <= 0xFF8057) {                  /* font: 4 dots of the 16-bit pattern */
        unsigned i = 12 - ((a - 0xFF8050) << 1);
        uint16_t v = 0;
        for (int k = 0; k < 4; k++) v |= (uint16_t)(((g.font.data >> (i | k)) & 1 ? g.font.fg : g.font.bg) << (4 * k));
        return v;
    }
    return 0;
}
void mcd_gpu_write(uint32_t a, bool upper, bool lower, uint16_t v) {
    switch (a) {
    case 0xFF804C: g.font.bg = v & 0xF; g.font.fg = v >> 4 & 0xF; return;
    case 0xFF804E: if (lower) g.font.data = (uint16_t)((g.font.data & 0xFF00) | (v & 0xFF));
                   if (upper) g.font.data = (uint16_t)((g.font.data & 0x00FF) | (v & 0xFF00)); return;
    case 0xFF8058: if (lower) { g.stamp.repeat = v & 1; g.stamp.tile_size = v >> 1 & 1; g.stamp.map_size = v >> 2 & 1; } return;
    case 0xFF805A: g.stamp.map_base = (g.stamp.map_base & 0x7F) | (uint32_t)((v >> 5) & 0x7FF) << 7; return;
    case 0xFF805C: g.image.vcells = v & 0x1F; return;
    case 0xFF805E: g.image.base = (g.image.base & 0x1F) | (uint32_t)((v >> 3) & 0x1FFF) << 5; return;
    case 0xFF8060: g.image.offset = v & 0x3F; return;
    case 0xFF8062: g.image.hdots = v & 0x1FF; return;
    case 0xFF8064: g.image.vdots = (uint8_t)v; return;
    case 0xFF8066: g.vector.base = (g.vector.base & 7) | (uint32_t)((v >> 1) & 0x7FFF) << 3; start(); return;
    }
}
void mcd_gpu_power(void) { memset(&g, 0, sizeof g); }
