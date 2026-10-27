/* ps_hw.c — PixelSorcery's Mega Drive hardware layer (native build, no emulator core).
 *
 * Cartridge ROM and work RAM, the 68000 memory map outside the VDP ports, the Z80 side
 * (RAM, YM2612 and PSG through ps_synth, bank window into 68000 space), Z80 bus request /
 * reset arbitration, and the I/O chip with 3-button pads. Timing follows the hardware as
 * the oracle models it (bus latencies, Z80 resynchronisation, 68000 wait states while the
 * Z80 reads the 68000 bus), so recompiled runs stay identical poll by poll.
 */
#ifdef PS_NATIVE
#include "ps_hw.h"
#include "ps_synth.h"
#include "genrecomp/m68k.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint8_t  work_ram[0x10000];
uint8_t  zram[0x2000];
uint16_t v_counter;
uint16_t lines_per_frame = 262;
uint32_t mcycles_vdp;
uint16_t status;
uint8_t  zstate;
ps_z80_clock_t Z80;
ps_input_t input;
void (*io_pad_write_hook)(int port, unsigned int data, unsigned int ctrl);
unsigned int g_dma68k_end;

static uint8_t *s_rom;               /* host-order words, like work RAM */
static void sram_detect(void);
#define SERIAL_SK "GM MK-1563"                /* SHK' header serial ($180) */
static uint32_t s_rom_size, s_rom_mask;
static uint32_t s_zbank;             /* Z80 window base in 68000 space */
static uint8_t  s_io[16];            /* I/O chip registers */
static struct { uint8_t state; unsigned latency; } s_pad[2];

void ps_audio_z80_reset(void);
void ps_audio_z80_run(unsigned int target);

/* ------------------------------------------------------------------ cartridge */
bool ps_hw_load_rom(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n <= 0x200 || n > 0x400000) { fclose(f); return false; }
    uint8_t *raw = malloc((size_t)n);
    if (fread(raw, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(raw); return false; }
    fclose(f);
    /* .smd: 512-byte header then 16 KB blocks with odd/even bytes split */
    if ((n & 0x3FFF) == 0x200 && raw[8] == 0xAA && raw[9] == 0xBB) {
        long m = n - 0x200; uint8_t *d = malloc((size_t)m);
        for (long b = 0; b < m; b += 0x4000)
            for (int i = 0; i < 0x2000; i++) {
                d[b + i * 2 + 1] = raw[0x200 + b + i];
                d[b + i * 2]     = raw[0x200 + b + 0x2000 + i];
            }
        free(raw); raw = d; n = m;
    }
    /* As GPGX's md_cart_init(): padded with $FF up to 2^k bytes and mirrored every 2^k; SHK alone is not mirrored at $200000-$3FFFFF (the empty lock-on slot reads $FF: with
     * the mirror it read its own header at $200100 and booted into SHBS). GPGX matches
     * the header name; the serial is used here (every SHK dump, locked on or not, has it). */
    uint32_t size = 0x10000; while (size < (uint32_t)n) size <<= 1;
    if (n > 0x190 && !memcmp(raw + 0x180, SERIAL_SK, 10)) size = 0x400000;
    s_rom = malloc(size);
    memset(s_rom, 0xFF, size);
    for (long i = 0; i + 1 < n; i += 2) { s_rom[i] = raw[i + 1]; s_rom[i + 1] = raw[i]; }  /* host-order words */
    free(raw);
    s_rom_size = (uint32_t)n; s_rom_mask = size - 1;
    sram_detect();
    char name[49] = {0};
    for (int i = 0; i < 48; i++) name[i] = (char)READ_BYTE(s_rom, 0x150 + i);
    printf("ps_hw: ROM \"%s\", %u KB\n", name, s_rom_size >> 10);
    return true;
}

/* Battery-backed save RAM (header "RA" at $1B0): 64 KB, erased to $FF, byte-addressed by
 * address & $FFFF. Mapped on its 64 KB bank when it lies past the ROM; SH3 (ROM mirrored
 * there at power-on) and ROMs it overlaps map it at $200000 through the $A130F1 register
 * (bit 0 on, bit 1 write-protect). */
