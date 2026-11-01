/*
 * Mega CD on the Genesis Plus GX backend (reference player, tracer): BIOS paths and backup RAM.
 * BIOS: PS_SCD_BIOS=DIR (default Application Support/PixelSorcery/bios), the Model 1 BIOS of each
 * region dumped from your own console, as scd_us.bin, scd_eu.bin, scd_jp.bin (GPGX picks one from
 * the disc; GENRECOMP_REGION forces one). Backup RAM - the internal
 * 8 KB and the 512 KB RAM cartridge GPGX plugs in when booting from CD - is formatted with Sega's
 * standard directory header when new (games otherwise stop at "RAM cartridge is not initialized")
 * and kept in Application Support/PixelSorcery/megacd/{scd,cart}.brm by players (not by tools).
 */
#include "shared.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* PS_SCD_CENSUS=file: what the sub CPU does with the Mega CD hardware (BACKLOG priority 0, step 1).
 * Counts every write to the PCM / gate-array area by register; records the Word RAM mode values,
 * the CDD commands (first nibble of $FF8042 when the command is sent by its last byte $FF804B) and
 * the graphics ASIC use; written at exit. */
static unsigned long s_cnt[2][0x200];               /* [word?][register >> 1 & 0x1FF] */
static unsigned long s_pcm, s_cdd[16], s_mode[256], s_asic;
static unsigned s_cdd_first;
static void census_hook(int width, unsigned int a, unsigned int d) {
    if (!(a & 0x8000)) { s_pcm++; return; }
    unsigned r = a & 0x1FF;
    s_cnt[width == 2][r >> 1]++;
    if (r == 0x03 || (r == 0x02 && width == 2)) s_mode[d & 0xFF]++;
    if (r == 0x42) s_cdd_first = width == 2 ? (d >> 8) & 0xF : d & 0xF;
    if (r == 0x4A || r == 0x4B) s_cdd[s_cdd_first & 15]++;
    if (r >= 0x58 && r <= 0x67) s_asic++;
}
static void census_write(void) {
    const char *f = getenv("PS_SCD_CENSUS"); FILE *o = f ? fopen(f, "w") : NULL;
    if (!o) return;
    fprintf(o, "pcm_writes %lu\nasic_writes %lu\n", s_pcm, s_asic);
    for (int w = 0; w < 2; w++)
        for (int i = 0; i < 0x100; i++)
            if (s_cnt[w][i]) fprintf(o, "reg $FF80%02X %s %lu\n", i * 2, w ? "word" : "byte", s_cnt[w][i]);
    for (int i = 0; i < 256; i++) if (s_mode[i]) fprintf(o, "mode_value $%02X %lu\n", i, s_mode[i]);
    for (int i = 0; i < 16; i++) if (s_cdd[i]) fprintf(o, "cdd_command %X %lu\n", i, s_cdd[i]);
    fclose(o);
}

void ps_megacd_bios(void) {
    if (getenv("PS_SCD_CENSUS") && !ps_scd_write_hook) { ps_scd_write_hook = census_hook; atexit(census_write); }
    const char *dir = getenv("PS_SCD_BIOS"), *home = getenv("HOME");
    char def[512]; snprintf(def, sizeof def, "%s/Library/Application Support/PixelSorcery/bios", home ? home : ".");
    if (!dir || !*dir) dir = def;
    /* the core's path buffers are 256 bytes: a longer folder is refused, not cut to a wrong path */
    if (snprintf(CD_BIOS_US, sizeof CD_BIOS_US, "%s/scd_us.bin", dir) >= (int)sizeof CD_BIOS_US ||
        snprintf(CD_BIOS_EU, sizeof CD_BIOS_EU, "%s/scd_eu.bin", dir) >= (int)sizeof CD_BIOS_EU ||
        snprintf(CD_BIOS_JP, sizeof CD_BIOS_JP, "%s/scd_jp.bin", dir) >= (int)sizeof CD_BIOS_JP) {
        fprintf(stderr, "megacd: BIOS folder path too long (%zu characters): %s\n", strlen(dir), dir);
        CD_BIOS_US[0] = CD_BIOS_EU[0] = CD_BIOS_JP[0] = 0;
    }
}

static void brm_path(const char *name, char *out, size_t n) {
    const char *home = getenv("HOME");
    snprintf(out, n, "%s/Library/Application Support/PixelSorcery/megacd/%s", home ? home : ".", name);
}
static void brm_format(uint8 *ram, uint32 size) {
    static const char tail[0x20] = "SEGA_CD_ROM\0\1\0\0\0RAM_CARTRIDGE___";
    if (!memcmp(ram + size - 0x20, tail, 0x20)) return;               /* already formatted */
    memset(ram, 0, size);
    uint8 *h = ram + size - 0x40;
    memset(h, '_', 11); h[0x0F] = 0x40;                               /* volume name, directory */
    unsigned blocks = size / 64 - 3;                                  /* free blocks, 4 copies */
    for (int i = 0; i < 4; i++) { h[0x10 + 2 * i] = (uint8)(blocks >> 8); h[0x11 + 2 * i] = (uint8)blocks; }
    memcpy(h + 0x20, tail, 0x20);
}
/* after system_reset(): load (files = true) or just format; at exit: save */
void ps_megacd_brm(bool load_files, bool save) {
    if (system_hw != SYSTEM_MCD) return;
    struct { const char *name; uint8 *ram; uint32 size; } b[2] = {
        { "scd.brm", scd.bram, sizeof scd.bram },
        { "cart.brm", scd.cartridge.area, scd.cartridge.id ? scd.cartridge.mask + 1 : 0 } };
    char dir[600]; brm_path("", dir, sizeof dir);
    if (save) for (char *q = dir + 1; *q; q++) if (*q == '/') { *q = 0; mkdir(dir, 0755); *q = '/'; }
    for (int i = 0; i < 2; i++) {
        if (!b[i].size) continue;
        char p[700]; brm_path(b[i].name, p, sizeof p);
        if (save) { FILE *f = fopen(p, "wb"); if (f) { fwrite(b[i].ram, 1, b[i].size, f); fclose(f); } continue; }
        if (load_files) {
            FILE *f = fopen(p, "rb");
            if (f) { if (fread(b[i].ram, 1, b[i].size, f) != b[i].size) memset(b[i].ram, 0, b[i].size); fclose(f); }
        }
        brm_format(b[i].ram, b[i].size);
    }
}

