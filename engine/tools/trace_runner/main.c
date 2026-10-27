/*
 * genrecomp trace runner: plays a ROM on Genesis Plus GX's 68K interpreter
 * (like genrecomp_ref) and records every code entry point it executes, for
 * bootstrapping a title's recomp.json seeds in one pass instead of one
 * dispatch miss per run:
 *   - targets of executed JSR / BSR / JMP (incl. indirect and jump tables)
 *   - interrupt / exception handler entries
 *   - PCs reached by RTS that were not the instruction after a call
 *     (return-address tricks), so they get entry points too
 * Built only with -DGENRECOMP_TRACE=ON (needs GPGX's HOOK_CPU).
 *
 *   genrecomp_trace --headless --frames 6000 --press 600:START \
 *       --trace-out seeds.txt rom.gen
 */

#include <genrecomp/genrecomp.h>
#include <genrecomp/platform.h>
#include <genrecomp/input.h>
#include <genrecomp/bus.h>
#include <stdio.h>
#include <stdlib.h>
#include "../../src/ps_png.h"
#include <stdlib.h>
#include <string.h>

#include "shared.h"
#include "debug/cpuhook.h"

static uint8_t  s_seen[0x400000 / 2];   /* one byte per even ROM address */
static uint32_t s_prev_pc = 0xFFFFFFFF;
static uint16_t s_prev_op = 0;
static uint32_t s_rom_size;
static uint32_t s_vectors[64];

static uint16_t rom16(uint32_t a) {
    const uint8_t *rom = cart.rom;     /* GPGX keeps ROM LSB_FIRST-swapped */
    return (uint16_t)(rom[a ^ 1] << 8 | rom[(a + 1) ^ 1]);
}

static void mark(uint32_t pc) {
    if (pc < 0x200 || pc >= s_rom_size || (pc & 1)) return;
    s_seen[pc >> 1] = 1;
}

unsigned g_gate_pc; long g_gate_first = -1000, g_gate_last = -1000;

/* PS_ROMUSE=DIR, PS_ROMUSE_KEY=ADDR (a work-RAM word, e.g. SH1's zone/act $FFFE10): which ROM
 * bytes each value of the key uses - executed instruction addresses, 68K reads, and 68K-bus DMA
 * sources (VDP registers $13-$17 + the DMA command, written through $C00004), and the Z80's reads
 * through its bank window (music, samples). For splitting a
 * cartridge game into memory-sized packs (a Mega CD build loads one pack at a time). Written as
 * DIR/<key>.bin: exec bitmap then read bitmap, one bit per ROM byte. */