static uint8_t s_sram[0x10000];
static int s_sram_bank = -1, s_sram_wp;     /* mapped bank ($20 = $200000), -1 none */
static uint32_t s_sram_start;
static bool s_sram_on, s_sram_default;    /* default: GPGX's undeclared 64 KB (not a save) */
static void sram_detect(void) {
    memset(s_sram, 0xFF, sizeof s_sram);
    s_sram_bank = -1; s_sram_wp = 0; s_sram_on = s_sram_default = false;
    char serial[11] = {0};
    for (int i = 0; i < 10; i++) serial[i] = (char)READ_BYTE(s_rom, 0x180 + i);
    if (!strcmp(serial, SERIAL_SK) && s_rom_size == 0x400000) {
        /* SH3K (locked on): no header of its own; SH3's FRAM, switched in at
         * $200000 over the SH3 half through $A130F1 */
        s_sram_start = 0x200001; s_sram_on = true; return;
    }
    if (READ_BYTE(s_rom, 0x1B0) != 'R' || READ_BYTE(s_rom, 0x1B1) != 'A') {
        /* GPGX's sram_init(): ROMs up to 2 MB without a header get 64 KB at $200000 anyway
         * (SHK alone reads $FF there, no lock-on) */
        if (s_rom_size <= 0x200000) { s_sram_start = 0x200000; s_sram_on = s_sram_default = true; s_sram_bank = 0x20; }
        return;
    }
    s_sram_start = (uint32_t)(READ_BYTE(s_rom, 0x1B4) << 24 | READ_BYTE(s_rom, 0x1B5) << 16 |
                              READ_BYTE(s_rom, 0x1B6) << 8 | READ_BYTE(s_rom, 0x1B7)) & 0xFFFFFF;
    if (s_sram_start == 0xFF0000) return;          /* work RAM declared as save RAM: none */
    if (s_sram_start >= 0x800000) s_sram_start = 0x200000;
    s_sram_on = true;
    /* GPGX leaves out a ROM named "SH THE HEDGEHOG 3" without padding; SH3's header has
     * padding spaces in the name, so no dump is left out and neither is any here */
    if (s_sram_start >= s_rom_size) s_sram_bank = (int)(s_sram_start >> 16);
}
static void sram_control(uint8_t v) {                /* $A130F1 */
    if (!s_sram_on) return;
    if (v & 1) { s_sram_bank = 0x20; s_sram_wp = v & 2; }
    else if (s_sram_bank == 0x20) s_sram_bank = -1;
}
uint8_t *ps_hw_sram(bool *present) { if (present) *present = s_sram_on && !s_sram_default; return s_sram; }   /* ps_menu.c: save files */

uint16_t ps_hw_rom_patch16(uint32_t addr, uint16_t value) {      /* returns the old word */
    addr &= s_rom_mask & ~1u;
    uint16_t old = (uint16_t)(READ_BYTE(s_rom, addr) << 8 | READ_BYTE(s_rom, addr + 1));
    s_rom[addr ^ 1] = (uint8_t)(value >> 8); s_rom[(addr + 1) ^ 1] = (uint8_t)value;
    return old;
}
const uint8_t *ps_hw_rom(uint32_t *size) { if (size) *size = s_rom_size; return s_rom; }

/* ------------------------------------------------------------------ open bus */
/* Unmapped reads return the word the 68000 prefetched (its next instruction word). */
static uint16_t prefetch16(void) {
    uint32_t pc = g_m68k_pf ? g_m68k_pf : g_m68k.pc;
    pc &= 0xFFFFFE;
    if (pc >= 0xE00000) return *(uint16_t *)(work_ram + (pc & 0xFFFF));
    if (pc < 0x400000) return *(uint16_t *)(s_rom + (pc & s_rom_mask));
    return 0xFFFF;
}
static uint8_t prefetch8(uint32_t addr) {
    uint16_t w = prefetch16();
    return (addr & 1) ? (uint8_t)w : (uint8_t)(w >> 8);
}

/* ------------------------------------------------------------------ Z80 control */
void z80_run(unsigned int cycles) { ps_audio_z80_run(cycles); }

