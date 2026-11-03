/*
 * mcd_main.c -- the native Mega CD seen from the main CPU: the BIOS and a PRG-RAM bank at
 * $000000-$03FFFF (booted from CD), Word RAM at $200000-$23FFFF (2 Mbit whole, or the main CPU's
 * 1 Mbit half with its cell-mapped windows), and the gate array's registers at $A12000-$A1203F.
 * Ported from ares (ares/md/mcd at commit dc4f4659d496: bus-external.cpp, io-external.cpp); the
 * ISC notice is in mcd.h. Differences: C; the LaserActive paths left out.
 */
#include "mcd.h"
#include "genrecomp/s68k.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include "genrecomp/m68k.h"
#include "genrecomp/func_table.h"

/* 1 Mbit mode, $220000-$23FFFF: cell-mapped views (V32, V16, V8, V4) of the main CPU's half */
static uint32_t cell_mapped(uint32_t a) {
    if (a < 0x220000) return a;
    if (a < 0x230000) return (a & 0x0FC00) >> 8 | (a & 0x003FC) << 6 | (a & 0x10002);
    if (a < 0x238000) return (a & 0x07E00) >> 7 | (a & 0x001FC) << 6 | (a & 0x18002);
    if (a < 0x23C000) return (a & 0x03F00) >> 6 | (a & 0x000FC) << 6 | (a & 0x1C002);
    return (a & 0x01F80) >> 5 | (a & 0x0007C) << 6 | (a & 0x1E002);
}
/* the word index of a main-CPU Word RAM address, or -1 when the main CPU does not hold it */
static long wram_index(uint32_t a) {
    Mcd *m = &g_mcd;
    if (!m->io.wram_mode) return m->io.wram_switch ? -1 : (long)((a & 0x3FFFF) >> 1);
    return (long)((cell_mapped(a) & 0x1FFFE) | (m->io.wram_select ? 1 : 0));
}

/* the main CPU's reads and writes in the Mega CD's ranges (addresses with the $380000 mirrors
 * folded); dma: a VDP DMA source read, answered one access late as the hardware does (games
 * correct the first word themselves) */
/* the backup RAM cartridge in the cartridge port, as the reference player attaches one when booting
 * from a disc: odd bytes only (even bytes and word high bytes read $FF); $400000-$5FFFFF its size code
 * (6: 512 KB), $600000-$6FFFFF the RAM, $700000-$7FFFFF the write-protect register */
static uint16_t cart_read(uint32_t a) {
    Mcd *m = &g_mcd;
    if (a < 0x600000) return 0xFF00 | 6;
    if (a < 0x700000) return 0xFF00 | m->cart[(a >> 1) & 0x7FFFF];
    return 0xFF00 | m->cart_prot;
}
static void cart_write(uint32_t a, bool lower, uint16_t data) {
    Mcd *m = &g_mcd;
    if (!lower || a < 0x600000) return;
    if (a < 0x700000) { if (m->cart_prot & 1) m->cart[(a >> 1) & 0x7FFFF] = (uint8_t)data; return; }
    m->cart_prot = (uint8_t)data;
}

uint16_t mcd_main_read(uint32_t a, uint16_t data, bool dma) {
    Mcd *m = &g_mcd;
    if ((a & 0xFFFFFF) >= 0x400000) return cart_read(a & 0x7FFFFF);
    a &= 0x3FFFFF; a &= ~0x1C0000u;                          /* bits 18-20: mirrors */
    if (a <= 0x01FFFF) {
        if (a == 0x70) return (uint16_t)(m->io.vector_level4 >> 16);
        if (a == 0x72) return (uint16_t)m->io.vector_level4;
        return m->bios[a >> 1];
    }
    if (a <= 0x03FFFF) return m->io.request ? m->pram[((uint32_t)m->io.pram_bank << 17 | (a & 0x1FFFF)) >> 1] : data;
    if (a >= 0x200000 && a <= 0x23FFFF) {
        long i = wram_index(a);
        if (i < 0) return data;
        if (!dma) return m->wram[i];
        static uint16_t latch;
        uint16_t v = latch; latch = m->wram[i]; return v;
    }
    return data;
}
/* a VDP DMA from Word RAM ($200000-$23FFFF, booted from the disc): its first word is the one still
 * on the bus, the source words follow one late and the last is dropped (ps_vdp.c) */
int mcd_dma_delayed(unsigned reg23) { extern bool g_ps_mcd; return g_ps_mcd && (reg23 & 0x70) == 0x10; }