#define RU_KEYS 128
static uint8_t *s_ru_exec[RU_KEYS], *s_ru_read[RU_KEYS]; static uint16_t s_ru_key[RU_KEYS];
static int s_ru_n, s_ru_on = -1; static unsigned s_ru_keyaddr; static const char *s_ru_dir;
static uint8_t s_ru_reg[32]; static int s_ru_pend; static uint16_t s_ru_first; static int s_ru_cpuonly;
static void ru_save(void) {
    for (int k = 0; k < s_ru_n; k++) {
        char path[1024]; snprintf(path, sizeof path, "%s/%04X.bin", s_ru_dir, s_ru_key[k]);
        FILE *f = fopen(path, "wb"); if (!f) continue;
        fwrite(s_ru_exec[k], 1, s_rom_size / 8 + 1, f); fwrite(s_ru_read[k], 1, s_rom_size / 8 + 1, f); fclose(f);
    }
}
static int ru_slot(void) {
    extern unsigned char work_ram[];
    uint16_t key = *(uint16_t *)(work_ram + (s_ru_keyaddr & 0xFFFE));
    for (int k = 0; k < s_ru_n; k++) if (s_ru_key[k] == key) return k;
    if (s_ru_n == RU_KEYS) return -1;
    s_ru_key[s_ru_n] = key;
    s_ru_exec[s_ru_n] = calloc(1, s_rom_size / 8 + 1); s_ru_read[s_ru_n] = calloc(1, s_rom_size / 8 + 1);
    return s_ru_n++;
}
static void ru_mark(uint8_t *bm, uint32_t a, uint32_t n) {
    for (uint32_t i = 0; i < n; i++, a++) if (a < s_rom_size) bm[a >> 3] |= (uint8_t)(1u << (a & 7));
}
static void ru_vdp_word(uint16_t v) {                  /* a word written to the VDP control port */
    if (s_ru_pend) {                                   /* second half of a command */
        s_ru_pend = 0;
        if ((v & 0x80) && (s_ru_reg[1] & 0x10) && !(s_ru_reg[23] & 0x80)) {   /* 68K-bus DMA */
            uint32_t len = (uint32_t)(s_ru_reg[19] | s_ru_reg[20] << 8); if (!len) len = 0x10000;
            uint32_t src = ((uint32_t)s_ru_reg[21] | s_ru_reg[22] << 8 | (s_ru_reg[23] & 0x7F) << 16) << 1;
            int k = ru_slot(); if (k >= 0 && !s_ru_cpuonly) ru_mark(s_ru_read[k], src, len * 2);
        }
        return;
    }
    if ((v & 0xC000) == 0x8000) { s_ru_reg[(v >> 8) & 0x1F] = (uint8_t)v; return; }
    s_ru_first = v; s_ru_pend = 1;
}
static void ru_hook(hook_type_t type, int width, unsigned int address, unsigned int value) {
    if (type == HOOK_M68K_E) {   /* PS_REGLOG=PC[,PC...]: registers each time execution reaches PC */
        static int rn = -1; static unsigned rpc[16];
        if (rn < 0) { rn = 0; for (const char *q = getenv("PS_REGLOG"); q && *q && rn < 16; ) {
            rpc[rn++] = (unsigned)strtoul(q, NULL, 16); q = strchr(q, ','); if (q) q++; } }
        for (int i = 0; i < rn; i++) if ((address & 0xFFFFFF) == rpc[i]) {
            extern long ps_frames; static int cnt; static long rfrom = -1;   /* PS_REGLOG_FROM=FRAME */
            if (rfrom < 0) rfrom = getenv("PS_REGLOG_FROM") ? atol(getenv("PS_REGLOG_FROM")) : 0;
            if (ps_frames >= rfrom && cnt++ < 400)
            fprintf(stderr, "reg f%ld pc %06X d0 %08X d1 %08X a0 %08X a1 %08X a2 %08X\n", ps_frames, rpc[i],
                    m68k.dar[0], m68k.dar[1], m68k.dar[8], m68k.dar[9], m68k.dar[10]); }
    }
    if (s_ru_on < 0) {
        s_ru_dir = getenv("PS_ROMUSE"); s_ru_on = s_ru_dir && *s_ru_dir;
        const char *k = getenv("PS_ROMUSE_KEY"); s_ru_keyaddr = k ? (unsigned)strtoul(k, NULL, 16) : 0xFFFE10;
        if (s_ru_on) atexit(ru_save);
    }
    if (!s_ru_on) return;
    static int z80only = -1, dmaonly, cpuonly; if (z80only < 0) { z80only = getenv("PS_ROMUSE_Z80ONLY") && atoi(getenv("PS_ROMUSE_Z80ONLY"));
        dmaonly = getenv("PS_ROMUSE_DMAONLY") && atoi(getenv("PS_ROMUSE_DMAONLY"));
        cpuonly = getenv("PS_ROMUSE_CPUONLY") && atoi(getenv("PS_ROMUSE_CPUONLY"));
        s_ru_cpuonly = cpuonly || z80only; }                  /* no DMA sources in either */
    {   /* PS_ROMUSE_FROM=FRAME: start recording there (a boot checksum loop reads the whole ROM;
         * patching it out instead would change the timing and desync input movies) */
        extern long ps_frames; static long from = -1;
        if (from < 0) { const char *f = getenv("PS_ROMUSE_FROM"); from = f ? atol(f) : 0; }
        if (ps_frames < from) return;
    }
    unsigned a = address & 0xFFFFFF;
    if (type == HOOK_M68K_E) { int k = ru_slot(); if (k >= 0) ru_mark(s_ru_exec[k], a, 2); }
    else if (((type == HOOK_Z80_R && !dmaonly && !cpuonly) || (type == HOOK_M68K_R && !z80only && !dmaonly)) && a < s_rom_size) {
        /* PS_ROMUSE_Z80ONLY=1: only the Z80's reads; PS_ROMUSE_DMAONLY=1: only DMA sources;
         * PS_ROMUSE_CPUONLY=1: only the 68K's own reads */ int k = ru_slot(); if (k >= 0) ru_mark(s_ru_read[k], a, (unsigned)width); }
    else if (type == HOOK_M68K_W && (a & 0xE0001C) == 0xC00004) {
        if (width == 4) { ru_vdp_word((uint16_t)(value >> 16)); ru_vdp_word((uint16_t)value); }
        else ru_vdp_word((uint16_t)value);
    }
}

/* PS_CD_TRACE=DIR (Mega CD): code loaded from disc runs from RAM, and a stage replaces it. Every
 * 600 frames, if word RAM changed, DIR/<frame>.wram (256 KB, 2M mode), .prgram (512 KB) and
 * .pcs (main-CPU instruction addresses executed since the last snapshot, one per line) - the
 * images to disassemble and where to start. Also at exit. */
static uint8_t s_cd_pcs[0x1000000 / 16];
static int s_cd_on = -1; static const char *s_cd_dir; static uint32_t s_cd_hash;
static void cd_snapshot(int final) {
    extern long ps_frames; extern unsigned char work_ram[];
    if (s_cd_on <= 0 || system_hw != SYSTEM_MCD || (!final && ps_frames % 600)) return;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < sizeof scd.word_ram_2M; i += 4) { h ^= *(uint32_t *)(scd.word_ram_2M + i); h *= 16777619u; }
    for (size_t i = 0; i < 0x10000; i += 4) { h ^= *(uint32_t *)(work_ram + i); h *= 16777619u; }   /* main program runs here */
    if (h == s_cd_hash && !final) return;
    s_cd_hash = h;
    char p[1024]; FILE *f;
    snprintf(p, sizeof p, "%s/%07ld.wram", s_cd_dir, ps_frames); if ((f = fopen(p, "wb"))) { fwrite(scd.word_ram_2M, 1, sizeof scd.word_ram_2M, f); fclose(f); }
    snprintf(p, sizeof p, "%s/%07ld.prgram", s_cd_dir, ps_frames); if ((f = fopen(p, "wb"))) { fwrite(scd.prg_ram, 1, sizeof scd.prg_ram, f); fclose(f); }
    { uint8_t w[0x10000]; for (int i = 0; i < 0x10000; i++) w[i] = work_ram[i ^ 1];   /* GPGX keeps it word-swapped */
      snprintf(p, sizeof p, "%s/%07ld.workram", s_cd_dir, ps_frames); if ((f = fopen(p, "wb"))) { fwrite(w, 1, sizeof w, f); fclose(f); } }
    snprintf(p, sizeof p, "%s/%07ld.pcs", s_cd_dir, ps_frames);
    if ((f = fopen(p, "w"))) {
        for (uint32_t a = 0; a < 0x1000000; a += 2) if (s_cd_pcs[a >> 4] >> ((a >> 1) & 7) & 1) fprintf(f, "%06X\n", a);
        fclose(f);
    }
    memset(s_cd_pcs, 0, sizeof s_cd_pcs);
    fprintf(stderr, "cd f%ld: comm flags main %02X sub %02X, memory mode %02X\n", ps_frames,
            scd.regs[0x0E >> 1].byte.h, scd.regs[0x0E >> 1].byte.l, scd.regs[0x03 >> 1].byte.l);
}
static void cd_final(void) { cd_snapshot(1); }