static void z80_reset(unsigned int cycles) {
    Z80.cycles = ((cycles + 14) / 15) * 15;   /* Z80 clock stays a multiple of 15 */
    ps_audio_z80_reset();
}

static void busreq_w(unsigned data, unsigned cycles) {
    if (data) {                                   /* /BUSREQ asserted */
        if (zstate == 1) z80_run(cycles);         /* bring the Z80 up to date first */
        zstate |= 2;
    } else {
        if (zstate == 3) Z80.cycles = ((cycles + 14) / 15) * 15;
        zstate &= 1;
    }
}

static void reset_w(unsigned data, unsigned cycles) {
    if (data) {                                   /* /RESET released */
        if (zstate == 0 || zstate == 2) {
            z80_reset(cycles);
            ps_synth_reset(cycles);
        }
        zstate |= 1;
    } else {                                      /* /RESET asserted: Z80 and YM2612 stop */
        if (zstate == 1) z80_run(cycles);
        ps_synth_reset(cycles);
        zstate &= 2;
    }
}

/* ------------------------------------------------------------------ I/O chip, pads */
/* 3-button pad: TH high reads ?1CBRLDU, TH low ?0SA00DU. When TH is switched from output
 * to input, its pulled-up level reaches the pad 172 master clocks later. */
static uint8_t pad_read(int port) {
    uint8_t data = s_pad[port].state | 0x3F;
    unsigned pad = input.pad[port];
    unsigned th = data >> 6;
    if ((int)(g_cyc - s_pad[port].latency) < 0) th = 0;
    if (th) data &= (uint8_t)~(pad & 0x3F);
    else    data &= (uint8_t)~((pad & 0x03) | ((pad >> 2) & 0x30) | 0x0C);
    return data;
}
static void pad_write(int port, uint8_t data, uint8_t mask) {
    if (mask & 0x40) { data &= 0x40; s_pad[port].latency = 0; }
    else {
        data = 0x40;
        if (!s_pad[port].state) s_pad[port].latency = g_cyc + 172;
    }
    s_pad[port].state = data;
}

static uint8_t io_read(unsigned off) {
    if (off >= 1 && off <= 3) {
        unsigned mask = 0x80 | s_io[off + 3];
        uint8_t d = off == 3 ? 0x7F : pad_read((int)off - 1);
        return (uint8_t)((s_io[off] & mask) | (d & ~mask));
    }
    return s_io[off];
}
static void io_write(unsigned off, uint8_t data) {
    switch (off) {
    case 1: case 2: case 3:
        if (io_pad_write_hook) io_pad_write_hook((int)off - 1, data, s_io[off + 3]);
        s_io[off] = data;
        if (off < 3) pad_write((int)off - 1, data, s_io[off + 3]);
        return;
    case 4: case 5: case 6:
        if (data != s_io[off]) {
            s_io[off] = data;
            if (off < 6) pad_write((int)off - 4, s_io[off - 3], data);
        }
        return;
    case 7: case 0x0A: case 0x0D: s_io[off] = data; return;
    case 9: case 0x0C: case 0x0F: s_io[off] = data & 0xF8; return;
    default: return;
    }
}

void ps_hw_end_frame(unsigned int cycles) {
    for (int p = 0; p < 2; p++)
        s_pad[p].latency = (int)(s_pad[p].latency - cycles) > 0 ? s_pad[p].latency - cycles : 0;
}

void ps_hw_reset(void) {
    memset(work_ram, 0, sizeof work_ram);
    memset(zram, 0, sizeof zram);
    memset(s_io, 0, sizeof s_io);
    s_io[0] = 0x80 | 0x20;                        /* overseas, NTSC, no expansion unit */
    s_io[7] = 0xFF; s_io[0x0A] = 0xFF; s_io[0x0D] = 0xFB;
    for (int p = 0; p < 2; p++) { s_pad[p].state = 0x40; s_pad[p].latency = 0; }
    zstate = 0; s_zbank = 0;
    Z80.irq_state = CLEAR_LINE;
    ps_synth_reset(0);
}