void mcd_main_write(uint32_t a, bool upper, bool lower, uint16_t data) {
    Mcd *m = &g_mcd;
    if ((a & 0xFFFFFF) >= 0x400000) { cart_write(a & 0x7FFFFF, lower, data); return; }
    a &= 0x3FFFFF; a &= ~0x1C0000u;
    uint16_t *w = NULL;
    if (a >= 0x020000 && a <= 0x03FFFF) { if (!m->io.request) return; w = &m->pram[((uint32_t)m->io.pram_bank << 17 | (a & 0x1FFFF)) >> 1]; }
    else if (a >= 0x200000 && a <= 0x23FFFF) { long i = wram_index(a); if (i < 0) return; w = &m->wram[i]; }
    if (!w) return;
    if (upper) *w = (uint16_t)((*w & 0x00FF) | (data & 0xFF00));
    if (lower) *w = (uint16_t)((*w & 0xFF00) | (data & 0x00FF));
}

/* the main CPU's register accesses where the reference syncs the sub CPU or watches for polling
 * (mcd.c); SIZE 1 or 2, V the value written, before the access lands */
void mcd_main_io_access(uint32_t a, int size, bool write, uint16_t v) {
    extern unsigned int g_cyc; extern uint32_t mcycles_vdp, g_m68k_pf; extern M68kContext g_m68k;
    unsigned rel = g_cyc - mcycles_vdp, pc = g_m68k_pf ? g_m68k_pf : g_m68k.pc, i = a & 0x3F;
    if (!write) {
        if (size == 1) {
            if (i == 0x03) mcd_main_poll_detect(1u << 3, g_cyc, pc);
            else if (i == 0x04) mcd_sync_sub(rel);
            else if (i == 0x0F) { mcd_sync_sub(rel); mcd_main_poll_detect(1u << 0x0F, g_cyc, pc); }
            else if (i >= 0x20 && i < 0x30) mcd_main_poll_detect(1u << (i - 0x10), g_cyc, pc);
        } else {
            if (i == 0x02) mcd_main_poll_detect(1u << 3, g_cyc, pc);
            else if (i == 0x08) { if (!(mcd_cdc_status() & 0x4000)) mcd_sync_sub(rel); }
            else if (i >= 0x20 && i < 0x30) { mcd_sync_sub(rel); mcd_main_poll_detect(3u << (i - 0x10), g_cyc, pc); }
        }
    } else if (size == 1) {
        if (i == 0x00) { if ((v & 0x100) && g_mcd.external.irq.enable) mcd_sync_sub(rel); }   /* IFL2: the level 2 interrupt */
        else if (i == 0x03) mcd_main_poll_sync(1u << 3, rel);
        else if (i == 0x0E || i == 0x0F) mcd_main_poll_sync(1u << 0x0E, rel);
        else if ((i & 0x30) == 0x10) mcd_main_poll_sync(1u << (i & 0x1F), rel);
    } else {
        if (i == 0x02) mcd_main_poll_sync(1u << 3, rel);
        else if (i == 0x0E) mcd_main_poll_sync(1u << 0x0E, rel);
        else if ((i & 0x30) == 0x10) mcd_main_poll_sync(3u << (i & 0x1E), rel);
    }
}
uint16_t mcd_main_io_read(uint32_t a, uint16_t data) {
    Mcd *m = &g_mcd;
    a = 0xA12000 | (a & 0x3F);                                /* $A12040-$A120FF mirror $A12000-$A1203F */
    {   static int on = -1; static unsigned long cnt[32], total;
        if (on < 0) on = getenv("PS_MCD_TRACE") != NULL;
        if (on) { cnt[(a & 0x3F) >> 1]++; if (++total % 2000000 == 0) {
            fprintf(stderr, "mcd: main reads:"); for (int i = 0; i < 32; i++) if (cnt[i]) fprintf(stderr, " %06X:%lu", 0xA12000 + 2 * i, cnt[i]);
            fprintf(stderr, " (main pc %06X)\n", g_m68k_pf); } } }
    switch (a) {
    case 0xA12000: return (uint16_t)(m->io.run | m->io.request << 1 | m->external.irq.pending << 8 | m->external.irq.enable << 15);
    case 0xA12002: {
        bool b0 = !m->io.wram_mode ? !m->io.wram_switch : m->io.wram_select;
        bool b1 = !m->io.wram_mode ? m->io.wram_switch : m->io.wram_switch_request;
        return (uint16_t)(b0 | b1 << 1 | m->io.wram_mode << 2 | (m->io.pram_bank & 3) << 6 | m->io.pram_protect << 8);
    }
    case 0xA12004: return (uint16_t)(mcd_cdc_status() & 0xFF00);
    case 0xA12006: return (uint16_t)m->io.vector_level4;
    case 0xA12008: { uint16_t mcd_cdc_host_cpu(int); return mcd_cdc_host_cpu(2); }   /* main-CPU host read */
    case 0xA1200C: return mcd_cdc_stopwatch();
    case 0xA1200E: return (uint16_t)(m->comm.cfm << 8 | m->comm.cfs);
    }
    if (a >= 0xA12010 && a <= 0xA1201F) return m->comm.command[(a - 0xA12010) >> 1];
    if (a >= 0xA12020 && a <= 0xA1202F) return m->comm.status[(a - 0xA12020) >> 1];
    return data;
}
void mcd_main_io_write(uint32_t a, bool upper, bool lower, uint16_t data) {
    Mcd *m = &g_mcd;
    a = 0xA12000 | (a & 0x3F);
    {   static int on = -1, n; if (on < 0) on = getenv("PS_MCD_TRACE") != NULL;
        if (on && a <= 0xA12002 && n++ < 200)
            fprintf(stderr, "mcd: main writes %06X %s%s %04X (run %d req %d)\n", a, upper ? "U" : "", lower ? "L" : "", data, m->io.run, m->io.request); }
    switch (a) {
    case 0xA12000:
        if (lower) {
            /* the documented forced gate-array reset sequence (FF00 to $A12002, 3, 2, 0 to $A12001)
             * is recognised and leaves the registers as they are; otherwise RESET and BUSREQ */
            if (!(m->io.request && !m->io.run && !(data & 3) && m->io.pram_protect == 0xFF && m->io.pram_bank == 0 &&
                  m->io.wram_switch_request)) {
                /* the vectors are read on the sub CPU's first step after RESET is released and the bus
                 * returned: the BIOS fills PRG-RAM in between (ares: resetCpu on assert, the reset
                 * interrupt reads them on the first step that runs) */
                if (!m->io.run && (data & 1)) m->irq.reset.pending = true;
                m->io.run = data & 1;
                m->io.request = m->io.run ? (data >> 1 & 1) : true;   /* reset held forces the bus request */
                m->io.halt = m->io.request;
            }
        }
        if (upper && (data & 0x100) && m->external.irq.enable && !m->external.irq.pending) { m->external.irq.pending = true; mcd_irq_sync(); }
        return;
    case 0xA12002:
        if (lower) {
            if (!(data & 2)) m->io.wram_switch_request = true;
            else m->io.wram_switch = true;                         /* DMNA: Word RAM to the sub CPU */
            m->io.pram_bank = data >> 6 & 3;
        }
        if (upper) m->io.pram_protect = (uint8_t)(data >> 8);
        return;
    case 0xA12006:
        if (upper) m->io.vector_level4 = (m->io.vector_level4 & 0x00FF) | (data & 0xFF00);
        if (lower) m->io.vector_level4 = (m->io.vector_level4 & 0xFF00) | (data & 0x00FF);
        return;
    case 0xA1200E: m->comm.cfm = (uint8_t)(data >> 8); return;    /* an 8-bit register: writes go to the high byte */
    }
    if (a >= 0xA12010 && a <= 0xA1201F) {
        uint16_t *c = &m->comm.command[(a - 0xA12010) >> 1];
        if (lower) *c = (uint16_t)((*c & 0xFF00) | (data & 0x00FF));
        if (upper) *c = (uint16_t)((*c & 0x00FF) | (data & 0xFF00));
    }
}

/* the overlay key (func_table.h), as the reference build's (ps_megacd.c): main-CPU writes to the memory
 * mode register (Word RAM handed over: a new program there) and over a compiled work RAM program */
extern uint32_t g_ps_mcd_mode_writes, g_ps_wram_code_writes;
static uint32_t mcd_overlay_key(uint32_t addr) {
    return addr >= 0xFF0000 ? g_ps_wram_code_writes : addr >= 0x200000 ? g_ps_mcd_mode_writes : 0;
}
void ps_megacd_overlays(void) { extern bool g_ps_mcd; if (g_ps_mcd) func_table_overlay_key = mcd_overlay_key; }