/* PS_TRAP=LO-HI (hex): log execution and 68K reads inside [LO, HI), with the instruction doing
 * them, and DMA sources there (from the VDP command words) - a relocated game's missed pointers
 * (megacd/relocate.py's trap cartridge leaves the old range empty). First 20000 distinct sites. */
static unsigned s_trap_lo[8], s_trap_hi[8]; static int s_trap_on = -1, s_trap_n;
static int in_trap(unsigned a) { for (int i = 0; i < s_trap_n; i++) if (a >= s_trap_lo[i] && a < s_trap_hi[i]) return 1; return 0; }
static void trap_hook(hook_type_t type, int width, unsigned int address, unsigned int value) {
    if (s_trap_on < 0) {                     /* PS_TRAP=LO-HI[,LO-HI...] */
        s_trap_on = 0;
        for (const char *q = getenv("PS_TRAP"); q && *q && s_trap_n < 8; ) {
            if (sscanf(q, "%x-%x", &s_trap_lo[s_trap_n], &s_trap_hi[s_trap_n]) == 2) s_trap_n++;
            q = strchr(q, ','); if (q) q++;
        }
        s_trap_on = s_trap_n > 0;
    }
    if (!s_trap_on) return;
    static unsigned last_pc, seen[20000]; static int n; static long from = -1;
    { extern long ps_frames; if (from < 0) from = getenv("PS_TRAP_FROM") ? atol(getenv("PS_TRAP_FROM")) : 0;
      if (ps_frames < from) { if (type == HOOK_M68K_E) last_pc = address & 0xFFFFFF; return; } }
    unsigned a = address & 0xFFFFFF;
    if (type == HOOK_M68K_E) {
        if (in_trap(a)) {
            for (int i = 0; i < n; i++) if (seen[i] == (a | 0x80000000u)) goto done;
            if (n < 20000) { seen[n++] = a | 0x80000000u; extern long ps_frames; fprintf(stderr, "trap f%ld: exec $%06X (from $%06X)\n", ps_frames, a, last_pc); }
        }
    done:
        last_pc = a; return;
    }
    if (type == HOOK_M68K_R && in_trap(a)) {
        for (int i = 0; i < n; i++) if (seen[i] == last_pc) return;
        if (n < 20000) { seen[n++] = last_pc; extern long ps_frames; fprintf(stderr, "trap f%ld: read%d $%06X by $%06X\n", ps_frames, width * 8, a, last_pc); }
    }
    {   /* PS_TRAPW=LO-HI: 68K writes there (e.g. a Mega CD game clobbering the BIOS jump slots) */
        /* PS_TRAPW_NOSTACK=1: not the pushes at the stack pointer; PS_TRAPW_MAX=N (default 4000) */
        static unsigned wlo = 1, whi; static int wn, wmax, nostack;
        if (wlo == 1) { const char *t = getenv("PS_TRAPW"); if (!t || sscanf(t, "%x-%x", &wlo, &whi) != 2) wlo = whi = 0;
                        wmax = getenv("PS_TRAPW_MAX") ? atoi(getenv("PS_TRAPW_MAX")) : 4000;
                        nostack = getenv("PS_TRAPW_NOSTACK") && atoi(getenv("PS_TRAPW_NOSTACK")); }
        if (type == HOOK_M68K_W && nostack && a + 8 >= (m68k.dar[15] & 0xFFFFFF) - 4 && a < (m68k.dar[15] & 0xFFFFFF) + 4) {}
        else if (type == HOOK_M68K_W && a >= wlo && a < whi && wn++ < wmax) { extern long ps_frames; fprintf(stderr, "trapw f%ld: write%d $%06X = %X by $%06X (SP %06X)\n", ps_frames, width * 8, a, value, last_pc, m68k.dar[15] & 0xFFFFFF); }
    }
    if (type == HOOK_M68K_W && (a & 0xE0001C) == 0xC00004) {     /* DMA source from the VDP command */
        static uint8_t reg[32]; static int pend;
        unsigned words[2] = { (unsigned)(value >> 16) & 0xFFFF, (unsigned)value & 0xFFFF }; int nw = width == 4 ? 2 : 1;
        for (int k = 2 - nw; k < 2; k++) {
            unsigned v = words[k];
            if (pend) {
                pend = 0;
                if ((v & 0x80) && (reg[1] & 0x10) && !(reg[23] & 0x80)) {
                    unsigned src = ((unsigned)reg[21] | reg[22] << 8 | (reg[23] & 0x7F) << 16) << 1;
                    if (getenv("PS_DMALOG")) { static int dl; extern long ps_frames; if (dl++ < 2000000) fprintf(stderr, "dma f%ld: src $%06X len %u words dst cmd by $%06X\n", ps_frames, src, (unsigned)(reg[19] | reg[20] << 8), last_pc); }
                    if (in_trap(src)) {
                        for (int i = 0; i < n; i++) if (seen[i] == (last_pc | 0x40000000u)) goto skip;
                        if (n < 200) { seen[n++] = last_pc | 0x40000000u; extern long ps_frames; fprintf(stderr, "trap f%ld: DMA from $%06X set up by $%06X\n", ps_frames, src, last_pc); }
                    skip:;
                    }
                }
            } else if ((v & 0xC000) == 0x8000) reg[(v >> 8) & 0x1F] = (uint8_t)v;
            else pend = 1;
        }
    }
}

