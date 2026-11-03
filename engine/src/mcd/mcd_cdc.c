/*
 * mcd_cdc.c -- the native Mega CD's CD controller (Sanyo LC8951), behaving as the reference
 * player's (Genesis Plus GX, which the 74 stage starts are checked against): its interface status
 * and the level-5 rule (raised on the /INT falling edge with the gate array's enable), the decoder's
 * active period after each sector, the buffer holding each block as its 4-byte header then the
 * user data, the DMA to PRG-RAM / Word RAM / PCM RAM in 8-byte blocks at 4 sub clocks a byte (run
 * per line), halted while the main CPU holds the destination, and host reads through $FF8008 /
 * $A12008. The port's own code, written to match that behaviour (the reference's code is not
 * copied); the register map is the chip's. Sectors come from the player's disc (mcd_disc.c).
 */
#include "mcd.h"
#include "genrecomp/s68k.h"
#include <string.h>

bool mcd_disc_read(int lba, uint8_t raw[2352]);
void mcd_pcm_write(uint32_t offset, uint8_t v);

enum { DTEI = 0x40, DECI = 0x20, DTBSY = 0x08, DTEN = 0x02 };          /* IFSTAT (active low) */
enum { DTEIEN = 0x40, DECIEN = 0x20, DOUTEN = 0x02 };                  /* IFCTRL */
enum { DECEN = 0x80, AUTORQ = 0x10, WRRQ = 0x04 };                     /* CTRL0 */
enum { MODRQ = 0x08, FORMRQ = 0x04, SHDREN = 0x01 };                   /* CTRL1 */
enum { VALST = 0x80 };                                                  /* STAT3 */
enum { DMA_NONE, DMA_PCM, DMA_PRG, DMA_WRAM };
#define DMA_CLOCKS_PER_BYTE 4                       /* sub-CPU clocks */
#define DECODER_PERIOD 67250                        /* sub clocks the decoder interrupt stays active */

static struct {
    uint8_t ifstat, ifctrl, ctrl[2], head[2][4], stat[4], irq;     /* irq: the inverse of /INT */
    uint16_t dbc, dac, pt, wa;
    long cycles[2];                                 /* DMA position, decoder end (sub clocks) */
    int dma, halted;
    uint8_t address, destination;                   /* $FF8004: register index, DMA destination */
    bool dsr, edt;                                  /* $FF8004 bits 14, 15 */
    uint16_t host;                                  /* $FF8008 */
    uint16_t dma_address;                           /* $FF800A (8-byte units) */
    uint16_t stopwatch;
    uint8_t ram[0x4000 + 2352];                     /* 16 KB with one block of overrun */
} c;

static long now(void) { return (long)(g_scyc / 7); }

static void level5(void) {
    McdIrq *q = &g_mcd.cdc_irq;
    if (q->enable && !q->pending) { q->pending = true; mcd_irq_sync(); }   /* only while enabled (the reference) */
}
static void level5_clear(void) {
    McdIrq *q = &g_mcd.cdc_irq;
    if (q->pending) { q->pending = false; mcd_irq_sync(); }
}
/* the decoder interrupt ends once its active period is over */
static void decoder_expire(void) {
    if (now() > c.cycles[1]) { c.ifstat |= DECI; c.irq &= (uint8_t)~DECI; }
}
/* a transfer has ended: !DTEN and !DTBSY set, DTEI pending, level 5 on the /INT falling edge */
static void transfer_end(void) {
    c.dbc = 0xFFFF;
    c.ifstat |= DTBSY | DTEN;
    c.ifstat &= (uint8_t)~DTEI;
    if (c.ifctrl & DTEIEN) {
        if ((c.irq & DECI) && c.cycles[0] > c.cycles[1]) { c.ifstat |= DECI; c.irq &= (uint8_t)~DECI; }
        if (!c.irq && g_mcd.cdc_irq.enable) level5();
        c.irq |= DTEI;
    }
    c.edt = true;
    mcd_sub_wake(1u << 4);                          /* a sub CPU polling the transfer's end goes on */
}

