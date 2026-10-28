/*
 * Recompiled function dispatch table.
 * Hash table mapping 24-bit M68K addresses to native C function pointers.
 */

#include <genrecomp/bus.h>
#include "genrecomp/func_table.h"
#include "genrecomp/m68k.h"
#include "genrecomp/bus.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* Timing trace (PS_FTRACE=file, PS_FTRACE_FROM/TO = frame window): every function entry
 * as "frame cycle address", to compare against the oracle (tracer PS_FTRACE) entry by
 * entry; file.funcs lists the function addresses the oracle should log. */
static FILE *s_ftrace; static long s_ft_from, s_ft_to = -1; static int s_ft_init;
extern long ps_frames; extern unsigned int g_cyc;
static void ftrace(uint32_t a);

#define FUNC_TABLE_SIZE  65536 /* Must be power of 2 (SHK2 registers more than 16384) */

typedef struct FuncEntry {
    uint32_t    m68k_addr;
    gen_func_t  func;
    bool        occupied;
} FuncEntry;

static FuncEntry s_table[FUNC_TABLE_SIZE];

void func_table_init(void) {
    memset(s_table, 0, sizeof(s_table));
}

static uint32_t hash_addr(uint32_t addr) {
    addr = ((addr >> 16) ^ addr) * 0x45D9F3B;
    addr = ((addr >> 16) ^ addr) * 0x45D9F3B;
    addr = (addr >> 16) ^ addr;
    return (addr >> 1) & (FUNC_TABLE_SIZE - 1);      /* code addresses are even */
}

void func_table_register(uint32_t m68k_addr, gen_func_t func) {
    uint32_t idx = hash_addr(m68k_addr);
    for (uint32_t i = 0; i < FUNC_TABLE_SIZE; i++) {
        uint32_t slot = (idx + i) & (FUNC_TABLE_SIZE - 1);
        if (!s_table[slot].occupied || s_table[slot].m68k_addr == m68k_addr) {
            s_table[slot].m68k_addr = m68k_addr;
            s_table[slot].func = func;
            s_table[slot].occupied = true;
            return;
        }
    }
    fprintf(stderr, "func_table: table full, cannot register $%06X\n", m68k_addr);
}

/* PS_INTERP=all: every lookup misses, so the interpreter runs the whole game (the
 * co-simulation check of m68k_interp.c against the original, side by side).
 * PS_INTERP=random:PCT[:SEED]: that share of functions is interpreted, chosen by a hash of
 * the address (the same set every run for a seed): the compiled/interpreted hand-over tested
 * on every kind of call without play - unknown code must run exactly.
 * PS_INTERP=ADDR[,ADDR...] (hex): only these functions are interpreted, as if not compiled
 * (the compiled/interpreted hand-over, tested on chosen functions). */
static int s_force_interp = -1;
static uint32_t s_interp_only[64]; static int s_ninterp_only;
static unsigned s_interp_pct, s_interp_seed;   /* PS_INTERP=random:PCT:SEED */
static gen_func_t table_find(uint32_t m68k_addr);
gen_func_t func_table_lookup(uint32_t m68k_addr) {
    if (s_force_interp < 0) {
        const char *e = getenv("PS_INTERP");
        s_force_interp = e && !strcmp(e, "all");
        if (e && sscanf(e, "random:%u:%u", &s_interp_pct, &s_interp_seed) >= 1) e = NULL;
        if (e && !s_force_interp)
            for (const char *p = e; *p && s_ninterp_only < 64; ) {
                char *q; unsigned long v = strtoul(*p == '$' ? p + 1 : p, &q, 16);
                if (q == p || q == p + 1) break;
                s_interp_only[s_ninterp_only++] = (uint32_t)v; p = *q == ',' ? q + 1 : q;
            }
    }
    if (s_force_interp) return NULL;
    if (s_interp_pct) {                          /* a fixed pseudo-random share of functions */
        uint32_t h = (m68k_addr ^ (s_interp_seed * 0x9E3779B9u)) * 0x85EBCA6Bu; h ^= h >> 15;
        if (h % 100 < s_interp_pct) return NULL;
    }
    for (int i = 0; i < s_ninterp_only; i++) if (s_interp_only[i] == m68k_addr) return NULL;
    return table_find(m68k_addr);
}

/* the compiled function at m68k_addr, whatever PS_INTERP says (NULL: none) */
static gen_func_t table_find(uint32_t m68k_addr) {
    uint32_t idx = hash_addr(m68k_addr);
    for (uint32_t i = 0; i < FUNC_TABLE_SIZE; i++) {
        uint32_t slot = (idx + i) & (FUNC_TABLE_SIZE - 1);
        if (!s_table[slot].occupied) return NULL;
        if (s_table[slot].m68k_addr == m68k_addr) return s_table[slot].func;
    }
    return NULL;
}

