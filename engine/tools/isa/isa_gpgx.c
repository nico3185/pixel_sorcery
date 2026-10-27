/* isa_gpgx.c: the same instruction tests run on Genesis Plus GX's 68000 core (the oracle every
 * replay is compared against). isa_check.py builds it with each isa_<group>.c (the generated test
 * functions are ignored here) to sort the recompiler's failures: where GPGX matches the suite the
 * recompiler is wrong; where GPGX disagrees with the suite too, the case is reported as a dispute
 * (our replays prove equality with GPGX, not with the suite's reference CPU).
 * One line per test: "GPGX\t<name>\t<OK or the first difference>\t<68000 clocks>". */
#include <stdio.h>
#include <string.h>
#include "m68k/m68kcpu.c"
#include "isa_rt.h"

int vdp_68k_irq_ack(int level) { (void)level; return 0; }
void error(char *fmt, ...) { (void)fmt; }
uint16 v_counter;
m68ki_cpu_core m68k;

static uint8 mem[1 << 24];                          /* word-swapped, as GPGX's banks are (LSB_FIRST) */
static void put(uint32_t a, uint8_t v) { mem[(a & 0xFFFFFF) ^ 1] = v; }
static uint8_t get(uint32_t a) { return mem[(a & 0xFFFFFF) ^ 1]; }

static const char *RN[19] = {"d0","d1","d2","d3","d4","d5","d6","d7","a0","a1","a2","a3","a4","a5","a6","usp","ssp","sr","pc"};

void isa_fn_unused(void) {}

int main(void) {
    for (int i = 0; i < 256; i++) {
        m68k.memory_map[i].base = mem + (i << 16);
        m68k.memory_map[i].read8 = NULL; m68k.memory_map[i].read16 = NULL;
        m68k.memory_map[i].write8 = NULL; m68k.memory_map[i].write16 = NULL;
    }
    m68k_init();
    for (int k = 0; k < isa_ntests; k++) {
        const IsaTest *t = &isa_tests[k];
        const uint32_t *ram = isa_ram + 2 * t->ram0;
        for (int i = 0; i < t->nram0; i++) put(ram[2 * i], (uint8_t)ram[2 * i + 1]);
        m68k_pulse_reset();
        for (int i = 0; i < 8; i++) { m68k_set_reg(M68K_REG_D0 + i, t->init[i]); }
        for (int i = 0; i < 7; i++) { m68k_set_reg(M68K_REG_A0 + i, t->init[8 + i]); }
        m68k_set_reg(M68K_REG_SR, 0x2700);                 /* supervisor while the stacks are set */
        m68k_set_reg(M68K_REG_USP, t->init[15]);
        m68k_set_reg(M68K_REG_ISP, t->init[16]);
        m68k_set_reg(M68K_REG_A7, t->init[16]);
        m68k_set_reg(M68K_REG_SR, t->init[17]);
        m68k_set_reg(M68K_REG_PC, t->init[18]);
        m68k.cycles = 0; m68k.refresh_cycles = 0x40000000;
        m68k_run(1);                                       /* one instruction */
        uint32_t got[19];
        for (int i = 0; i < 8; i++) got[i] = m68k_get_reg(M68K_REG_D0 + i);
        for (int i = 0; i < 7; i++) got[8 + i] = m68k_get_reg(M68K_REG_A0 + i);
        got[15] = m68k_get_reg(M68K_REG_USP); got[16] = m68k_get_reg(M68K_REG_ISP);
        got[17] = m68k_get_reg(M68K_REG_SR); got[18] = m68k_get_reg(M68K_REG_PC);
        if (got[17] & 0x2000) got[16] = m68k_get_reg(M68K_REG_A7); else got[15] = m68k_get_reg(M68K_REG_A7);
        char why[200] = "";
        const uint32_t *f = t->fin;
        for (int i = 0; i < 19 && !*why; i++) {
            if (t->vector >= 0 && (i == 18 || i == 16 || i == 17)) continue;   /* exception: regs only */
            uint32_t m = i == 17 ? 0x271F : i == 18 ? 0xFFFFFF : 0xFFFFFFFF;
            if ((got[i] & m) != (f[i] & m)) snprintf(why, sizeof why, "%s %08X, expected %08X", RN[i], got[i], f[i]);
        }
        const uint32_t *r1 = isa_ram + 2 * t->ram1;
        for (int i = 0; i < t->nram1 && !*why && t->vector < 0; i++)
            if (get(r1[2 * i]) != r1[2 * i + 1])
                snprintf(why, sizeof why, "ram[%06X] %02X, expected %02X", r1[2 * i] & 0xFFFFFF, get(r1[2 * i]), r1[2 * i + 1]);
        printf("GPGX\t%s\t%s\t%d\n", t->name, *why ? why : "OK", m68k.cycles / 7);
        for (int i = 0; i < t->nram0; i++) put(ram[2 * i], 0);
        for (int i = 0; i < t->nram1; i++) put(r1[2 * i], 0);
    }
    return 0;
}
