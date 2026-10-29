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

#endif /* GENRECOMP_FUNC_TABLE_H */