void m68k_interp_run(uint32_t entry);
/* addresses already reported as interpreted (open addressing) */
static uint32_t s_seen[4096];
static bool interp_seen(uint32_t a) {
    uint32_t h = (a * 2654435761u) >> 20;
    for (int i = 0; i < 4096; i++) {
        uint32_t k = (h + (uint32_t)i) & 4095;
        if (s_seen[k] == a + 1) return true;
        if (!s_seen[k]) { s_seen[k] = a + 1; return false; }
    }
    return true;
}

static int s_miss_count = 0;
static uint32_t s_last_miss = 0;
static int s_call_depth = 0;
static int s_max_depth = 0;
static uint32_t s_stack[512];  /* shadow call stack of M68K addresses */

void func_table_dump_stack(FILE *f) {
    fprintf(f, "call stack (%d):", s_call_depth);
    for (int i = 0; i < s_call_depth && i < 512; i++) fprintf(f, " $%06X", s_stack[i]);
    fprintf(f, "\n");
}

static uint32_t s_tail;  /* pending tail call, 0 = none */

void func_table_tail(uint32_t m68k_addr) {
    s_tail = m68k_addr;
}

/* Return address each frame's JSR pushed (0: entered some other way) */
static uint32_t s_expect[512];
static uint8_t s_from_interp[512];      /* the frame was called by the interpreter (interp_jsr) */
static int s_unwind = -1;       /* frame an RTS returned into directly, -1 = none */
uint32_t g_func_resume_pc = 0;

bool func_table_jsr(uint32_t m68k_addr, uint32_t ret) {
    uint32_t sp = g_m68k.a[7] - 4;
    g_m68k.a[7] = sp;
    bus_write32(sp, ret);
    bool ok = func_table_call_expecting(m68k_addr, ret);
    /* A missing callee never executed its RTS: drop the return address we
     * pushed, or every miss leaks 4 bytes and later RTSes pop stale PCs. */
    if (!ok && g_m68k.a[7] == sp && (bus_read32(sp) & 0xFFFFFF) == ret)
        g_m68k.a[7] = sp + 4;
    /* Unwinding to an outer frame: every frame deeper than it returns now. */
    if (s_unwind >= 0) {
        if (s_call_depth > s_unwind) return false;
        s_unwind = -1;
    }
    return true;
}

/* An RTE at the frame of the exception being handled ends the handler (m68k.c), from whatever
 * depth it runs: a routine the handler called can drop its return address and branch to the
 * handler's exit (SH1's VBlank on a lag frame). Every frame deeper than the handler's C
 * caller (depth d) returns, as for an RTS into an outer frame. */
int func_table_depth(void) { return s_call_depth; }
void func_table_unwind_to(int d) { if (s_call_depth > d + 1) s_unwind = d; }
void func_table_unwind_done(int d) { if (s_unwind == d) s_unwind = -1; }

/* An RTS to an address no C frame expects and no function starts at (the code dropped return
 * addresses its interpreted callers pushed): recorded here; the interpreter, whose own pending
 * returns are not C frames, resumes there at the matching level (m68k_interp.c). */
uint32_t g_rts_orphan;
int g_jsr_from_interp;
static int s_resume_root;          /* func_table_resume is running: frame 0 has no C caller */
static uint32_t s_root_ret;        /* where frame 0 returned to */
/* innermost frame below d that the interpreter called, not crossing an interrupt or
 * exception frame (entered without a JSR): -1 if none */