/* The Mega CD beside the recompiled main CPU (bus.c, GPGX backend): scd_update() without its
 * m68k_run - the main CPU has already run this line - so the sub CPU, CDD (75 Hz), the timer
 * (level 3), CDC DMA and the graphics chip run one line after it, as GPGX does per line.
 * m68k.cycle_end stays at the line end: GPGX's sync helpers then never start its own main
 * CPU, and a main CPU found polling a register idles to the line end (bus_sync_back). */
void ps_megacd_line(unsigned int line_end) {
    if (system_hw != SYSTEM_MCD) return;
    unsigned int mc = m68k.cycles;               /* the sub CPU's sync helpers may move it */
    int end = scd.cycles + SCYCLES_PER_LINE;
    while (scd.cycles < end) {
        int run = end - scd.cycles;
        if (scd.timer > 0 && scd.timer < run) run = scd.timer;
        s68k_run(scd.cycles + run);
        scd.cycles += run;
        cdd.cycles += run * 3;
        if (cdd.cycles >= 500000 * 4) {
            cdd.cycles -= 500000 * 4;
            cdd_update();
            if (scd.regs[0x37 >> 1].byte.l & 0x04) {
                scd.pending |= 1 << 4;
                if (scd.regs[0x32 >> 1].byte.l & 0x10) s68k_update_irq((scd.pending & scd.regs[0x32 >> 1].byte.l) >> 1);
            }
        }
        if (scd.timer) {
            scd.timer -= run;
            if (scd.timer <= 0) {
                scd.timer += scd.regs[0x30 >> 1].byte.l * TIMERS_SCYCLES_RATIO;
                if (scd.regs[0x32 >> 1].byte.l & 0x08) {
                    scd.pending |= 1 << 3;
                    s68k_update_irq((scd.pending & scd.regs[0x32 >> 1].byte.l) >> 1);
                }
            }
        }
    }
    if (cdc.dma_w) cdc_dma_update(scd.cycles);
    if (scd.regs[0x58 >> 1].byte.h & 0x80) gfx_update(scd.cycles);
    m68k.cycles = mc;
    m68k.cycle_end = line_end + MCYCLES_PER_LINE;
    m68k.stopped = 0;
}
/* GPGX found the main CPU polling a sub CPU register (m68k.stopped, m68k.cycles moved to the
 * line end): as in scd_update, the sub CPU runs on to the line end, and a write of the polled
 * register restarts the main CPU at that moment (s68k_poll_sync sets m68k.cycles), else the
 * main CPU stays idle to the line end. */
void ps_megacd_poll_wait(void) {
    unsigned int end = m68k.cycle_end;
    s68k_run((unsigned int)(((unsigned long long)end * SCYCLES_PER_LINE) / MCYCLES_PER_LINE));
    if (m68k.stopped) { m68k.cycles = end; m68k.stopped = 0; }
}
/* frame boundary (bus.c rebases the main CPU clock by the same amount) */
void ps_megacd_end_frame(unsigned int line_end) {
    if (system_hw != SYSTEM_MCD) return;
    scd_end_frame(scd.cycles);
    scd.cycles = 0;
    m68k.cycle_end = line_end;
}

/* A main-CPU DMA from Word RAM ($200000-$23FFFF, $600000 when booted from a cartridge) */
int ps_megacd_dma_delayed(unsigned reg23) {
    return system_hw == SYSTEM_MCD && (reg23 & 0x70) == ((scd.cartridge.boot >> 1) + 0x10);
}

/* the overlay key (func_table.h): main-CPU writes to the memory mode register (bus.c). The main
 * CPU hands Word RAM to the sub CPU (DMNA) before a program is loaded there, and cannot run Word
 * RAM code until it is handed back, so a new program always comes with a new key */
extern uint32_t (*func_table_overlay_key)(uint32_t addr);
extern uint32_t g_ps_mcd_mode_writes, g_ps_wram_code_writes;
static uint32_t ps_megacd_overlay_key(uint32_t addr) {
    return addr >= 0xFF0000 ? g_ps_wram_code_writes            /* work RAM: a program copied over it */
         : addr >= 0x200000 ? g_ps_mcd_mode_writes : 0;       /* Word RAM; the BIOS never changes */
}
void ps_megacd_overlays(void) { if (system_hw == SYSTEM_MCD) func_table_overlay_key = ps_megacd_overlay_key; }
