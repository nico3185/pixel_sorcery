#ifndef GENRECOMP_FUNC_TABLE_H
#define GENRECOMP_FUNC_TABLE_H

/*
 * Recompiled function dispatch table.
 *
 * Register your recompiled C functions at their original M68K 24-bit
 * addresses, then call them by address. All functions use the global
 * CPU state (g_m68k) and bus_read/bus_write for memory access.
 */

#include <stdint.h>
#include <stdbool.h>

/* All recompiled functions: no args, no return, use globals */
typedef void (*gen_func_t)(void);

void        func_table_init(void);
void        func_table_register(uint32_t m68k_addr, gen_func_t func);
gen_func_t  func_table_lookup(uint32_t m68k_addr);
bool        func_table_call(uint32_t m68k_addr);

/* Tail jump: record the target and return from the current recompiled
 * function; the enclosing func_table_call() runs it without growing the
 * C stack. Generated code: { func_table_tail(0x1234); return; } */
void        func_table_tail(uint32_t m68k_addr);

/* JSR/BSR: push the return address on the 68K stack and call. The frame
 * remembers it, so func_table_rts() can tell a normal return from code that
 * returns somewhere it arranged itself (PEA + BRA, push + JMP).
 * Returns false when an RTS deeper down returned straight to an outer
 * frame's continuation (stack-dropping returns): the caller must return
 * immediately. Generated code: if (!func_table_jsr(t, ret)) return; */
bool        func_table_jsr(uint32_t m68k_addr, uint32_t ret);
/* C call depth, and the unwind an RTE at an exception's own frame starts from deeper frames:
 * frames deeper than d + 1 (the handler's) return; func_table_unwind_done(d) once back at d. */
int         func_table_depth(void);
void        func_table_unwind_to(int d);
void        func_table_unwind_done(int d);

/* Resumable interior entry: a function entered through a resume wrapper
 * (an RTS into the middle of it) jumps to this PC first. 0 = normal entry. */
extern uint32_t g_func_resume_pc;

/* RTS: pop the return address; if it isn't the one this frame's JSR pushed,
 * continue there (as a tail jump) instead of returning. Generated code:
 * { func_table_rts(); return; } */
void        func_table_rts(void);

/* Interpret from m68k_addr (m68k_interp.c): code the recompiler did not translate */
void        m68k_interp_run(uint32_t m68k_addr);

/* func_table_call with a known return address (used by func_table_jsr) */
bool        func_table_call_expecting(uint32_t m68k_addr, uint32_t ret);

/* Print the shadow call stack (outermost first). Called from a VBlank
 * callback it shows where the game's main thread is blocked. */
#include <stdio.h>
void        func_table_dump_stack(FILE *f);

/* Code loaded at run time (Mega CD programs in Word RAM): one overlay per program image. Its
 * compiled functions are used only while that image is in memory: the opcode word of every
 * compiled instruction (addrs) must hash to `hash`, checked again whenever
 * func_table_overlay_key(its address) changes: Word RAM handed over (Mega CD memory mode register),
 * or for work RAM programs a main-CPU write over one of their instructions
 * (g_ps_overlay_wram_code: one bit per even address of $FF0000-$FFFFFF, bus.c counts the writes). */
typedef struct { uint32_t addr; gen_func_t fn; } PsOverlayFunc;
typedef struct PsOverlay {
    const char *name;
    uint32_t lo, hi;                     /* the addresses its functions cover */
    const uint32_t *addrs; uint32_t naddrs; uint32_t hash;
    PsOverlayFunc *funcs; uint32_t nfuncs;
    uint32_t key; int state;             /* runtime: key last checked, 1 resident, -1 not */
} PsOverlay;
void func_table_add_overlay(PsOverlay *ov);
const char *func_table_last_overlay(void);   /* the overlay last called into, or NULL */
extern uint32_t (*func_table_overlay_key)(uint32_t addr);
extern uint8_t g_ps_overlay_wram_code[0x1000];

#endif /* GENRECOMP_FUNC_TABLE_H */