static int outer_interp_frame(int d) {
    for (int k = d - 1; k >= 0; k--) {
        if (k >= 512) continue;
        if (s_from_interp[k]) return k;
        if (!s_expect[k]) return -1;
    }
    return -1;
}
void func_table_rts(void) {
    /* RTS pops the real return address. If it isn't the one this frame's
     * JSR pushed, the code arranged its own continuation (PEA + BRA,
     * push + JMP, a rewritten return address): go there, in this frame,
     * rather than back to the C caller. */
    uint32_t r = bus_read32(g_m68k.a[7]) & 0xFFFFFF;
    g_m68k.a[7] += 4;
    int d = s_call_depth - 1, k_interp;
    if (d == 0 && s_resume_root) s_root_ret = r;    /* func_table_resume: continue there */
    uint32_t e = (d >= 0 && d < 512) ? s_expect[d] : 0;
    if (e && r != e) {
        /* Returned straight to an outer frame's continuation (the code
         * dropped its caller's return address): unwind the C frames above. */
        for (int k = d - 1; k >= 0; k--) {
            if (k < 512 && s_expect[k] == r) { s_unwind = k; return; }
        }
        /* a function the table knows, compiled or (PS_INTERP) interpreted: the trampoline runs it */
        if (func_table_lookup(r) || table_find(r)) s_tail = r;
        else if (d < 512 && s_from_interp[d]) g_rts_orphan = r;   /* the interpreter resumes there */
        else if ((k_interp = outer_interp_frame(d)) >= 0) {
            /* returned past compiled frames into a level the interpreter runs (its pending
             * calls are not C frames): those frames return, the interpreter resumes at r */
            s_unwind = k_interp; g_rts_orphan = r;
        } else {
            /* Mid-function return points (a task switcher returning into
             * another task's saved PC) aren't entry points: fall back to a
             * plain return, which is what the C call structure implies. */
            static int s_warned;
            if (s_warned++ < 10) {
                fprintf(stderr, "func_table: RTS to $%06X (expected $%06X, SP=$%08X) has no function; returning, ",
                        r, e, g_m68k.a[7] - 4);
                func_table_dump_stack(stderr);
            }
        }
    }
}

#include <setjmp.h>
jmp_buf *ps_state_jmpbuf(void);
bool ps_state_pending(void), ps_state_boot_load(void);
void ps_state_resume_loaded(void);
bool func_table_call(uint32_t m68k_addr) {
    static int s_root_entered;
    if (s_call_depth == 0 && !s_root_entered) {
        /* the root of the C stack (main's power-on call): a machine state restored in place
         * (ps_state.c) unwinds to here and the game resumes from it */
        s_root_entered = 1;
        ps_state_boot_load();
        if (setjmp(*ps_state_jmpbuf())) { ps_state_resume_loaded(); return true; }
        if (ps_state_pending()) { ps_state_resume_loaded(); return true; }
    }
    return func_table_call_expecting(m68k_addr, 0);
}
/* a restored state starts with no frames (ps_state.c) */
void func_table_reset_frames(void) {
    s_call_depth = 0; s_tail = 0; s_unwind = -1; g_rts_orphan = 0; g_jsr_from_interp = 0; s_resume_root = 0;
}

bool func_table_call_expecting(uint32_t m68k_addr, uint32_t ret) {
    s_call_depth++;
    if (s_call_depth <= 512) { s_expect[s_call_depth - 1] = ret; s_from_interp[s_call_depth - 1] = (uint8_t)g_jsr_from_interp; }
    g_jsr_from_interp = 0;                  /* only the frame the interpreter called */
    if (s_call_depth > s_max_depth) {
        s_max_depth = s_call_depth;
        if (s_max_depth <= 50 || (s_max_depth % 100 == 0)) {
            fprintf(stderr, "[depth] new max call depth: %d (calling $%06X)\n", s_max_depth, m68k_addr);
        }
    }
    if (s_call_depth > 500) {
        fprintf(stderr, "[depth] ABORT: call depth %d at $%06X — likely infinite recursion\n", s_call_depth, m68k_addr);
        s_call_depth--;
        return false;
    }

    /* Trampoline: a function that ends in a tail jump (JMP, BRA or
     * fall-through into another function) records it with
     * func_table_tail() and returns, and the jump runs here in the same
     * frame. M68K code loops through such jumps freely; as nested C calls
     * they would grow the native stack every iteration. */
    bool found = true;
    do {
        s_tail = 0;
        gen_func_t fn = func_table_lookup(m68k_addr);
        /* RAM trampolines: games copy `JMP abs` stubs into work RAM and call them
         * (e.g. `jsr $FF84.w`). Follow the stub to the ROM function it targets. */
        for (int hops = 0; !fn && hops < 4; hops++) {
            uint32_t ram = m68k_addr;
            if (ram >= 0x8000 && ram <= 0xFFFF) ram |= 0xFF0000;   /* truncated abs.w */
            if (ram < 0xE00000) break;
            uint16_t op = bus_read16(ram);
            if (op == 0x4EF9) { m68k_addr = bus_read32(ram + 2) & 0xFFFFFF; M68K_PRE(); CYC(12 * 7); }   /* JMP abs.l */
            else if (op == 0x4EF8) { m68k_addr = (uint32_t)(int32_t)(int16_t)bus_read16(ram + 2) & 0xFFFFFF; M68K_PRE(); CYC(10 * 7); } /* JMP abs.w */
            else break;
            fn = func_table_lookup(m68k_addr);
        }
        if (!fn) {
            /* not compiled: the interpreter runs it (m68k_interp.c), reported once per
             * address so the next generation compiles it in */
            extern uint32_t g_m68k_rte_target;
            if (m68k_addr == g_m68k_rte_target) {
                /* an RTE to code an interrupt left mid-loop: any instruction can be one; not a
                 * missing function, so not reported */
            } else if (!s_force_interp && !s_interp_pct && !interp_seen(m68k_addr)) {
                fprintf(stderr, "func_table: no function at $%06X (SP=$%08X), interpreting; ", m68k_addr, g_m68k.a[7]);
                func_table_dump_stack(stderr);
                static FILE *log; static int log_init;
                if (!log_init) { log_init = 1; const char *p = getenv("PS_INTERP_LOG"); if (p && *p) log = fopen(p, "a"); }
                if (log) { fprintf(log, "0x%06X\n", m68k_addr); fflush(log); }
            }
            s_stack[s_call_depth - 1] = m68k_addr;
            m68k_interp_run(m68k_addr);
            m68k_addr = s_tail;
            continue;
        }
        s_stack[s_call_depth - 1] = m68k_addr;
        if (s_ft_init <= 0 || s_ftrace) ftrace(m68k_addr);
        fn();
        m68k_addr = s_tail;
    } while (m68k_addr);

    s_call_depth--;
    return found;
}