/* ------------------------------------------------------------------ 68000 side */
static uint8_t z80_area_read(uint32_t a) {
    g_cyc += 7;                                    /* Z80 bus access latency */
    switch ((a >> 13) & 3) {
    case 2: return (uint8_t)ps_synth_fm_read(g_cyc, a & 3);
    case 3: return 0xFF;
    default: return zram[a & 0x1FFF];
    }
}
static void z80_area_write(uint32_t a, uint8_t v) {
    g_cyc += 7;
    switch ((a >> 13) & 3) {
    case 2: ps_synth_fm_write(g_cyc, a & 3, v); return;
    case 3: if (((a >> 8) & 0x7F) == 0x60) s_zbank = ((s_zbank >> 1) | ((uint32_t)(v & 1) << 23)) & 0xFF8000; return;
    default: zram[a & 0x1FFF] = v; return;
    }
}

uint8_t ps_hw_read8(uint32_t a) {
    a &= 0xFFFFFF;
    if ((int)(a >> 16) == s_sram_bank) return s_sram[a & 0xFFFF];
    if (a < 0x400000) return READ_BYTE(s_rom, a & s_rom_mask);
    if (a >= 0xE00000) return READ_BYTE(work_ram, a & 0xFFFF);
    if ((a >> 16) == 0xA0) return zstate == 3 ? z80_area_read(a) : prefetch8(a);
    if ((a >> 16) == 0xA1) {
        switch ((a >> 8) & 0xFF) {
        case 0x00: return (a & 0xE0) ? prefetch8(a) : io_read((a >> 1) & 0x0F);
        case 0x11:
            if (a & 1) return prefetch8(a);
            return zstate == 3 ? (prefetch8(a & ~1u) & 0xFE) : (prefetch8(a & ~1u) | 0x01);
        default: return prefetch8(a);
        }
    }
    return prefetch8(a);
}

uint16_t ps_hw_read16(uint32_t a) {
    a &= 0xFFFFFE;
    if ((int)(a >> 16) == s_sram_bank) return (uint16_t)(s_sram[a & 0xFFFF] << 8 | s_sram[(a & 0xFFFF) + 1]);
    if (a < 0x400000) return *(uint16_t *)(s_rom + (a & s_rom_mask));
    if (a >= 0xE00000) return *(uint16_t *)(work_ram + (a & 0xFFFF));
    if ((a >> 16) == 0xA0) {
        if (zstate != 3) return prefetch16();
        uint8_t b = z80_area_read(a);
        return (uint16_t)(b | (b << 8));
    }
    if ((a >> 16) == 0xA1) {
        switch ((a >> 8) & 0xFF) {
        case 0x00: if (!(a & 0xE0)) { uint8_t d = io_read((a >> 1) & 0x0F); return (uint16_t)(d << 8 | d); } break;
        case 0x11: return zstate == 3 ? (prefetch16() & 0xFEFF) : (prefetch16() | 0x0100);
        }
    }
    return prefetch16();
}

void ps_hw_write8(uint32_t a, uint8_t v) {
    a &= 0xFFFFFF;
    if (a >= 0xE00000) { WRITE_BYTE(work_ram, a & 0xFFFF, v); return; }
    if ((int)(a >> 16) == s_sram_bank) { if (!s_sram_wp) s_sram[a & 0xFFFF] = v; return; }
    if (a == 0xA130F1) { sram_control(v); return; }
    if ((a >> 16) == 0xA0) { if (zstate == 3) z80_area_write(a, v); return; }
    if ((a >> 16) == 0xA1) {
        switch ((a >> 8) & 0xFF) {
        case 0x00: if ((a & 0xE1) == 0x01) io_write((a >> 1) & 0x0F, v); return;
        case 0x11: if (!(a & 1)) busreq_w(v & 1, g_cyc); return;
        case 0x12: if (!(a & 1)) reset_w(v & 1, g_cyc); return;
        default: return;
        }
    }
    if (a >= 0xC00000 && a < 0xE00000 && (a & 0x1F) >= 0x10 && (a & 0x1F) < 0x18 && (a & 1))
        ps_synth_psg_write(g_cyc, v);             /* PSG at $C00011 */
}

