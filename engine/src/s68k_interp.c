/*
 * s68k_interp.c -- the Mega CD's sub CPU on the port's own 68000 interpreter (BACKLOG priority 0,
 * step 2): m68k_interp.c compiled a second time with PS_SUBCPU and its globals renamed, so the
 * main CPU's build of it is unchanged. Nothing is compiled for the sub CPU: every call stays in
 * the interpreter, and a run is a slice of a free-running CPU (s68k_run: up to a cycle budget).
 *
 * The bus is the title's: s68k_read8/16/32, s68k_write8/16/32 (PRG-RAM, Word RAM, PCM, the gate
 * array); interrupts come from s68k_set_irq (level 1-6, autovectored as on the Mega CD).
 * Cycles count in the same units as the main CPU's table (68000 clocks x 7).
 */
#include <stdbool.h>
#include <stdint.h>
#include "genrecomp/s68k.h"

/* every global the interpreter (and m68k.h's inline helpers and macros) names, renamed before
 * the headers are read: their declarations then declare the sub CPU's */
#define g_m68k g_s68k
#define g_cyc g_scyc
#define g_cyc_event g_scyc_event
#define g_cyc_skip g_scyc_skip
#define g_refresh g_scyc_refresh
#define g_refresh_hold g_scyc_refresh_hold
#define g_m68k_pf g_s68k_pf
#define g_m68k_icost g_scyc_icost
#define g_m68k_ipc g_s68k_ipc
#define g_m68k_ipc_kind g_s68k_ipc_kind
#define g_m68k_redirect g_s68k_redirect
#define g_rts_orphan g_s68k_rts_orphan
#define g_jsr_from_interp g_s68k_jsr_from_interp
#define bus_read8 s68k_read8
#define bus_read16 s68k_read16
#define bus_read32 s68k_read32
#define bus_write8 s68k_write8
#define bus_write16 s68k_write16
#define bus_write32 s68k_write32
#define bus_cycle_event s68k_cycle_event
#define recomp_m68k_exception s68k_exception
#define m68k_rte s68k_rte
#define func_table_lookup s68k_ft_lookup
#define func_table_jsr s68k_ft_jsr
#define func_table_tail s68k_ft_tail
#define func_table_rts s68k_ft_rts
#define m68k_interp_run s68k_interp_run
#define m68k_interp_entered s68k_interp_entered
#define m68k_interp_instructions s68k_interp_instructions
#define func_table_call s68k_ft_call
#define g_m68k_irq_pending g_s68k_irq_pending   /* m68k_set_sr: interrupts are s68k_boundary's */
#define g_irq_phase_at g_s68k_irq_phase_at
#define g_irq_phase_set g_s68k_irq_phase_set
#define func_table_init s68k_ft_init
#include "genrecomp/m68k.h"
#include "genrecomp/bus.h"
#include "genrecomp/func_table.h"

M68kContext g_s68k;
unsigned int g_scyc, g_scyc_event, g_scyc_skip, g_scyc_refresh, g_scyc_refresh_hold, g_scyc_icost;
uint32_t g_s68k_pf, g_s68k_ipc, g_s68k_redirect, g_s68k_rts_orphan;
uint8_t g_s68k_ipc_kind, g_s68k_irq_pending;
unsigned g_s68k_irq_phase_at; bool g_s68k_irq_phase_set;
int g_s68k_jsr_from_interp;
static uint32_t s_jump;                 /* an exception taken inside an instruction: its handler */
static int s_stopped;                   /* STOP: waiting for an interrupt */
static unsigned s_irq;                  /* pending levels (bit n = level n) */
static unsigned int s_end;              /* the slice's end (g_scyc) */

/* nothing is compiled for the sub CPU: every lookup misses, so calls and jumps stay interpreted */
void s68k_cycle_event(void) {}                       /* never due: g_scyc_event stays ahead */
gen_func_t s68k_ft_lookup(uint32_t a) { (void)a; return NULL; }
bool s68k_ft_jsr(uint32_t a, uint32_t r) { (void)a; (void)r; return false; }
void s68k_ft_tail(uint32_t a) { (void)a; }
void s68k_ft_rts(void) {}

/* the main CPU's refresh and trace hooks do not apply to the sub CPU */
#undef M68K_PRE
#define M68K_PRE() ((void)0)
#undef ITRACE
#define ITRACE(pc) ((void)0)

#define PS_SUBCPU 1
#include "m68k_interp.c"

static void push_frame(uint32_t pc) {
    uint16_t sr = m68k_get_sr();
    if (!g_s68k.flag_S) { g_s68k.usp = g_s68k.a[7]; g_s68k.a[7] = g_s68k.ssp; g_s68k.flag_S = true; }
    g_s68k.a[7] -= 4; s68k_write32(g_s68k.a[7], pc);
    g_s68k.a[7] -= 2; s68k_write16(g_s68k.a[7], sr);
}
/* TRAP, CHK, divide by zero, illegal: g_s68k.pc holds the PC to stack (as the main CPU's) */
void s68k_exception(uint8_t vector) {
    g_scyc += (vector == 5 || vector == 6 ? 38 : 34) * 7;
    push_frame(g_s68k.pc);
    s_jump = s68k_read32((uint32_t)vector * 4) & 0xFFFFFF;
}
bool s68k_rte(uint32_t *pc) {
    uint32_t sp = g_s68k.a[7];
    uint16_t sr = s68k_read16(sp);
    *pc = s68k_read32(sp + 2) & 0xFFFFFF;
    g_s68k.a[7] = sp + 6;
    m68k_set_sr(sr);
    return true;
}
void s68k_stop(void) { s_stopped = 1; }

static int irq_level(void) {
    for (int l = 7; l >= 1; l--)
        if ((s_irq >> l & 1) && (l > g_s68k.int_mask || l == 7)) return l;
    return 0;
}
/* every instruction boundary: an exception's handler, then an interrupt, then the slice's end */
uint32_t s68k_boundary(uint32_t next) {
    if (s_jump) { next = s_jump; s_jump = 0; }
    int l = irq_level();
    if (l) {
        s_irq &= ~(1u << l); s_stopped = 0;
        push_frame(next);
        g_s68k.int_mask = (uint8_t)l;
        g_scyc += 44 * 7;                                /* interrupt acknowledge + vector fetch */
        next = s68k_read32((uint32_t)(24 + l) * 4) & 0xFFFFFF;
    }
    if (s_stopped && (int)(g_scyc - s_end) < 0) g_scyc = s_end;
    if ((int)(g_scyc - s_end) >= 0) { g_s68k.pc = next; return 0; }
    g_scyc_event = g_scyc + 0x40000000u;
    return next;
}

void s68k_set_irq(int level) { if (level >= 1 && level <= 7) s_irq |= 1u << level; }
void s68k_clear_irq(int level) { if (level >= 1 && level <= 7) s_irq &= ~(1u << level); }
void s68k_reset(void) {
    g_s68k = (M68kContext){ 0 };
    g_s68k.flag_S = true; g_s68k.int_mask = 7;
    g_s68k.a[7] = g_s68k.ssp = s68k_read32(0);
    g_s68k.pc = s68k_read32(4) & 0xFFFFFF;
    s_irq = 0; s_stopped = 0; s_jump = 0;
}
void s68k_run(unsigned int end) {
    s_end = end;
    if ((int)(g_scyc - s_end) >= 0) return;
    while ((int)(g_scyc - s_end) < 0) {
        if (s_stopped && !irq_level()) { g_scyc = s_end; break; }
        s68k_interp_run(g_s68k.pc);                  /* returns at the slice's end */
    }
}