static uint16_t ram_word(uint16_t a) { a &= 0x3FFE; return (uint16_t)(c.ram[a] << 8 | c.ram[a + 1]); }

/* DMA of LEN bytes from the buffer at DAC to the destination at $FF800A */
static void dma_write(int kind, unsigned len) {
    Mcd *m = &g_mcd;
    if (kind == DMA_PCM) {
        uint16_t src = c.dac & 0x3FFF, dst = (uint16_t)((c.dma_address << 2) & 0xFFF);
        c.dma_address += (uint16_t)(len >> 2); c.dac += (uint16_t)len;
        while (len--) { mcd_pcm_write(0x1000 | dst, c.ram[src]); src = (src + 1) & 0x3FFF; dst = (dst + 1) & 0xFFF; }
        return;
    }
    unsigned words = len >> 1;
    uint16_t src = c.dac & 0x3FFE;
    uint32_t dst = (uint32_t)c.dma_address << 3;
    c.dma_address += (uint16_t)(words >> 2); c.dac += (uint16_t)(words << 1);
    while (words--) {
        uint16_t w = ram_word(src);
        if (kind == DMA_PRG) m->pram[(dst & 0x7FFFE) >> 1] = w;
        else if (!m->io.wram_mode) m->wram[(dst & 0x3FFFE) >> 1] = w;
        else m->wram[(dst & 0x1FFFE) | (m->io.wram_select ? 0 : 1)] = w;   /* the sub CPU's 1 Mbit bank */
        src = (src + 2) & 0x3FFE; dst += 2;
    }
}

/* DTRG: the transfer to the destination in $FF8004 starts (host reads load the first word now) */
static void dma_start(void) {
    if (c.ifstat & DTEN) return;
    c.dma = c.halted = DMA_NONE;
    Mcd *m = &g_mcd;
    switch (c.destination & 7) {
    case 2: case 3:
        c.host = ram_word(c.dac); c.dsr = true;
        c.dac += 2; c.dbc -= 2;
        if ((int16_t)c.dbc < 0) transfer_end();
        break;
    case 4: c.dma = DMA_PCM; break;
    case 5: if (m->io.request) c.halted = DMA_PRG; else c.dma = DMA_PRG; break;   /* main CPU holds PRG-RAM */
    case 7:
        if (m->io.wram_mode) c.dma = DMA_WRAM;
        else if (!m->io.wram_switch) c.halted = DMA_WRAM;                       /* main CPU holds 2 Mbit Word RAM */
        else c.dma = DMA_WRAM;
        break;
    }
}

/* once a line (mcd.c): the DMA catches up with the sub CPU's clock */
void mcd_cdc_dma_line(void) {
    Mcd *m = &g_mcd;
    if (c.halted && !c.dma) {                       /* resumes once the main CPU lets go */
        if ((c.halted == DMA_PRG && !m->io.request) || (c.halted == DMA_WRAM && (m->io.wram_mode || m->io.wram_switch))) {
            c.dma = c.halted; c.halted = DMA_NONE;
        }
    }
    if (!c.dma) return;
    long t = now();
    int bytes = (int)((t - c.cycles[0] + DMA_CLOCKS_PER_BYTE - 1) / DMA_CLOCKS_PER_BYTE);
    bytes = bytes / 8 * 8;
    if (c.dbc < bytes) {
        dma_write(c.dma, (unsigned)c.dbc + 1);
        c.cycles[0] += ((long)c.dbc + 1) * DMA_CLOCKS_PER_BYTE;
        transfer_end();
        c.dma = c.halted = DMA_NONE;
    } else if (bytes > 0) {
        dma_write(c.dma, (unsigned)bytes);
        c.dbc -= (uint16_t)bytes;
        c.cycles[0] += (long)bytes * DMA_CLOCKS_PER_BYTE;
    }
}
void mcd_cdc_dma(void) {}                           /* the old 6-clock hook: unused */

