/*
 * mcd_bus.c -- the native Mega CD's sub-CPU bus and gate-array registers ($FF8000-$FF81FF),
 * its interrupt sources and timer. Ported from ares (ares/md/mcd at commit dc4f4659d496:
 * bus-internal.cpp, io-internal.cpp, irq.cpp, timer.cpp); the ISC notice is in mcd.h.
 * Differences: C; Word RAM written while the main CPU holds it (2 Mbit mode) is dropped instead
 * of holding the sub CPU (the interpreter does not stall on the bus); the CDC, CDD and graphics
 * registers are their parts' (mcd.h).
 */
#include "mcd.h"
#include "genrecomp/s68k.h"
#include <stddef.h>
#include <string.h>

Mcd g_mcd;

static uint16_t io_read(bool upper, bool lower, uint32_t a, uint16_t data);
static void io_write(bool upper, bool lower, uint32_t a, uint16_t data);

/* 1 Mbit mode: the half of Word RAM the sub CPU owns, word index for a 17-bit offset */
static uint32_t wram_1m(uint32_t a) { return (a & 0x1FFFE) | (g_mcd.io.wram_select ? 0 : 1); }   /* banks interleave by word */

static uint16_t mcd_read(bool upper, bool lower, uint32_t a, uint16_t data) {
    Mcd *m = &g_mcd;
    a &= 0xFFFFFF;
    if (a <= 0x07FFFF) return m->pram[a >> 1];
    if (a <= 0x0BFFFF) {
        if (!m->io.wram_mode) return m->io.wram_switch ? m->wram[(a & 0x3FFFF) >> 1] : data;
        /* 1 Mbit dot-mapped window: each byte of the image is one 4-bit dot */
        int ofs = (a & 2) ? 0 : 8;
        uint16_t w = m->wram[(((a >> 1) & 0x1FFFF) & ~1u) | (m->io.wram_select ? 0 : 1)];
        return (uint16_t)(((w >> (4 + ofs)) & 0xF) << 8 | ((w >> ofs) & 0xF));
    }
    if (a <= 0x0DFFFF) return m->io.wram_mode ? m->wram[wram_1m(a)] : data;
    if (a >= 0xFE0000 && a <= 0xFEFFFF) return lower ? m->bram[(a >> 1) & 0x1FFF] : data;
    if (a >= 0xFF0000 && a <= 0xFF7FFF) return lower ? mcd_pcm_read((a >> 1) & 0x3FFF) : data;
    if (a >= 0xFF8000) return io_read(upper, lower, a, data);
    return data;
}

static void mcd_write(bool upper, bool lower, uint32_t a, uint16_t data) {
    Mcd *m = &g_mcd;
    a &= 0xFFFFFF;
    if (a <= 0x07FFFF) {
        if (a < (uint32_t)m->io.pram_protect << 9) return;
        uint16_t *w = &m->pram[a >> 1];
        if (upper) *w = (uint16_t)((*w & 0x00FF) | (data & 0xFF00));
        if (lower) *w = (uint16_t)((*w & 0xFF00) | (data & 0x00FF));
        return;
    }
    if (a <= 0x0BFFFF) {
        if (!m->io.wram_mode) {
            if (!m->io.wram_switch) return;            /* held by the main CPU */
            uint16_t *w = &m->wram[(a & 0x3FFFF) >> 1];
            if (upper) *w = (uint16_t)((*w & 0x00FF) | (data & 0xFF00));
            if (lower) *w = (uint16_t)((*w & 0xFF00) | (data & 0x00FF));
            return;
        }
        int ofs = (a & 2) ? 0 : 8;
        uint16_t *w = &m->wram[(((a >> 1) & 0x1FFFF) & ~1u) | (m->io.wram_select ? 0 : 1)];
        unsigned hi = (data >> 8) & 0xF, lo = data & 0xF;
        unsigned cur_hi = (*w >> (4 + ofs)) & 0xF, cur_lo = (*w >> ofs) & 0xF;
        bool wh = upper, wl = lower;
        if (m->io.wram_priority == 1) { wh = wh && !cur_hi; wl = wl && !cur_lo; }      /* under */
        else if (m->io.wram_priority == 2) { wh = wh && hi; wl = wl && lo; }           /* over */
        else if (m->io.wram_priority == 3) return;                                     /* invalid */
        if (wh) *w = (uint16_t)((*w & ~(0xFu << (4 + ofs))) | hi << (4 + ofs));
        if (wl) *w = (uint16_t)((*w & ~(0xFu << ofs)) | lo << ofs);
        return;
    }
    if (a <= 0x0DFFFF) {
        if (!m->io.wram_mode) return;
        uint16_t *w = &m->wram[wram_1m(a)];
        if (upper) *w = (uint16_t)((*w & 0x00FF) | (data & 0xFF00));
        if (lower) *w = (uint16_t)((*w & 0xFF00) | (data & 0x00FF));
        return;
    }
    if (a >= 0xFE0000 && a <= 0xFEFFFF) { if (lower) m->bram[(a >> 1) & 0x1FFF] = (uint8_t)data; return; }
    if (a >= 0xFF0000 && a <= 0xFF7FFF) { if (lower) mcd_pcm_write((a >> 1) & 0x3FFF, (uint8_t)data); return; }
    if (a >= 0xFF8000) io_write(upper, lower, a, data);
}