void ps_hw_write16(uint32_t a, uint16_t v) {
    a &= 0xFFFFFE;
    if (a >= 0xE00000) { *(uint16_t *)(work_ram + (a & 0xFFFF)) = v; return; }
    if ((int)(a >> 16) == s_sram_bank) { if (!s_sram_wp) { s_sram[a & 0xFFFF] = (uint8_t)(v >> 8); s_sram[(a & 0xFFFF) + 1] = (uint8_t)v; } return; }
    if (a == 0xA130F0) { sram_control((uint8_t)v); return; }
    if ((a >> 16) == 0xA0) { if (zstate == 3) z80_area_write(a, (uint8_t)(v >> 8)); return; }
    if ((a >> 16) == 0xA1) {
        switch ((a >> 8) & 0xFF) {
        case 0x00: if (!(a & 0xE0)) io_write((a >> 1) & 0x0F, (uint8_t)v); return;
        case 0x11: busreq_w((v >> 8) & 1, g_cyc); return;
        case 0x12: reset_w((v >> 8) & 1, g_cyc); return;
        default: return;
        }
    }
    if (a >= 0xC00000 && a < 0xE00000 && (a & 0x1F) >= 0x10 && (a & 0x1F) < 0x18)
        ps_synth_psg_write(g_cyc, v & 0xFF);
}

/* ------------------------------------------------------------------ Z80 side */
/* The Z80 reading or writing the 68000 bus: it waits out a 68K-bus DMA, the 68000 loses
 * the bus for ~10 cycles and the Z80 takes 3 wait states. */
static void z80_bus_access(void) {
    if ((int)(Z80.cycles - g_dma68k_end) < 0) Z80.cycles = g_dma68k_end;
    g_cyc += (((Z80.cycles % 7) + 72) / 7) * 7;
    Z80.cycles += 3 * 15;
}

uint8_t ps_hw_z80_read(uint16_t a) {
    switch ((a >> 13) & 7) {
    case 0: case 1: return zram[a & 0x1FFF];
    case 2: return (uint8_t)ps_synth_fm_read(Z80.cycles, a & 3);
    case 3: if ((a >> 8) == 0x7F) { z80_bus_access(); return 0xFF; } return 0xFF;
    default: {
        z80_bus_access();
        uint32_t addr = s_zbank | (a & 0x7FFF);
        if (addr < 0x400000) return READ_BYTE(s_rom, addr & s_rom_mask);
        return 0xFF;
    }
    }
}

void ps_hw_z80_write(uint16_t a, uint8_t v) {
    switch ((a >> 13) & 7) {
    case 0: case 1: zram[a & 0x1FFF] = v; return;
    case 2: ps_synth_fm_write(Z80.cycles, a & 3, v); return;
    case 3:
        if ((a >> 8) == 0x60) { s_zbank = ((s_zbank >> 1) | ((uint32_t)(v & 1) << 23)) & 0xFF8000; return; }
        if ((a >> 8) == 0x7F) {
            z80_bus_access();
            if ((a & 0xF9) == 0x11) ps_synth_psg_write(Z80.cycles, v);   /* PSG through the VDP window */
        }
        return;
    default: {
        z80_bus_access();
        uint32_t addr = s_zbank | (a & 0x7FFF);
        if (addr >= 0xE00000) WRITE_BYTE(work_ram, addr & 0xFFFF, v);
        return;
    }
    }
}

/* ps_state.c: memories, I/O chip, Z80 bus, save RAM (the ROM is checked by hash) */
void ps_state_blob(void *p, unsigned long n);
uint32_t ps_hw_rom_hash(void) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < s_rom_size; i++) { h ^= s_rom[i]; h *= 16777619u; }
    return h ^ s_rom_size;
}
void ps_hw_state(void) {
#define B(v) ps_state_blob(&(v), sizeof (v))
    B(work_ram); B(zram); B(v_counter); B(lines_per_frame); B(mcycles_vdp); B(status); B(zstate);
    B(Z80); B(input); B(g_dma68k_end); B(s_zbank); B(s_io); B(s_pad);
    B(s_sram); B(s_sram_bank); B(s_sram_wp);
#undef B
}
#endif