static void ftrace(uint32_t a)
{
    if (!s_ft_init) {
        s_ft_init = -1;
        const char *p = getenv("PS_FTRACE");
        if (!p || !*p) { s_ft_init = 1; return; }
        s_ftrace = fopen(p, "w");
        if (getenv("PS_FTRACE_FROM")) s_ft_from = atol(getenv("PS_FTRACE_FROM"));
        if (getenv("PS_FTRACE_TO")) s_ft_to = atol(getenv("PS_FTRACE_TO"));
        char path[1024]; snprintf(path, sizeof path, "%s.funcs", p);
        FILE *f = fopen(path, "w");
        if (f) { for (int i = 0; i < FUNC_TABLE_SIZE; i++) if (s_table[i].occupied) fprintf(f, "%06X\n", s_table[i].m68k_addr); fclose(f); }
        s_ft_init = 1;
    }
    if (s_ftrace && ps_frames >= s_ft_from && (s_ft_to < 0 || ps_frames <= s_ft_to))
        fprintf(s_ftrace, "%ld %u %06X\n", ps_frames, g_cyc, a);
}

#ifdef PS_ITRACE
void ps_itrace(uint32_t pc)
{
    static FILE *f; static int init; static long n; static long from, to = -1;
    if (!init) {
        init = 1; const char *p = getenv("PS_ITRACE");
        if (p && *p) { f = fopen(p, "w"); if (f) setvbuf(f, NULL, _IONBF, 0); }
        if (getenv("PS_FTRACE_FROM")) from = atol(getenv("PS_FTRACE_FROM"));
        if (getenv("PS_FTRACE_TO")) to = atol(getenv("PS_FTRACE_TO"));
    }
    if (f && ps_frames >= from && (to < 0 || ps_frames <= to) && n++ < 60000000)
        fprintf(f, "%06X %u R%d %X %X %X %X %X %X %X %X %X %X %X %X %X %X %X I%u\n", pc, g_cyc, (int)(g_refresh - g_cyc), g_m68k.d[0], g_m68k.d[1], g_m68k.d[2],
                g_m68k.d[3], g_m68k.d[4], g_m68k.d[5], g_m68k.d[6], g_m68k.d[7],
                g_m68k.a[0], g_m68k.a[1], g_m68k.a[2], g_m68k.a[3], g_m68k.a[4], g_m68k.a[5], g_m68k.a[6], (unsigned)g_m68k.int_mask);
}
#endif

/* Resume the 68000 at pc with no C frames under it (a restored machine state, ps_state.c):
 * the code runs as the interpreter's own level - every return address is in emulated RAM -
 * and each time the outermost frame returns, execution continues where it returned to.
 * Calls from there run compiled code as usual. Never returns (the game runs until exit). */
void func_table_resume(uint32_t pc) {
    s_resume_root = 1;
    for (;;) {
        s_root_ret = 0; g_rts_orphan = 0; s_unwind = -1;
        g_jsr_from_interp = 1;                 /* frame 0: returns past it go on at its level */
        func_table_call_expecting(pc, 0);
        s_unwind = -1;
        if (g_rts_orphan) { pc = g_rts_orphan; g_rts_orphan = 0; }
        else if (s_root_ret) pc = s_root_ret;
        else { fprintf(stderr, "func_table_resume: outermost frame left without a return address, stopping\n"); return; }
    }
}