/* the sub CPU's bus (s68k.h): byte accesses are word accesses with one lane */
uint8_t s68k_read8(uint32_t a) {
    uint16_t v = mcd_read(!(a & 1), a & 1, a & ~1u, 0);
    return (uint8_t)((a & 1) ? v : v >> 8);
}
uint16_t s68k_read16(uint32_t a) { return mcd_read(true, true, a & ~1u, 0); }
uint32_t s68k_read32(uint32_t a) { return (uint32_t)s68k_read16(a) << 16 | s68k_read16(a + 2); }
void s68k_write8(uint32_t a, uint8_t v) { mcd_write(!(a & 1), a & 1, a & ~1u, (uint16_t)(v << 8 | v)); }
void s68k_write16(uint32_t a, uint16_t v) { mcd_write(true, true, a & ~1u, v); }
void s68k_write32(uint32_t a, uint32_t v) { s68k_write16(a, (uint16_t)(v >> 16)); s68k_write16(a + 2, (uint16_t)v); }

/* ------------------------------------------------------------------ gate array */
static uint16_t io_read(bool upper, bool lower, uint32_t a, uint16_t data) {
    Mcd *m = &g_mcd;
    (void)upper;                                             /* no register here reads one byte lane alone */
    a = 0xFF8000 | (a & 0x1FF);
    switch (a) {
    case 0xFF8000: return (uint16_t)(1 | m->led.red << 8 | m->led.green << 9);    /* peripheral ready, version 0 */
    case 0xFF8002: {
        bool b0 = !m->io.wram_mode ? !m->io.wram_switch : m->io.wram_select;
        bool b1 = !m->io.wram_mode ? m->io.wram_switch : m->io.wram_switch_request;
        return (uint16_t)(b0 | b1 << 1 | m->io.wram_mode << 2 | (m->io.wram_priority & 3) << 3 | m->io.pram_protect << 8);
    }
    case 0xFF8004: return mcd_cdc_status();
    case 0xFF8006: return lower ? mcd_cdc_reg_read() : data;
    case 0xFF8008: return mcd_cdc_host_read();
    case 0xFF800A: return mcd_cdc_dma_address();
    case 0xFF800C: return mcd_cdc_stopwatch();
    case 0xFF800E: return (uint16_t)(m->comm.cfm << 8 | m->comm.cfs);
    case 0xFF8030: return m->timer.frequency;
    case 0xFF8032: return (uint16_t)(m->gpu_irq.enable << 1 | m->external.irq.enable << 2 | m->timer.irq.enable << 3 |
                                     m->cdd_irq.enable << 4 | m->cdc_irq.enable << 5 | m->irq.subcode.enable << 6);
    }
    if (a >= 0xFF8010 && a <= 0xFF801F) return m->comm.command[(a - 0xFF8010) >> 1];
    if (a >= 0xFF8020 && a <= 0xFF802F) return m->comm.status[(a - 0xFF8020) >> 1];
    if ((a >= 0xFF8034 && a <= 0xFF804B) || a == 0xFF8068 || a >= 0xFF8100) return mcd_cdd_read(a);
    if (a >= 0xFF804C && a <= 0xFF8066) return mcd_gpu_read(a);
    return data;
}