/* PS_PTRS=FILE: ROM locations read as a longword whose value is itself a ROM address (pointer
 * tables in data: what a relocation has to rewrite besides the code). Written at exit. */
static uint8_t *s_ptr_bm; static int s_ptr_on = -1; static const char *s_ptr_path;
static void ptr_save(void) {
    FILE *f = fopen(s_ptr_path, "w"); if (!f) return;
    for (uint32_t a = 0; a < s_rom_size; a += 2) if (s_ptr_bm[a >> 4] >> ((a >> 1) & 7) & 1) fprintf(f, "%06X\n", a);
    fclose(f);
}
/* A candidate counts only once it is used as one (a level's size $000029A0 read as a longword
 * looks like a ROM address too): within the next 64 instructions the CPU reads or executes within
 * 256 bytes of the value, or at any later time a read, an instruction or a DMA starts exactly at
 * the value (art queued for decompression frames later). PS_PTRS_ALL=1: every candidate. */
#define PTR_PEND 32
static struct { unsigned loc, val; int left; } s_pend[PTR_PEND];
static uint32_t *s_cand; static uint8_t *s_start;
static void ptr_mark(unsigned a) { s_ptr_bm[a >> 4] |= (uint8_t)(1u << ((a >> 1) & 7)); }
static void ptr_final(void) {
    for (uint32_t a = 0; a < s_rom_size; a += 2) {
        uint32_t v = s_cand[a >> 1];
        if (v && (s_start[v >> 3] >> (v & 7) & 1)) ptr_mark(a);
    }
    ptr_save();
}
static void ptr_hook(hook_type_t type, int width, unsigned int address, unsigned int value) {
    static int all; static uint8_t reg[32]; static int pend;
    if (s_ptr_on < 0) {
        s_ptr_path = getenv("PS_PTRS"); s_ptr_on = s_ptr_path && *s_ptr_path;
        all = getenv("PS_PTRS_ALL") && atoi(getenv("PS_PTRS_ALL"));
        if (s_ptr_on) { s_ptr_bm = calloc(1, s_rom_size / 16 + 1); s_cand = calloc(s_rom_size / 2 + 1, 4);
                        s_start = calloc(1, s_rom_size / 8 + 1); atexit(all ? ptr_save : ptr_final); }
    }
    if (!s_ptr_on) return;
    unsigned a = address & 0xFFFFFF;
    if (type == HOOK_M68K_W && (a & 0xE0001C) == 0xC00004) {     /* 68K-bus DMA sources */
        unsigned ws[2] = { value >> 16, value & 0xFFFF }; int nw = width == 4 ? 2 : 1;
        for (int k = 2 - nw; k < 2; k++) {
            unsigned v = ws[k];
            if (pend) { pend = 0;
                if ((v & 0x80) && (reg[1] & 0x10) && !(reg[23] & 0x80)) {
                    unsigned src = ((unsigned)reg[21] | reg[22] << 8 | (reg[23] & 0x7F) << 16) << 1;
                    if (src < s_rom_size) s_start[src >> 3] |= (uint8_t)(1u << (src & 7)); }
            } else if ((v & 0xC000) == 0x8000) reg[(v >> 8) & 0x1F] = (uint8_t)v;
            else pend = 1;
        }
        return;
    }
    if (type == HOOK_M68K_E || (type == HOOK_M68K_R && a < s_rom_size)) {
        if (a < s_rom_size) s_start[a >> 3] |= (uint8_t)(1u << (a & 7));
        for (int i = 0; i < PTR_PEND; i++) {               /* a use close to the read */
            if (!s_pend[i].left) continue;
            if (a >= s_pend[i].val && a < s_pend[i].val + 0x100 && !(type == HOOK_M68K_R && a >= s_pend[i].loc && a < s_pend[i].loc + 4))
                { ptr_mark(s_pend[i].loc); s_pend[i].left = 0; }
            else if (type == HOOK_M68K_E) s_pend[i].left--;
        }
    }
    if (type != HOOK_M68K_R || width != 4) return;
    unsigned v = value & 0xFFFFFF;
    if (a < s_rom_size && !(a & 1) && v >= 0x200 && v < s_rom_size) {  /* the top byte may be a tag
                                                                          (SH1's level headers) */
        if (all) { ptr_mark(a); return; }
        s_cand[a >> 1] = v;
        static int next; s_pend[next].loc = a; s_pend[next].val = v; s_pend[next].left = 64; next = (next + 1) % PTR_PEND;
    }
}