/* the host reads a word: $A12008 (CPU 2, main) or $FF8008 (CPU 3, sub) */
uint16_t mcd_cdc_host_cpu(int cpu) {
    uint16_t data = c.host;
    if ((c.destination & 7) == cpu && c.dsr) {
        if (c.edt) c.dsr = false;
        else {
            c.host = ram_word(c.dac);
            c.dac += 2; c.dbc -= 2;
            if ((int16_t)c.dbc < 0) transfer_end();
        }
    }
    return data;
}
uint16_t mcd_cdc_host_read(void) { return mcd_cdc_host_cpu(3); }

/* a sector under the head (mcd_cdd.c, 75 a second); header: minute, second, frame (BCD), mode */
static void decode(const uint8_t header[4], int lba) {
    if (!(c.ctrl[0] & DECEN)) return;
    memcpy(c.head[0], header, 4);
    c.stat[3] = 0;
    c.ifstat &= (uint8_t)~DECI;
    c.cycles[1] = now() + DECODER_PERIOD;
    if (c.ifctrl & DECIEN) {
        if (!(c.irq & DTEI) && g_mcd.cdc_irq.enable) level5();
        c.irq |= DECI;
    }
    if (c.ctrl[0] & WRRQ) {
        c.pt += 2352; c.wa += 2352;
        int off = c.pt & 0x3FFF;
        memcpy(c.ram + off, header, 4); off += 4;
        if (c.head[0][3] == 0x01) {                 /* Mode 1: the 2048 user bytes */
            uint8_t raw[2352];
            if (lba < 0 || !mcd_disc_read(lba, raw)) memset(raw, 0, sizeof raw);
            memcpy(c.ram + off, raw + 16, 2048); off += 2048;
        } else off += (c.ctrl[1] & MODRQ) ? 2336 : 2328;   /* Mode 2 / none: not on SHCD's disc */
        if (off > 0x4000) memcpy(c.ram, c.ram + 0x4000, (size_t)(off - 0x4000));
    }
}
static uint8_t bcd(int v) { return (uint8_t)(v / 10 << 4 | v % 10); }
void mcd_cdc_decode(int lba) {
    int a = lba + 150;
    uint8_t h[4] = { bcd(a / 75 / 60), bcd(a / 75 % 60), bcd(a % 75), 0x01 };
    decode(h, lba);
}
void mcd_cdc_decode_idle(void) { static const uint8_t z[4]; decode(z, -1); }

uint8_t mcd_cdc_reg_read(void) {
    uint8_t d;
    switch (c.address & 0xF) {
    case 0x1: decoder_expire(); d = c.ifstat; break;
    case 0x2: d = (uint8_t)c.dbc; break;
    case 0x3: d = (uint8_t)(c.dbc >> 8); break;
    case 0x4: case 0x5: case 0x6: case 0x7: d = c.head[c.ctrl[1] & SHDREN][(c.address & 0xF) - 4]; break;
    case 0x8: d = (uint8_t)c.pt; break;
    case 0x9: d = (uint8_t)(c.pt >> 8); break;
    case 0xA: d = (uint8_t)c.wa; break;
    case 0xB: d = (uint8_t)(c.wa >> 8); break;
    case 0xC: d = c.stat[0]; break;
    case 0xD: d = 0; break;
    case 0xE: d = c.stat[2]; break;
    case 0xF: d = c.stat[3]; c.stat[3] = VALST; c.ifstat |= DECI; c.irq &= (uint8_t)~DECI; break;
    default: d = 0xFF; break;
    }
    if (c.address) c.address = (c.address + 1) & 0x0F;
    return d;
}

static void cdc_reset(void) {
    c.address = 0;
    c.ifstat = 0xFF; c.ifctrl = 0;
    c.ctrl[0] = c.ctrl[1] = 0;
    memset(c.stat, 0, sizeof c.stat); c.stat[3] = 0x80;
    memset(c.head, 0, sizeof c.head); c.head[0][3] = 0x01;
    c.cycles[0] = c.cycles[1] = 0;
    c.dma = c.halted = DMA_NONE;
    c.irq = 0;
    level5_clear();
}

