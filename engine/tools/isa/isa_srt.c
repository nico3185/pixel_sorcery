/* isa_srt.c: the instruction tests (isa_check.py --sub) on the Mega CD sub CPU's interpreter
 * (s68k_interp.c, m68k_interp.c built with PS_SUBCPU): a flat 24-bit memory as its bus, one
 * slice of one instruction per test (s68k_run to the first boundary after it). The sub CPU takes
 * its own exceptions - frame pushed, handler from the vector table - so every test, exception or
 * not, is compared with the suite's final state: registers, SR, memory, PC, and cycles. */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <genrecomp/m68k.h>
#include <genrecomp/s68k.h>
#include "isa_rt.h"

extern M68kContext g_s68k;
/* the main CPU's names m68k.h declares: unused here, defined for the link */
M68kContext g_m68k;
uint8_t g_m68k_irq_pending;
unsigned int g_cyc, g_cyc_event;

static uint8_t mem[1 << 24];
uint8_t s68k_read8(uint32_t a) { return mem[a & 0xFFFFFF]; }
uint16_t s68k_read16(uint32_t a) { a &= 0xFFFFFF; return (uint16_t)(mem[a] << 8 | mem[(a + 1) & 0xFFFFFF]); }
uint32_t s68k_read32(uint32_t a) { return (uint32_t)s68k_read16(a) << 16 | s68k_read16(a + 2); }
void s68k_write8(uint32_t a, uint8_t v) { mem[a & 0xFFFFFF] = v; }
void s68k_write16(uint32_t a, uint16_t v) { s68k_write8(a, (uint8_t)(v >> 8)); s68k_write8(a + 1, (uint8_t)v); }
void s68k_write32(uint32_t a, uint32_t v) { s68k_write16(a, (uint16_t)(v >> 16)); s68k_write16(a + 2, (uint16_t)v); }

static const char *RN[19] = {"d0","d1","d2","d3","d4","d5","d6","d7","a0","a1","a2","a3","a4","a5","a6","usp","ssp","sr","pc"};

static uint16_t get_sr(void) {
    const M68kContext *c = &g_s68k;
    return (uint16_t)(c->flag_C | c->flag_V << 1 | c->flag_Z << 2 | c->flag_N << 3 | c->flag_X << 4 |
                      (c->flag_S ? 0x2000 : 0) | (c->int_mask & 7) << 8);
}
static void load(const uint32_t *r, const uint32_t *ram, int n) {
    for (int i = 0; i < n; i++) mem[ram[2 * i] & 0xFFFFFF] = (uint8_t)ram[2 * i + 1];
    M68kContext *c = &g_s68k;
    memcpy(c->d, r, 32); memcpy(c->a, r + 8, 28);
    c->usp = r[15]; c->ssp = r[16];
    uint16_t sr = (uint16_t)r[17];
    c->flag_C = sr & 1; c->flag_V = sr >> 1 & 1; c->flag_Z = sr >> 2 & 1; c->flag_N = sr >> 3 & 1; c->flag_X = sr >> 4 & 1;
    c->flag_S = (sr & 0x2000) != 0; c->int_mask = (uint8_t)(sr >> 8 & 7);
    c->a[7] = c->flag_S ? c->ssp : c->usp;
    c->pc = r[18];
}

int main(int argc, char **argv) {
    int verbose = argc > 1;
    int pass = 0, fail = 0, cyc_bad = 0;
    for (int k = 0; k < isa_ntests; k++) {
        const IsaTest *t = &isa_tests[k];
        load(t->init, isa_ram + 2 * t->ram0, t->nram0);
        g_scyc = 0;
        s68k_run(1);                               /* one instruction: the slice ends at its boundary */
        M68kContext *c = &g_s68k;
        if (c->flag_S) c->ssp = c->a[7]; else c->usp = c->a[7];
        char why[256] = "";
        const uint32_t *f = t->fin;
        uint32_t got[19];
        memcpy(got, c->d, 32); memcpy(got + 8, c->a, 28);
        got[15] = c->usp; got[16] = c->ssp; got[17] = get_sr(); got[18] = c->pc;
        for (int i = 0; i < 19 && !*why; i++) {
            /* SR: the trace bit is not modelled; PC: the 24-bit bus */
            uint32_t m = i == 17 ? 0x271F : i == 18 ? 0xFFFFFF : 0xFFFFFFFF;
            if ((got[i] & m) != (f[i] & m)) snprintf(why, sizeof why, "%s %08X, expected %08X", RN[i], got[i], f[i]);
        }
        const uint32_t *r1 = isa_ram + 2 * t->ram1;
        for (int i = 0; i < t->nram1 && !*why; i++)
            if (mem[r1[2 * i] & 0xFFFFFF] != r1[2 * i + 1])
                snprintf(why, sizeof why, "ram[%06X] %02X, expected %02X", r1[2 * i] & 0xFFFFFF, mem[r1[2 * i] & 0xFFFFFF], r1[2 * i + 1]);
        for (int i = 0; i < t->nram0; i++) mem[isa_ram[2 * (t->ram0 + i)] & 0xFFFFFF] = 0;   /* clean slate */
        for (int i = 0; i < t->nram1; i++) mem[isa_ram[2 * (t->ram1 + i)] & 0xFFFFFF] = 0;
        unsigned cyc = g_scyc / 7;
        if (*why) { fail++; printf("FAIL\t%s\t%s\n", t->name, why); }
        else {
            pass++;
            if (t->vector < 0 && cyc != t->cycles) { cyc_bad++; if (verbose) printf("CYC\t%s\t%u, expected %u\n", t->name, cyc, t->cycles); }
        }
    }
    printf("TOTAL\t%d\t%d\t%d\n", pass, fail, cyc_bad);
    return 0;
}