/* PS_PCCOVER=FILE: every 68000 instruction address the original executed, one bit per even
 * address below $400000 (256 KB: byte a >> 4, bit (a >> 1) & 7). OR-merged into FILE if it exists,
 * written at exit - the union over many replays is the "executed" side of engine/tools/kpi.py. */
static uint8_t *s_pcc; static const char *s_pcc_path;
static void pcc_save(void) {
    enum { N = 0x400000 / 16 };
    FILE *f = fopen(s_pcc_path, "rb");
    if (f) { uint8_t *old = calloc(1, N); size_t got = fread(old, 1, N, f); fclose(f);
             for (size_t i = 0; i < got; i++) s_pcc[i] |= old[i]; free(old); }
    char tmp[1100]; snprintf(tmp, sizeof tmp, "%s.tmp", s_pcc_path);
    if ((f = fopen(tmp, "wb"))) { fwrite(s_pcc, 1, N, f); fclose(f); remove(s_pcc_path); rename(tmp, s_pcc_path); }
}

static void hook(hook_type_t type, int width, unsigned int address, unsigned int value) {
    (void)width; (void)value;
    ru_hook(type, width, address, value);
    trap_hook(type, width, address, value);
    ptr_hook(type, width, address, value);
    if (s_cd_on < 0) { s_cd_dir = getenv("PS_CD_TRACE"); s_cd_on = s_cd_dir && *s_cd_dir; if (s_cd_on) atexit(cd_final); }
    if (s_cd_on && type == HOOK_M68K_E) { unsigned a = address & 0xFFFFFE; s_cd_pcs[a >> 4] |= (uint8_t)(1u << ((a >> 1) & 7)); }
    if (type == HOOK_Z80_E) {            /* PS_Z80_TRACE: reference trace for the recompiled driver */
        static FILE *zt; static int init; static long n, zfrom;
        extern long ps_frames;
        if (!init) { init = 1; const char *p = getenv("PS_Z80_TRACE"); if (p && *p) zt = fopen(p, "w");
                     if (getenv("PS_Z80_TRACE_FROM")) zfrom = atol(getenv("PS_Z80_TRACE_FROM")); }
        if (zt && ps_frames >= zfrom && n++ <= 200000)
            fprintf(zt, "%ld %04X A%02X F%02X B%02X C%02X D%02X E%02X H%02X L%02X SP%04X CY%u\n", ps_frames,
                    address & 0xFFFF, Z80.af.b.h, Z80.af.b.l & 0xD7, Z80.bc.b.h, Z80.bc.b.l,
                    Z80.de.b.h, Z80.de.b.l, Z80.hl.b.h, Z80.hl.b.l, Z80.sp.w.l, Z80.cycles);
        /* PS_Z80_DUMP: Z80 RAM as the driver starts (first run from $0000 with code in RAM);
         * PS_Z80_PCS: bitmap of executed Z80 addresses. A compressed driver (SH2, unpacked
         * by the 68K) is not in the ROM verbatim: find_driver.py --image takes these. */
        static FILE *zp; static int dumped; static uint8_t pcs[0x2000 / 8];
        if (init == 1) { init = 2; const char *p = getenv("PS_Z80_PCS"); if (p && *p) zp = fopen(p, "wb"); }
        if (zp) {
            static long k; unsigned a = address & 0x1FFF;
            pcs[a >> 3] |= 1 << (a & 7);
            if (++k % 100000 == 0) { rewind(zp); fwrite(pcs, 1, sizeof pcs, zp); fflush(zp); }
        }
        /* PS_Z80_DUMP_LAST=1: the last start instead (SHK2 patches the uploaded
         * SH2 driver - its sample banks - and restarts it) */
        const char *d = getenv("PS_Z80_DUMP");
        static int last = -1; if (last < 0) last = getenv("PS_Z80_DUMP_LAST") != NULL;
        if ((!dumped || last) && (address & 0xFFFF) == 0 && Z80.pc.w.l == 0 && d && *d) {
            int nz = 0; for (int i = 0; i < 0x2000; i++) nz += zram[i] != 0;
            FILE *f;
            if (nz >= 256 && (f = fopen(d, "wb"))) { fwrite(zram, 1, 0x2000, f); fclose(f); dumped = 1; }
        }
        return;
    }
    if (type == HOOK_M68K_W) {           /* PS_MEM_WATCH=ADDR: 68K writes covering ADDR (word) */
        static unsigned wa = 1; extern long ps_frames;
        if (wa == 1) { const char *w = getenv("PS_MEM_WATCH"); wa = w ? (unsigned)strtoul(w, NULL, 16) & 0xFFFFFF : 0; }
        unsigned a = address & 0xFFFFFF;
        if (wa && a <= wa + 1 && a + (unsigned)width > wa)
            fprintf(stderr, "memwatch f%ld pc %06X write%d %06X = %X\n", ps_frames, s_prev_pc, width * 8, a, value);
        return;
    }
    if (type != HOOK_M68K_E) return;
    uint32_t pc = address & 0xFFFFFF;
    if (s_pcc && pc < 0x400000) s_pcc[pc >> 4] |= (uint8_t)(1u << ((pc >> 1) & 7));
    {   /* PS_DMA_WATCH=PC: VDP DMA state each time the 68K reaches PC (DMA wait loops) */
        static unsigned wpc = 1; extern long ps_frames;
        if (wpc == 1) { const char *w = getenv("PS_DMA_WATCH"); wpc = w ? (unsigned)strtoul(w, NULL, 16) : 0; }
        if (wpc && pc == wpc) fprintf(stderr, "dmawatch f%ld line %d cyc %u len %u status %04X end %u reg1 %02X\n",
                                      ps_frames, v_counter, m68k.cycles, dma_length, status, dma_endCycles, reg[1]);
    }
    {   /* gated RAM toggles (PS_ORACLE_TOGGLES ADDR=VAL@PC): frames on which the game ran PC */
        extern unsigned g_gate_pc; extern long g_gate_first, g_gate_last; extern long ps_frames;
        if (g_gate_pc && pc == g_gate_pc) { if (ps_frames - g_gate_last > 1) g_gate_first = ps_frames; g_gate_last = ps_frames; }
    }
    {   /* PS_FTRACE: function-entry timing, same format as the recompiled build's */
        static FILE *ft; static int init; static uint8_t *isf; static long from, to = -1;
        extern long ps_frames;
        if (!init) {
            init = 1;
            const char *p = getenv("PS_FTRACE"), *fl = getenv("PS_FTRACE_FUNCS");
            if (p && *p && fl && *fl) {
                ft = fopen(p, "w"); isf = calloc(1, 0x1000000);
                FILE *f = fopen(fl, "r"); unsigned a;
                while (f && fscanf(f, "%x", &a) == 1) isf[a & 0xFFFFFF] = 1;
                if (f) fclose(f);
                if (getenv("PS_FTRACE_FROM")) from = atol(getenv("PS_FTRACE_FROM"));
                if (getenv("PS_FTRACE_TO")) to = atol(getenv("PS_FTRACE_TO"));
            }
        }
        /* only entries reached by a call/jump or an exception, like the recompiled dispatcher */
        int via = (s_prev_op & 0xFF80) == 0x4E80 || (s_prev_op & 0xFF00) == 0x6100 || (s_prev_op & 0xFF00) == 0x6000;
        for (int i = 24; i < 32 && !via; i++) if (s_vectors[i] == pc) via = 1;
        static FILE *it; static int it_init; static long it_n;
        if (!it_init) { it_init = 1; const char *q = getenv("PS_ITRACE"); if (q && *q) it = fopen(q, "w"); }
        if (it && ps_frames >= from && (to < 0 || ps_frames <= to) && it_n++ < 60000000)
            fprintf(it, "%06X %u R%d %X %X %X %X %X %X %X %X %X %X %X %X %X %X %X I%u\n", pc, m68k.cycles, (int)(m68k.refresh_cycles - (int)m68k.cycles),
                    m68k.dar[0], m68k.dar[1], m68k.dar[2], m68k.dar[3], m68k.dar[4], m68k.dar[5], m68k.dar[6], m68k.dar[7],
                    m68k.dar[8], m68k.dar[9], m68k.dar[10], m68k.dar[11], m68k.dar[12], m68k.dar[13], m68k.dar[14],
                    (unsigned)(m68k.int_mask >> 8) & 7);
        if (ft && isf[pc] && via && ps_frames >= from && (to < 0 || ps_frames <= to))
            fprintf(ft, "%ld %u %06X\n", ps_frames, m68k.cycles, pc);
    }
    /* code in work RAM too (SH3K: the VBlank vector is a JMP stub at $FFFFF0) */
    uint16_t op = pc < s_rom_size ? rom16(pc) : pc >= 0xE00000 ? *(uint16_t *)(work_ram + (pc & 0xFFFE)) : 0;
    if (s_prev_pc != 0xFFFFFFFF) {
        uint16_t p = s_prev_op;
        int call_or_jump = (p & 0xFF80) == 0x4E80          /* JSR / JMP <ea> */
                        || (p & 0xFF00) == 0x6100;         /* BSR */
        int is_rts = (p == 0x4E75 || p == 0x4E73 || p == 0x4E77);  /* RTS/RTE/RTR */
        if (call_or_jump) mark(pc);
        else if (is_rts) mark(pc);   /* resume points; harmless if they are ordinary */
        else {
            /* exception entry: PC jumped to a vector target */
            for (int i = 2; i < 64; i++) if (s_vectors[i] == pc) { mark(pc); break; }
        }
    }
    s_prev_pc = pc;
    s_prev_op = op;
}