void mcd_cdc_reg_write(uint8_t v) {
    switch (c.address & 0xF) {
    case 0x1: {                                     /* IFCTRL */
        uint8_t prev = c.irq;
        if (now() > c.cycles[1]) { c.ifstat |= DECI; prev &= (uint8_t)~DECI; }
        c.irq = (uint8_t)(~c.ifstat & v & (DTEIEN | DECIEN));
        if (c.irq && !prev && g_mcd.cdc_irq.enable) level5();
        if (!(v & DOUTEN)) { c.ifstat |= DTBSY | DTEN; c.dma = c.halted = DMA_NONE; }
        c.ifctrl = v;
    } break;
    case 0x2: c.dbc = (uint16_t)((c.dbc & 0xFF00) | v); break;
    case 0x3: c.dbc = (uint16_t)((c.dbc & 0x00FF) | (v & 0x0F) << 8); break;
    case 0x4: c.dac = (uint16_t)((c.dac & 0xFF00) | v); break;
    case 0x5: c.dac = (uint16_t)((c.dac & 0x00FF) | v << 8); break;
    case 0x6:                                       /* DTRG */
        if (c.ifctrl & DOUTEN) {
            c.ifstat &= (uint8_t)~(DTBSY | DTEN);
            c.dsr = c.edt = false;
            dma_start();
            c.cycles[0] = now();
        }
        break;
    case 0x7: c.ifstat |= DTEI; c.irq &= (uint8_t)~DTEI; break;     /* DTACK */
    case 0x8: c.wa = (uint16_t)((c.wa & 0xFF00) | v); break;
    case 0x9: c.wa = (uint16_t)((c.wa & 0x00FF) | v << 8); break;
    case 0xA:                                       /* CTRL0 */
        c.stat[0] = v & DECEN;
        if (!(v & DECEN)) { c.ifstat |= DECI; c.irq &= (uint8_t)~DECI; }
        c.stat[2] = (v & AUTORQ) ? (uint8_t)((c.ctrl[1] & MODRQ) | ((c.head[1][2] & 0x20) >> 3)) : (uint8_t)(c.ctrl[1] & (MODRQ | FORMRQ));
        c.ctrl[0] = v;
        break;
    case 0xB:                                       /* CTRL1 */
        c.stat[2] = (c.ctrl[0] & AUTORQ) ? (uint8_t)((v & MODRQ) | ((c.head[1][2] & 0x20) >> 3)) : (uint8_t)(v & (MODRQ | FORMRQ));
        c.ctrl[1] = v;
        break;
    case 0xC: c.pt = (uint16_t)((c.pt & 0xFF00) | v); break;
    case 0xD: c.pt = (uint16_t)((c.pt & 0x00FF) | v << 8); break;
    case 0xF: cdc_reset(); break;
    }
    if (c.address) c.address = (c.address + 1) & 0x0F;
}

/* the gate array's side: $FF8004 (and $A12004's top byte), $FF800A, $FF800C */
uint16_t mcd_cdc_status(void) {
    return (uint16_t)((c.address & 0x1F) | (c.destination & 7) << 8 | c.dsr << 14 | c.edt << 15);
}
void mcd_cdc_ctrl_write(bool upper, bool lower, uint16_t v) {
    if (lower) c.address = v & 0x0F;
    if (upper) { c.destination = v >> 8 & 7; c.dsr = c.edt = false; }   /* a new destination clears DSR and EDT */
}
uint16_t mcd_cdc_dma_address(void) { return c.dma_address; }
void mcd_cdc_dma_address_write(uint16_t v) { c.dma_address = v; }
uint16_t mcd_cdc_stopwatch(void) { return c.stopwatch & 0xFFF; }
void mcd_cdc_stopwatch_reset(void) { c.stopwatch = 0; }
void mcd_cdc_clock(void) { c.stopwatch = (c.stopwatch + 1) & 0xFFF; }
void mcd_cdc_power(void) { memset(&c, 0, sizeof c); cdc_reset(); c.host = 0xFFFF; c.dma_address = 0xFFFF; }

/* ps_state.c: the CDC (registers, buffer, DMA and decoder positions) */
void ps_state_blob(void *p, unsigned long n);
#define B(v) ps_state_blob(&(v), sizeof (v))
void mcd_cdc_state(void) { B(c); }
#undef B