static void io_write(bool upper, bool lower, uint32_t a, uint16_t data) {
    Mcd *m = &g_mcd;
    a = 0xFF8000 | (a & 0x1FF);
    switch (a) {
    case 0xFF8000:
        if (lower && !(data & 1)) {                          /* RESET cleared: the CD hardware is reset (as the reference) */
            void mcd_cdd_power(void), mcd_cdd_rephase(long), mcd_cdc_power(void), mcd_parts_power(void), mcd_cdc_stopwatch_reset(void);
            extern unsigned g_mcd_line_scyc;
            m->timer.frequency = m->timer.counter = 0;
            m->gpu_irq = m->external.irq = m->timer.irq = m->cdd_irq = m->cdc_irq = m->irq.subcode = (McdIrq){ 0 };
            mcd_irq_sync();
            mcd_cdd_power(); mcd_cdd_rephase((long)((g_scyc - g_mcd_line_scyc) / 7));
            mcd_cdc_power(); mcd_parts_power(); mcd_cdc_stopwatch_reset();
        }
        if (upper) { m->led.red = data >> 8 & 1; m->led.green = data >> 9 & 1; }
        return;
    case 0xFF8002:                                           /* an 8-bit register: writes go to the low byte */
        if (m->io.wram_select != (data & 1)) m->io.wram_switch_request = false;
        m->io.wram_select = data & 1;
        m->io.wram_mode = data >> 2 & 1;
        m->io.wram_priority = data >> 3 & 3;
        if (m->io.wram_select) m->io.wram_switch = false;
        return;
    case 0xFF8004: mcd_cdc_ctrl_write(upper, lower, data); return;
    case 0xFF8006: if (lower) mcd_cdc_reg_write((uint8_t)data); return;
    case 0xFF800A: mcd_cdc_dma_address_write(data); return;
    case 0xFF800C: mcd_cdc_stopwatch_reset(); return;
    case 0xFF800E: m->comm.cfs = (uint8_t)data; return;
    case 0xFF8030: m->timer.counter = m->timer.frequency = (uint8_t)data; return;
    case 0xFF8032:
        if (lower) {
            m->gpu_irq.enable = data >> 1 & 1; m->external.irq.enable = data >> 2 & 1; m->timer.irq.enable = data >> 3 & 1;
            m->cdd_irq.enable = data >> 4 & 1; m->cdc_irq.enable = data >> 5 & 1; m->irq.subcode.enable = data >> 6 & 1;
            mcd_irq_sync();
        }
        return;
    }
    if (a >= 0xFF8020 && a <= 0xFF802F) {
        uint16_t *s = &m->comm.status[(a - 0xFF8020) >> 1];
        if (lower) *s = (uint16_t)((*s & 0xFF00) | (data & 0x00FF));
        if (upper) *s = (uint16_t)((*s & 0x00FF) | (data & 0xFF00));
        return;
    }
    if ((a >= 0xFF8034 && a <= 0xFF804B) || a >= 0xFF8100) { mcd_cdd_write(a, upper, lower, data); return; }
    if (a >= 0xFF804C && a <= 0xFF8066) mcd_gpu_write(a, upper, lower, data);
}

/* ------------------------------------------------------------------ interrupts, timer */
/* the sub CPU's level: 1 graphics, 2 main CPU request, 3 timer, 4 CDD, 5 CDC, 6 subcode */
void mcd_irq_sync(void) {
    Mcd *m = &g_mcd;
    const McdIrq *src[7] = { NULL, &m->gpu_irq, &m->external.irq, &m->timer.irq, &m->cdd_irq, &m->cdc_irq, &m->irq.subcode };
    m->irq.pending = m->irq.reset.pending;
    for (int l = 1; l <= 6; l++) {
        if (src[l]->pending && src[l]->enable) { s68k_set_irq(l); m->irq.pending = true; }   /* latched, masked by enable (GPGX) */
        else s68k_clear_irq(l);
    }
}
unsigned long g_mcd_irq_taken[8];
void s68k_irq_acknowledge(int level) {
    Mcd *m = &g_mcd;
    if (level >= 0 && level < 8) g_mcd_irq_taken[level]++;
    McdIrq *src[7] = { NULL, &m->gpu_irq, &m->external.irq, &m->timer.irq, &m->cdd_irq, &m->cdc_irq, &m->irq.subcode };
    if (level >= 1 && level <= 6) { src[level]->pending = false; mcd_irq_sync(); }
}
static void irq_raise(McdIrq *q) { if (!q->pending) { q->pending = true; mcd_irq_sync(); } }
void mcd_timer_clock(void) {
    Mcd *m = &g_mcd;
    if (m->timer.frequency && !m->timer.counter--) { m->timer.counter = m->timer.frequency; irq_raise(&m->timer.irq); }
}

void mcd_power(bool reset) {
    Mcd *m = &g_mcd;
    uint32_t vec4 = m->io.vector_level4;
    memset(&m->io, 0, sizeof m->io); m->io.halt = true;
    m->io.vector_level4 = reset ? vec4 : 0xFFFFFFFFu;
    memset(&m->comm, 0, sizeof m->comm);
    memset(&m->timer, 0, sizeof m->timer);
    m->external.irq = m->gpu_irq = m->cdd_irq = m->cdc_irq = (McdIrq){ 0 };
    m->irq.reset = m->irq.subcode = (McdIrq){ 0 }; m->irq.pending = false;
}