long ps_poll_index(void);
int main(int argc, char *argv[]) {
    (void)ps_poll_index();   /* links ps_poll.c (its constructor installs the poll hook) */
    const char *out_path = "trace_seeds.txt";
    for (int i = 1; i < argc - 1; i++)
        if (!strcmp(argv[i], "--trace-out")) {
            out_path = argv[i + 1];
            memmove(&argv[i], &argv[i + 2], (size_t)(argc - i - 2) * sizeof(char *));
            argc -= 2;
            break;
        }
    argc = platform_parse_args(argc, argv);
    if (argc < 2) {
        fprintf(stderr, "usage: genrecomp_trace [--headless] [--frames N] [--press F:BTN[:LEN]] "
                        "[--trace-out FILE] rom.gen\n");
        return 1;
    }
    if (!genrecomp_init("genrecomp trace", 3)) return 1;
    { void ps_megacd_bios(void); ps_megacd_bios(); }   /* Mega CD discs */
    char path[MAXPATHLEN];
    strncpy(path, argv[1], sizeof(path) - 1);
    path[sizeof(path) - 1] = 0;
    { extern void ps_60hz(const char *); ps_60hz(path); }
    if (!load_rom(path)) { fprintf(stderr, "trace: cannot load %s\n", argv[1]); return 1; }
    fprintf(stderr, "trace: %s timing (%u lines), version register PAL bit %d\n", vdp_pal ? "50 Hz" : "60 Hz",
            vdp_pal ? 313u : 262u, (region_code >> 6) & 1);
    s_rom_size = cart.romsize;
    for (int i = 0; i < 64; i++)
        s_vectors[i] = ((uint32_t)rom16(i * 4) << 16 | rom16(i * 4 + 2)) & 0xFFFFFF;
    mark(s_vectors[1]);
    audio_init(44100, 0);
    input.system[0] = SYSTEM_GAMEPAD;
    input.system[1] = SYSTEM_GAMEPAD;
    system_init();
    system_reset();
    { void ps_megacd_brm(bool load_files, bool save); ps_megacd_brm(false, false); }   /* formatted, empty */
    s_pcc_path = getenv("PS_PCCOVER");
    if (s_pcc_path && *s_pcc_path) { s_pcc = calloc(1, 0x400000 / 16); atexit(pcc_save); }
    set_cpu_hook(hook);

    static uint8_t present[GEN_RENDER_WIDTH * GEN_RENDER_HEIGHT * 4];
    static int16_t audio[4096];
    /* platform_frame_sync() exits the process at --frames: write seeds every frame
     * batch so the file is complete whenever that happens. */
    long frame = 0;
    for (;;) {
        if (!platform_poll_events()) break;
        recomp_input_update();
        {   /* RAM codes of a replayed session (SESSION.meta toggles), as the port's menu
             * applies them each frame: PS_ORACLE_TOGGLES=F325=03,F323=03,F321=05 (bit order) */
            static int init, n, ne; static unsigned ad[16], va[16], gpc[16]; static long ef[256]; static unsigned et[256];
            extern long ps_frames; extern unsigned char work_ram[];
            if (!init) {
                init = 1; const char *t = getenv("PS_ORACLE_TOGGLES"), *r = getenv("PS_POLL_REPLAY");
                for (const char *q = t; q && *q && n < 16; ) {
                    if (sscanf(q, "%x=%x", &ad[n], &va[n]) == 2) {
                        const char *at = strchr(q, '@'), *cm = strchr(q, ',');
                        gpc[n] = at && (!cm || at < cm) ? (unsigned)strtoul(at + 1, NULL, 16) : 0;
                        if (gpc[n]) g_gate_pc = gpc[n];   /* the port's ps_menu_set_toggle_gate */
                        n++;
                    }
                    q = strchr(q, ','); if (q) q++;
                }
                if (n && r) {
                    char m[1024]; snprintf(m, sizeof m, "%s", r); char *d = strrchr(m, '.'); if (d) *d = 0;
                    strncat(m, ".meta", sizeof m - strlen(m) - 1);
                    FILE *f = fopen(m, "r"); char line[256]; long fr; int o; unsigned c, tg;
                    while (f && fgets(line, sizeof line, f) && ne < 256)
                        if (sscanf(line, "logic %ld %d %u %u", &fr, &o, &c, &tg) == 4) { ef[ne] = fr; et[ne++] = tg; }
                    if (f) fclose(f);
                }
            }
            {   /* PS_POKE=ADDR=VAL@FRAME[,...]: one work-RAM byte written at that frame (scripted
                 * setups: e.g. SH1's level-select flag and cursor, to load every act) */
                static int pn = -1; static unsigned pa[64], pv[64]; static long pf[64];
                if (pn < 0) {
                    pn = 0;
                    for (const char *q = getenv("PS_POKE"); q && *q && pn < 64; ) {
                        if (sscanf(q, "%x=%x@%ld", &pa[pn], &pv[pn], &pf[pn]) == 3) pn++;
                        q = strchr(q, ','); if (q) q++;
                    }
                }
                {   /* PS_RAMDUMP=FRAME:FILE: work RAM (byte order of the 68K) at that frame */
                    static long rf = -2; static char rp[600];
                    if (rf == -2) { rf = -1; const char *q = getenv("PS_RAMDUMP"); if (q) sscanf(q, "%ld:%599s", &rf, rp); }
                    if (rf == ps_frames) { FILE *f = fopen(rp, "wb"); if (f) { for (int i = 0; i < 0x10000; i++) fputc(work_ram[i ^ 1], f);
                        for (int i = 0; i < 0x10000; i++) fputc(vram[i ^ 1], f); fwrite(cram, 1, 0x80, f); fwrite(reg, 1, 0x20, f); fwrite(vsram, 1, 0x80, f); fclose(f); } }   /* + VRAM, CRAM, VDP registers */
                }
                {   /* PS_RAMDUMP_EVERY=N:DIR: the same dump every N frames, DIR/fNNNNNN.ram */
                    static long re = -2; static char rd[560];
                    if (re == -2) { re = -1; const char *q = getenv("PS_RAMDUMP_EVERY"); if (q) sscanf(q, "%ld:%559s", &re, rd); }
                    if (re > 0 && ps_frames % re == 0) {
                        char fn[600]; snprintf(fn, sizeof fn, "%s/f%06ld.ram", rd, ps_frames);
                        if (getenv("PS_VSRAM_LOG")) fprintf(stderr, "vsram f%ld %02X%02X %02X%02X\n", ps_frames, vsram[0], vsram[1], vsram[2], vsram[3]);
                        FILE *f = fopen(fn, "wb");
                        if (f) { for (int i = 0; i < 0x10000; i++) fputc(work_ram[i ^ 1], f);
                            for (int i = 0; i < 0x10000; i++) fputc(vram[i ^ 1], f); fwrite(cram, 1, 0x80, f); fwrite(reg, 1, 0x20, f); fwrite(vsram, 1, 0x80, f); fclose(f); }
                    }
                }
                for (int i = 0; i < pn; i++) if (pf[i] == ps_frames) { work_ram[(pa[i] & 0xFFFF) ^ 1] = (unsigned char)pv[i]; if (getenv("PS_POKE_LOG")) fprintf(stderr, "poke f%ld %04X=%02X mode %02X\n", ps_frames, pa[i] & 0xFFFF, pv[i], work_ram[0xF600 ^ 1]); }
            }
            {   /* code cheats (Game Genie style) as a real Game Genie or the port's compiled
                 * switch behaves: after boot, so the ROM checksum test still passes.
                 * PS_ORACLE_PATCH=9F6C=4E71[,ADDR=WORD] applied from frame 300 */
                static int done; const char *pt = getenv("PS_ORACLE_PATCH");
                if (pt && !done && ps_frames >= 300) {
                    done = 1;
                    for (const char *q = pt; *q; ) {
                        unsigned a, v;
                        if (sscanf(q, "%x=%x", &a, &v) == 2) { cart.rom[a ^ 1] = (uint8)(v >> 8); cart.rom[(a + 1) ^ 1] = (uint8)v; }
                        q = strchr(q, ','); if (!q) break; q++;
                    }
                }
            }
            unsigned cur = 0;
            for (int i = 0; i < ne && ef[i] < ps_frames; i++) cur = et[i];   /* port: menu step after the frame boundary */
            static int on[16]; static unsigned orig[16];
            for (int i = 0; i < n; i++) {
                if (ad[i] < 0x400000) {          /* ROM toggle (Game Genie data code): word patch */
                    int want = (cur >> i & 1) && ps_frames >= 300;
                    if (want != on[i]) {
                        unsigned a = ad[i] & ~1u;
                        if (want) orig[i] = (unsigned)cart.rom[a ^ 1] << 8 | cart.rom[(a + 1) ^ 1];
                        unsigned v = want ? va[i] : orig[i];
                        cart.rom[a ^ 1] = (uint8)(v >> 8); cart.rom[(a + 1) ^ 1] = (uint8)v; on[i] = want;
                    }
                } else if ((cur >> i & 1) && (!gpc[i] || (ps_frames - g_gate_last <= 1 && ps_frames - g_gate_first >= 60)))
                    work_ram[(ad[i] & 0xFFFF) ^ 1] = (unsigned char)va[i];
            }
        }
        if (system_hw == SYSTEM_MCD) system_frame_scd(0); else system_frame_gen(0);
        cd_snapshot(0);
        { extern long ps_frames; ps_frames++;
          static FILE *fl; static int init;
          if (!init) { init = 1; const char *q = getenv("PS_FRAMELOG"); if (q && *q) fl = fopen(q, "w"); }
          extern uint16 spr_col;
          if (fl) fprintf(fl, "%ld %u %u dma=%u st=%04X col=%04X\n", ps_frames, m68k.cycles, Z80.cycles, dma_length, status, spr_col); }
        audio_update(audio);
        if (++frame % 60 == 0 || frame < 2) {
            FILE *f = fopen(out_path, "w");
            if (f) {
                for (uint32_t a = 0x200; a < s_rom_size; a += 2)
                    if (s_seen[a >> 1]) fprintf(f, "0x%06X\n", a);
                fclose(f);
            }
        }
        memset(present, 0, sizeof(present));
        int w = bitmap.viewport.w < GEN_RENDER_WIDTH ? bitmap.viewport.w : GEN_RENDER_WIDTH;
        int h = bitmap.viewport.h < GEN_RENDER_HEIGHT ? bitmap.viewport.h : GEN_RENDER_HEIGHT;
        const uint8_t *src = bitmap.data + bitmap.viewport.y * bitmap.pitch + bitmap.viewport.x * 4;
        for (int y = 0; y < h; y++)
            memcpy(present + y * GEN_RENDER_WIDTH * 4, src + y * bitmap.pitch, (size_t)w * 4);
        /* TRACE_DUMP_DIR + TRACE_DUMP_EVERY=N: PNG of every Nth frame (sync checks) */
        static const char *dump_dir = (const char *)-1;
        static long dump_every;
        if (dump_dir == (const char *)-1) {
            dump_dir = getenv("TRACE_DUMP_DIR");
            dump_every = getenv("TRACE_DUMP_EVERY") ? atol(getenv("TRACE_DUMP_EVERY")) : 0;
        }
        if (dump_dir && dump_every > 0 && frame % dump_every == 0) {
            char png[512];
            snprintf(png, sizeof png, "%s/t%06ld.png", dump_dir, frame);
            ps_png_write(png, present, GEN_RENDER_WIDTH, GEN_RENDER_HEIGHT);
        }
        platform_present_frame(present);
        platform_frame_sync();
    }
    genrecomp_shutdown();
    return 0;
}
