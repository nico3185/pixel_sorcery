#ifndef GENRECOMP_M68K_H
#define GENRECOMP_M68K_H

/*
 * Motorola 68000 CPU register state for recompiled code.
 *
 * This struct is what your recompiled functions read/write directly.
 * It is NOT connected to Genesis Plus GX's M68K emulation — the
 * recompiled native code IS the CPU. This struct just holds the
 * register state that the original 68000 code would have used.
 */

#include <stdint.h>
#include <stdbool.h>

typedef struct M68kContext {
    /* Data registers D0-D7 */
    uint32_t d[8];

    /* Address registers A0-A7 (A7 aliases current stack pointer) */
    uint32_t a[8];

    /* Stack pointers (A7 switches between these based on supervisor mode) */
    uint32_t usp;   /* User Stack Pointer */
    uint32_t ssp;   /* Supervisor Stack Pointer */

    /* Program counter (for debugging/exceptions — recompiled code uses native flow) */
    uint32_t pc;

    /* Condition Code Register flags (individual bools for fast branching) */
    bool flag_C;    /* Carry */
    bool flag_V;    /* Overflow */
    bool flag_Z;    /* Zero */
    bool flag_N;    /* Negative */
    bool flag_X;    /* Extend (like carry, but sticky) */

    /* Supervisor state */
    bool flag_S;    /* Supervisor mode */
    uint8_t int_mask; /* Interrupt priority mask (3 bits, 0-7) */

    /* Cycle tracking for M68K/Z80/VDP synchronization */
    int64_t cycles;
    int64_t target_cycles;
} M68kContext;

extern M68kContext g_m68k;

/* ================================================================
 * CCR / SR access helpers
 * ================================================================ */

/* CCR = lower 5 bits of SR: X N Z V C */
static inline uint8_t m68k_get_ccr(void) {
    uint8_t ccr = 0;
    if (g_m68k.flag_C) ccr |= 0x01;
    if (g_m68k.flag_V) ccr |= 0x02;
    if (g_m68k.flag_Z) ccr |= 0x04;
    if (g_m68k.flag_N) ccr |= 0x08;
    if (g_m68k.flag_X) ccr |= 0x10;
    return ccr;
}

static inline void m68k_set_ccr(uint8_t ccr) {
    g_m68k.flag_C = (ccr & 0x01) != 0;
    g_m68k.flag_V = (ccr & 0x02) != 0;
    g_m68k.flag_Z = (ccr & 0x04) != 0;
    g_m68k.flag_N = (ccr & 0x08) != 0;
    g_m68k.flag_X = (ccr & 0x10) != 0;
}

/* SR = supervisor bits (high byte) + CCR (low byte) */
static inline uint16_t m68k_get_sr(void) {
    uint16_t sr = (uint16_t)m68k_get_ccr();
    if (g_m68k.flag_S) sr |= 0x2000;
    sr |= (uint16_t)(g_m68k.int_mask & 0x07) << 8;
    return sr;
}

extern uint8_t g_m68k_irq_pending;
extern unsigned int g_cyc, g_cyc_event;
static inline void m68k_set_sr(uint16_t sr) {
    m68k_set_ccr((uint8_t)(sr & 0xFF));
    bool s_new = (sr & 0x2000) != 0;
    if (s_new != g_m68k.flag_S) {             /* A7 is the other stack pointer from now on */
        if (s_new) { g_m68k.usp = g_m68k.a[7]; g_m68k.a[7] = g_m68k.ssp; }
        else { g_m68k.ssp = g_m68k.a[7]; g_m68k.a[7] = g_m68k.usp; }
    }
    g_m68k.flag_S = s_new;
    g_m68k.int_mask = (uint8_t)((sr >> 8) & 0x07);
    /* a held-back interrupt is taken once unmasked; its E-clock phase is that of the
     * SR-writing instruction's start (the CPU takes it inside that instruction) */
    if (g_m68k_irq_pending > g_m68k.int_mask) {
        extern unsigned g_irq_phase_at; extern bool g_irq_phase_set;
        if (!g_irq_phase_set) { g_irq_phase_at = g_cyc; g_irq_phase_set = true; }
        g_cyc_event = g_cyc;
    }
}

/* ================================================================
 * Flag update helpers (N and Z for each operand size)
 * ================================================================ */

static inline void m68k_update_nz8(uint8_t val) {
    g_m68k.flag_N = (val & 0x80) != 0;
    g_m68k.flag_Z = (val == 0);
}

static inline void m68k_update_nz16(uint16_t val) {
    g_m68k.flag_N = (val & 0x8000) != 0;
    g_m68k.flag_Z = (val == 0);
}

static inline void m68k_update_nz32(uint32_t val) {
    g_m68k.flag_N = (val & 0x80000000u) != 0;
    g_m68k.flag_Z = (val == 0);
}

/* ================================================================
 * Arithmetic macros
 *
 * These update flags exactly as the real M68K does.
 * dst/src are evaluated once via do{} wrapper.
 * Result is stored back into dst.
 *
 * Internal temps are prefixed _m68_ rather than plain _s/_d/_r/_res/_x/_q/_rem: the
 * generator's own locals routinely use exactly those names (e.g. M68K_ABCD(_t, _s) for a
 * predecrement BCD chain), and `uint8_t _s = (uint8_t)(src)` with src == the caller's _s
 * self-initializes from its own uninitialized value instead of the caller's — silently
 * corrupting the operation. Found via a Windows/MSVC build (warning C4700), but it's a
 * plain C hygiene bug that can misbehave under any compiler.
 * ================================================================ */

/* --- ADD (8/16/32) --- */
#define M68K_ADD8(dst, src) do { \
    uint8_t _m68_s = (uint8_t)(src); \
    uint8_t _m68_d = (uint8_t)(dst); \
    uint16_t _m68_r = (uint16_t)_m68_d + (uint16_t)_m68_s; \
    uint8_t _m68_res = (uint8_t)_m68_r; \
    g_m68k.flag_C = g_m68k.flag_X = (_m68_r > 0xFF); \
    g_m68k.flag_V = ((_m68_d ^ _m68_res) & (_m68_s ^ _m68_res) & 0x80) != 0; \
    m68k_update_nz8(_m68_res); \
    (dst) = ((dst) & ~0xFFu) | (uint8_t)(_m68_res); \
} while(0)

#define M68K_ADD16(dst, src) do { \
    uint16_t _m68_s = (uint16_t)(src); \
    uint16_t _m68_d = (uint16_t)(dst); \
    uint32_t _m68_r = (uint32_t)_m68_d + (uint32_t)_m68_s; \
    uint16_t _m68_res = (uint16_t)_m68_r; \
    g_m68k.flag_C = g_m68k.flag_X = (_m68_r > 0xFFFF); \
    g_m68k.flag_V = ((_m68_d ^ _m68_res) & (_m68_s ^ _m68_res) & 0x8000) != 0; \
    m68k_update_nz16(_m68_res); \
    (dst) = ((dst) & ~0xFFFFu) | (uint16_t)(_m68_res); \
} while(0)

#define M68K_ADD32(dst, src) do { \
    uint32_t _m68_s = (uint32_t)(src); \
    uint32_t _m68_d = (uint32_t)(dst); \
    uint64_t _m68_r = (uint64_t)_m68_d + (uint64_t)_m68_s; \
    uint32_t _m68_res = (uint32_t)_m68_r; \
    g_m68k.flag_C = g_m68k.flag_X = (_m68_r > 0xFFFFFFFFu); \
    g_m68k.flag_V = ((_m68_d ^ _m68_res) & (_m68_s ^ _m68_res) & 0x80000000u) != 0; \
    m68k_update_nz32(_m68_res); \
    (dst) = _m68_res; \
} while(0)

/* --- ADDX (add with extend) --- */
#define M68K_ADDX8(dst, src) do { \
    uint8_t _m68_s = (uint8_t)(src); \
    uint8_t _m68_d = (uint8_t)(dst); \
    uint16_t _m68_r = (uint16_t)_m68_d + (uint16_t)_m68_s + (g_m68k.flag_X ? 1u : 0u); \
    uint8_t _m68_res = (uint8_t)_m68_r; \
    g_m68k.flag_C = g_m68k.flag_X = (_m68_r > 0xFF); \
    g_m68k.flag_V = ((_m68_d ^ _m68_res) & (_m68_s ^ _m68_res) & 0x80) != 0; \
    g_m68k.flag_N = (_m68_res & 0x80) != 0; \
    if (_m68_res != 0) g_m68k.flag_Z = false; \
    (dst) = ((dst) & ~0xFFu) | (uint8_t)(_m68_res); \
} while(0)

#define M68K_ADDX16(dst, src) do { \
    uint16_t _m68_s = (uint16_t)(src); \
    uint16_t _m68_d = (uint16_t)(dst); \
    uint32_t _m68_r = (uint32_t)_m68_d + (uint32_t)_m68_s + (g_m68k.flag_X ? 1u : 0u); \
    uint16_t _m68_res = (uint16_t)_m68_r; \
    g_m68k.flag_C = g_m68k.flag_X = (_m68_r > 0xFFFF); \
    g_m68k.flag_V = ((_m68_d ^ _m68_res) & (_m68_s ^ _m68_res) & 0x8000) != 0; \
    g_m68k.flag_N = (_m68_res & 0x8000) != 0; \
    if (_m68_res != 0) g_m68k.flag_Z = false; \
    (dst) = ((dst) & ~0xFFFFu) | (uint16_t)(_m68_res); \
} while(0)

#define M68K_ADDX32(dst, src) do { \
    uint32_t _m68_s = (uint32_t)(src); \
    uint32_t _m68_d = (uint32_t)(dst); \
    uint64_t _m68_r = (uint64_t)_m68_d + (uint64_t)_m68_s + (g_m68k.flag_X ? 1u : 0u); \
    uint32_t _m68_res = (uint32_t)_m68_r; \
    g_m68k.flag_C = g_m68k.flag_X = (_m68_r > 0xFFFFFFFFu); \
    g_m68k.flag_V = ((_m68_d ^ _m68_res) & (_m68_s ^ _m68_res) & 0x80000000u) != 0; \
    g_m68k.flag_N = (_m68_res & 0x80000000u) != 0; \
    if (_m68_res != 0) g_m68k.flag_Z = false; \
    (dst) = _m68_res; \
} while(0)

/* --- SUB (8/16/32) --- */
#define M68K_SUB8(dst, src) do { \
    uint8_t _m68_s = (uint8_t)(src); \
    uint8_t _m68_d = (uint8_t)(dst); \
    uint16_t _m68_r = (uint16_t)_m68_d - (uint16_t)_m68_s; \
    uint8_t _m68_res = (uint8_t)_m68_r; \
    g_m68k.flag_C = g_m68k.flag_X = (_m68_d < _m68_s); \
    g_m68k.flag_V = ((_m68_d ^ _m68_s) & (_m68_d ^ _m68_res) & 0x80) != 0; \
    m68k_update_nz8(_m68_res); \
    (dst) = ((dst) & ~0xFFu) | (uint8_t)(_m68_res); \
} while(0)

#define M68K_SUB16(dst, src) do { \
    uint16_t _m68_s = (uint16_t)(src); \
    uint16_t _m68_d = (uint16_t)(dst); \
    uint32_t _m68_r = (uint32_t)_m68_d - (uint32_t)_m68_s; \
    uint16_t _m68_res = (uint16_t)_m68_r; \
    g_m68k.flag_C = g_m68k.flag_X = (_m68_d < _m68_s); \
    g_m68k.flag_V = ((_m68_d ^ _m68_s) & (_m68_d ^ _m68_res) & 0x8000) != 0; \
    m68k_update_nz16(_m68_res); \
    (dst) = ((dst) & ~0xFFFFu) | (uint16_t)(_m68_res); \
} while(0)

#define M68K_SUB32(dst, src) do { \
    uint32_t _m68_s = (uint32_t)(src); \
    uint32_t _m68_d = (uint32_t)(dst); \
    uint64_t _m68_r = (uint64_t)_m68_d - (uint64_t)_m68_s; \
    uint32_t _m68_res = (uint32_t)_m68_r; \
    g_m68k.flag_C = g_m68k.flag_X = (_m68_d < _m68_s); \
    g_m68k.flag_V = ((_m68_d ^ _m68_s) & (_m68_d ^ _m68_res) & 0x80000000u) != 0; \
    m68k_update_nz32(_m68_res); \
    (dst) = _m68_res; \
} while(0)

/* --- SUBX (subtract with extend) --- */
#define M68K_SUBX8(dst, src) do { \
    uint8_t _m68_s = (uint8_t)(src); \
    uint8_t _m68_d = (uint8_t)(dst); \
    uint16_t _m68_x = (g_m68k.flag_X ? 1u : 0u); \
    uint16_t _m68_r = (uint16_t)_m68_d - (uint16_t)_m68_s - _m68_x; \
    uint8_t _m68_res = (uint8_t)_m68_r; \
    g_m68k.flag_C = g_m68k.flag_X = ((uint16_t)_m68_s + _m68_x > (uint16_t)_m68_d); \
    g_m68k.flag_V = ((_m68_d ^ _m68_s) & (_m68_d ^ _m68_res) & 0x80) != 0; \
    g_m68k.flag_N = (_m68_res & 0x80) != 0; \
    if (_m68_res != 0) g_m68k.flag_Z = false; \
    (dst) = ((dst) & ~0xFFu) | (uint8_t)(_m68_res); \
} while(0)

#define M68K_SUBX16(dst, src) do { \
    uint16_t _m68_s = (uint16_t)(src); \
    uint16_t _m68_d = (uint16_t)(dst); \
    uint32_t _m68_x = (g_m68k.flag_X ? 1u : 0u); \
    uint32_t _m68_r = (uint32_t)_m68_d - (uint32_t)_m68_s - _m68_x; \
    uint16_t _m68_res = (uint16_t)_m68_r; \
    g_m68k.flag_C = g_m68k.flag_X = ((uint32_t)_m68_s + _m68_x > (uint32_t)_m68_d); \
    g_m68k.flag_V = ((_m68_d ^ _m68_s) & (_m68_d ^ _m68_res) & 0x8000) != 0; \
    g_m68k.flag_N = (_m68_res & 0x8000) != 0; \
    if (_m68_res != 0) g_m68k.flag_Z = false; \
    (dst) = ((dst) & ~0xFFFFu) | (uint16_t)(_m68_res); \
} while(0)

#define M68K_SUBX32(dst, src) do { \
    uint32_t _m68_s = (uint32_t)(src); \
    uint32_t _m68_d = (uint32_t)(dst); \
    uint64_t _m68_x = (g_m68k.flag_X ? 1u : 0u); \
    uint64_t _m68_r = (uint64_t)_m68_d - (uint64_t)_m68_s - _m68_x; \
    uint32_t _m68_res = (uint32_t)_m68_r; \
    g_m68k.flag_C = g_m68k.flag_X = ((uint64_t)_m68_s + _m68_x > (uint64_t)_m68_d); \
    g_m68k.flag_V = ((_m68_d ^ _m68_s) & (_m68_d ^ _m68_res) & 0x80000000u) != 0; \
    g_m68k.flag_N = (_m68_res & 0x80000000u) != 0; \
    if (_m68_res != 0) g_m68k.flag_Z = false; \
    (dst) = _m68_res; \
} while(0)

/* --- CMP (8/16/32) — like SUB but doesn't store result or update X --- */
#define M68K_CMP8(dst, src) do { \
    uint8_t _m68_s = (uint8_t)(src); \
    uint8_t _m68_d = (uint8_t)(dst); \
    uint8_t _m68_res = _m68_d - _m68_s; \
    g_m68k.flag_C = (_m68_d < _m68_s); \
    g_m68k.flag_V = ((_m68_d ^ _m68_s) & (_m68_d ^ _m68_res) & 0x80) != 0; \
    m68k_update_nz8(_m68_res); \
} while(0)

#define M68K_CMP16(dst, src) do { \
    uint16_t _m68_s = (uint16_t)(src); \
    uint16_t _m68_d = (uint16_t)(dst); \
    uint16_t _m68_res = _m68_d - _m68_s; \
    g_m68k.flag_C = (_m68_d < _m68_s); \
    g_m68k.flag_V = ((_m68_d ^ _m68_s) & (_m68_d ^ _m68_res) & 0x8000) != 0; \
    m68k_update_nz16(_m68_res); \
} while(0)

#define M68K_CMP32(dst, src) do { \
    uint32_t _m68_s = (uint32_t)(src); \
    uint32_t _m68_d = (uint32_t)(dst); \
    uint32_t _m68_res = _m68_d - _m68_s; \
    g_m68k.flag_C = (_m68_d < _m68_s); \
    g_m68k.flag_V = ((_m68_d ^ _m68_s) & (_m68_d ^ _m68_res) & 0x80000000u) != 0; \
    m68k_update_nz32(_m68_res); \
} while(0)

/* --- NEG (8/16/32) — negate: 0 - dst --- */
#define M68K_NEG8(dst) do { \
    uint8_t _m68_d = (uint8_t)(dst); \
    uint8_t _m68_res = (uint8_t)(0 - _m68_d); \
    g_m68k.flag_C = g_m68k.flag_X = (_m68_d != 0); \
    g_m68k.flag_V = (_m68_d == 0x80); \
    m68k_update_nz8(_m68_res); \
    (dst) = ((dst) & ~0xFFu) | (uint8_t)(_m68_res); \
} while(0)

#define M68K_NEG16(dst) do { \
    uint16_t _m68_d = (uint16_t)(dst); \
    uint16_t _m68_res = (uint16_t)(0 - _m68_d); \
    g_m68k.flag_C = g_m68k.flag_X = (_m68_d != 0); \
    g_m68k.flag_V = (_m68_d == 0x8000); \
    m68k_update_nz16(_m68_res); \
    (dst) = ((dst) & ~0xFFFFu) | (uint16_t)(_m68_res); \
} while(0)

#define M68K_NEG32(dst) do { \
    uint32_t _m68_d = (uint32_t)(dst); \
    uint32_t _m68_res = (uint32_t)(0 - _m68_d); \
    g_m68k.flag_C = g_m68k.flag_X = (_m68_d != 0); \
    g_m68k.flag_V = (_m68_d == 0x80000000u); \
    m68k_update_nz32(_m68_res); \
    (dst) = _m68_res; \
} while(0)

/* --- NEGX (negate with extend) --- */
#define M68K_NEGX8(dst) do { \
    uint8_t _m68_d = (uint8_t)(dst); \
    uint8_t _m68_x = (g_m68k.flag_X ? 1u : 0u); \
    uint8_t _m68_res = (uint8_t)(0 - _m68_d - _m68_x); \
    g_m68k.flag_C = g_m68k.flag_X = (_m68_d != 0 || _m68_x != 0); \
    g_m68k.flag_V = (_m68_res & 0x80) && (_m68_d & 0x80); \
    g_m68k.flag_N = (_m68_res & 0x80) != 0; \
    if (_m68_res != 0) g_m68k.flag_Z = false; \
    (dst) = ((dst) & ~0xFFu) | (uint8_t)(_m68_res); \
} while(0)

#define M68K_NEGX16(dst) do { \
    uint16_t _m68_d = (uint16_t)(dst); \
    uint16_t _m68_x = (g_m68k.flag_X ? 1u : 0u); \
    uint16_t _m68_res = (uint16_t)(0 - _m68_d - _m68_x); \
    g_m68k.flag_C = g_m68k.flag_X = (_m68_d != 0 || _m68_x != 0); \
    g_m68k.flag_V = (_m68_res & 0x8000) && (_m68_d & 0x8000); \
    g_m68k.flag_N = (_m68_res & 0x8000) != 0; \
    if (_m68_res != 0) g_m68k.flag_Z = false; \
    (dst) = ((dst) & ~0xFFFFu) | (uint16_t)(_m68_res); \
} while(0)

#define M68K_NEGX32(dst) do { \
    uint32_t _m68_d = (uint32_t)(dst); \
    uint32_t _m68_x = (g_m68k.flag_X ? 1u : 0u); \
    uint32_t _m68_res = (uint32_t)(0 - _m68_d - _m68_x); \
    g_m68k.flag_C = g_m68k.flag_X = (_m68_d != 0 || _m68_x != 0); \
    g_m68k.flag_V = (_m68_res & 0x80000000u) && (_m68_d & 0x80000000u); \
    g_m68k.flag_N = (_m68_res & 0x80000000u) != 0; \
    if (_m68_res != 0) g_m68k.flag_Z = false; \
    (dst) = _m68_res; \
} while(0)

/* --- Cycle model (master clocks; one 68000 cycle = 7) ---
 * Generated code charges each instruction's documented 68000 cost after its body
 * (control flow: before the transfer), like the CPU's bus timing. M68K_PRE applies the
 * Mega Drive bus-refresh delay (2 cycles every 128). Crossing g_cyc_event (next scanline
 * or interrupt) runs bus_cycle_event() at that instruction boundary. */
extern unsigned int g_cyc, g_cyc_event, g_refresh;
int ps_cheat(int n);                      /* runtime cheat switches (recomp.json "cheats", settings menu) */
extern unsigned int g_cyc_skip;
extern uint32_t g_m68k_pf;         /* prefetch address of the instruction doing the current read */
extern unsigned int g_m68k_icost;  /* its base cost (status reads are evaluated at its end) */   /* multi-line 68K DMA: the stalled instruction's own cost is absorbed */
void bus_cycle_event(void);
extern unsigned int g_refresh_hold;   /* instruction run inside the delayed-interrupt window: no refresh check */
#define M68K_PRE() do { if (g_refresh_hold) g_refresh_hold = 0; else if ((int)(g_cyc - g_refresh) >= 0) { g_refresh = g_cyc + 128 * 7; g_cyc += 2 * 7; } } while (0)
/* Instruction trace (build with -DPS_ITRACE): "pc cycle d0..d7" per instruction, same
 * format as the oracle tracer's PS_ITRACE, for instruction-level timing comparison. */
#ifdef PS_ITRACE
void ps_itrace(uint32_t pc);
#define ITRACE(pc) ps_itrace(pc)
#else
#define ITRACE(pc) ((void)0)
#endif
/* Long MOVEM, register-count shifts and DIV hide a bus refresh falling inside them */
#define M68K_SKIPREF() do { if ((int)(g_cyc - g_refresh) >= 0) g_refresh += 128 * 7; } while (0)
#define CYC(n) do { if (g_cyc_skip) g_cyc_skip = 0; else g_cyc += (unsigned)(n); if ((int)(g_cyc - g_cyc_event) >= 0) { g_m68k_ipc = 0; bus_cycle_event(); } } while (0)
/* Interrupt return address: an interrupt taken at an instruction boundary stacks the PC of
 * the next instruction (CYCP's pc; 0 where the generator does not know it: calls, returns,
 * computed jumps). A handler that rewrites it (SH3: a decompressor interrupted by VBlank
 * is made to yield until the next frame) makes execution continue there, as RTE would. */
extern uint32_t g_m68k_ipc, g_m68k_redirect;
void func_table_tail(uint32_t m68k_addr);
bool m68k_rte(uint32_t *pc);
#ifndef M68K_REDIRECTED
#define M68K_REDIRECTED() do { if (g_m68k_redirect) { uint32_t _t = g_m68k_redirect; g_m68k_redirect = 0; func_table_tail(_t); return; } } while (0)
#endif
/* g_m68k_ipc_kind = 1: the event is at a plain instruction boundary (nothing of the next
 * instruction done): the machine state is exactly the 68000's there (ps_state.c snapshots) */
extern uint8_t g_m68k_ipc_kind;
#define CYCP(n, pc) do { if (g_cyc_skip) g_cyc_skip = 0; else g_cyc += (unsigned)(n); if ((int)(g_cyc - g_cyc_event) >= 0) { g_m68k_ipc = (pc); g_m68k_ipc_kind = 1; bus_cycle_event(); g_m68k_ipc = 0; g_m68k_ipc_kind = 0; M68K_REDIRECTED(); } } while (0)
/* CYCJ: a call (JSR/BSR) to `t` (an lvalue): an interrupt taken there returns to the callee, as
 * the 68000 has done the JSR; if the handler rewrote that return address the call goes there
 * instead (same return address on the stack). */
#define CYCJ(n, t) do { if (g_cyc_skip) g_cyc_skip = 0; else g_cyc += (unsigned)(n); if ((int)(g_cyc - g_cyc_event) >= 0) { g_m68k_ipc = (t); bus_cycle_event(); g_m68k_ipc = 0; if (g_m68k_redirect) { (t) = g_m68k_redirect; g_m68k_redirect = 0; } } } while (0)
/* CYCR: an RTS: an interrupt taken there returns to the address on the stack; if the handler
 * rewrote it, the RTS is done (popped) and execution continues at the new address. */
#define CYCR(n) do { if (g_cyc_skip) g_cyc_skip = 0; else g_cyc += (unsigned)(n); if ((int)(g_cyc - g_cyc_event) >= 0) { g_m68k_ipc = bus_read32(g_m68k.a[7]) & 0xFFFFFF; bus_cycle_event(); g_m68k_ipc = 0; if (g_m68k_redirect) { uint32_t _t = g_m68k_redirect; g_m68k_redirect = 0; g_m68k.a[7] += 4; func_table_tail(_t); return; } } } while (0)

static inline unsigned m68k_cyc_mulu(uint16_t s)
{ unsigned c = 38; while (s) { c += (s & 1) * 2; s >>= 1; } return c * 7; }
static inline unsigned m68k_cyc_muls(int16_t s)
{ unsigned t = ((unsigned)(int32_t)s << 1 ^ (unsigned)(int32_t)s) & 0xFFFF, c = 38; while (t) { c += (t & 1) * 2; t >>= 1; } return c * 7; }
static inline unsigned m68k_cyc_divu(uint32_t d, uint16_t s)
{
    if ((d >> 16) >= s) return 10 * 7;                       /* overflow */
    uint32_t sh = (uint32_t)s << 16; unsigned c = 76;
    for (int i = 0; i < 15; i++) {
        if ((int32_t)d < 0) { d <<= 1; d -= sh; }
        else { d <<= 1; c += 4; if (d >= sh) { d -= sh; c -= 2; } }
    }
    return c * 7;
}
static inline unsigned m68k_cyc_divs(int32_t d, int16_t s)
{
    unsigned c = 12 + (d < 0 ? 2 : 0);
    uint32_t ad = d < 0 ? 0u - (uint32_t)d : (uint32_t)d, as = s < 0 ? (uint32_t)(-(int32_t)s) : (uint32_t)s;
    if ((ad >> 16) < as) {
        uint32_t q = ad / as; c += 110;
        if (s >= 0) c = (d >= 0) ? c - 2 : c + 2;
        for (int i = 0; i < 15; i++) { q >>= 1; if (!(q & 1)) c += 2; }
    } else c += 4;
    return c * 7;
}

/* --- MULU / MULS (16x16 -> 32) --- */
#define M68K_MULU(dst, src) do { \
    uint16_t _m68_s = (uint16_t)(src); \
    uint16_t _m68_d = (uint16_t)(dst); \
    uint32_t _m68_res = (uint32_t)_m68_d * (uint32_t)_m68_s; \
    g_cyc += m68k_cyc_mulu(_m68_s); \
    g_m68k.flag_C = false; \
    g_m68k.flag_V = false; \
    m68k_update_nz32(_m68_res); \
    (dst) = _m68_res; \
} while(0)

#define M68K_MULS(dst, src) do { \
    int16_t _m68_s = (int16_t)(src); \
    int16_t _m68_d = (int16_t)(dst); \
    int32_t _m68_res = (int32_t)_m68_d * (int32_t)_m68_s; \
    g_cyc += m68k_cyc_muls(_m68_s); \
    g_m68k.flag_C = false; \
    g_m68k.flag_V = false; \
    m68k_update_nz32((uint32_t)_m68_res); \
    (dst) = (uint32_t)_m68_res; \
} while(0)

/* --- DIVU / DIVS (32 / 16 -> 16q:16r) --- */
/* Result: low 16 = quotient, high 16 = remainder */
#define M68K_DIVU(dst, src) do { \
    uint16_t _m68_s = (uint16_t)(src); \
    uint32_t _m68_d = (uint32_t)(dst); \
    if (_m68_s) { g_cyc += m68k_cyc_divu(_m68_d, _m68_s); \
        if ((_m68_d >> 16) < _m68_s && (int)(g_cyc - g_refresh) >= 0) g_refresh += 128 * 7; } /* DIV hides a refresh */ \
    if (_m68_s == 0) { \
        /* Division by zero — would trigger exception in real hardware */ \
        g_m68k.flag_C = false; \
        break; \
    } \
    uint32_t _m68_q = _m68_d / (uint32_t)_m68_s; \
    uint32_t _m68_rem = _m68_d % (uint32_t)_m68_s; \
    if (_m68_q > 0xFFFF) { \
        /* Overflow */ \
        g_m68k.flag_V = true; \
        g_m68k.flag_C = false; \
    } else { \
        g_m68k.flag_V = false; \
        g_m68k.flag_C = false; \
        uint32_t _m68_res = (_m68_rem << 16) | (_m68_q & 0xFFFF); \
        m68k_update_nz16((uint16_t)_m68_q); \
        (dst) = _m68_res; \
    } \
} while(0)

#define M68K_DIVS(dst, src) do { \
    int16_t _m68_s = (int16_t)(src); \
    int32_t _m68_d = (int32_t)(uint32_t)(dst); \
    if (_m68_s) { g_cyc += m68k_cyc_divs(_m68_d, _m68_s); \
        if ((int)(g_cyc - g_refresh) >= 0) g_refresh += 128 * 7; } /* DIV hides a refresh */ \
    if (_m68_s == 0) { \
        g_m68k.flag_C = false; \
        break; \
    } \
    int32_t _m68_q = _m68_d / (int32_t)_m68_s; \
    int32_t _m68_rem = _m68_d % (int32_t)_m68_s; \
    if (_m68_q > 32767 || _m68_q < -32768) { \
        g_m68k.flag_V = true; \
        g_m68k.flag_C = false; \
    } else { \
        g_m68k.flag_V = false; \
        g_m68k.flag_C = false; \
        uint32_t _m68_res = ((uint16_t)_m68_rem << 16) | ((uint16_t)_m68_q & 0xFFFF); \
        m68k_update_nz16((uint16_t)_m68_q); \
        (dst) = _m68_res; \
    } \
} while(0)

/* --- ABCD / SBCD / NBCD (packed BCD, byte only) ---
 * Decimal add/subtract with extend: binary result, then decimal-adjust
 * the low digit and the byte. X = C = decimal carry/borrow. Z is only
 * cleared (a multi-byte BCD chain stays Z only if every byte was 0). N is
 * undefined on the 68000; set from bit 7 like other byte ops. V likewise. */
/* Internal temps are prefixed _bcd_ (not _s/_d/_t/_x/_r/_c): callers are generated code
 * whose own locals are routinely named _s/_t/_d (e.g. M68K_ABCD(_t, _s) for predecrement
 * BCD chains), and a plain `src`/`dst` placeholder here would be substituted with that
 * same name — so `unsigned _s = (uint8_t)(src), ...` would self-initialize _s from its own
 * (not yet initialized) value instead of the caller's, silently corrupting every BCD op. */
/* ABCD / SBCD / NBCD as the 68000 computes them, invalid BCD digits and the undocumented N and
 * V included (the low digit corrected by 6 when it overflows/borrows, then the whole byte by
 * $A0; V and N from the uncorrected and corrected sums): equal to the SingleStepTests suite and
 * to GPGX's core on every case (engine/tools/isa). Z is only cleared, as on the CPU. */
static inline uint8_t m68k_abcd(uint8_t d, uint8_t s) {
    unsigned x = g_m68k.flag_X, lo = (s & 15u) + (d & 15u) + x, corr = lo > 9 ? 6 : 0;
    unsigned r = (s & 0xF0u) + (d & 0xF0u) + lo, pre = ~r;
    r += corr;
    bool c = r > 0x9F;
    if (c) r -= 0xA0;
    g_m68k.flag_C = g_m68k.flag_X = c;
    g_m68k.flag_V = (pre & r & 0x80) != 0;
    g_m68k.flag_N = (r & 0x80) != 0;
    if (r & 0xFF) g_m68k.flag_Z = false;
    return (uint8_t)r;
}
static inline uint8_t m68k_sbcd(uint8_t d, uint8_t s) {
    unsigned x = g_m68k.flag_X, lo = (d & 15u) - (s & 15u) - x, corr = lo > 15 ? 6 : 0;
    unsigned r = lo + (d & 0xF0u) - (s & 0xF0u), pre = r;
    bool c;
    if (r > 0xFF) { r += 0xA0; c = true; }
    else c = r < corr;
    r = (r - corr) & 0xFF;
    g_m68k.flag_C = g_m68k.flag_X = c;
    g_m68k.flag_V = (pre & ~r & 0x80) != 0;
    g_m68k.flag_N = (r & 0x80) != 0;
    if (r) g_m68k.flag_Z = false;
    return (uint8_t)r;
}
static inline uint8_t m68k_nbcd(uint8_t d) {
    unsigned r = 0u - d - (unsigned)g_m68k.flag_X;
    if (!r) { g_m68k.flag_V = g_m68k.flag_C = g_m68k.flag_X = g_m68k.flag_N = false; return d; }
    unsigned pre = r;
    if (((r | d) & 0x0F) == 0) r = (r & 0xF0) + 6;
    r = (r + 0x9A) & 0xFF;
    g_m68k.flag_V = (pre & ~r & 0x80) != 0;
    g_m68k.flag_C = g_m68k.flag_X = true;
    g_m68k.flag_N = (r & 0x80) != 0;
    if (r) g_m68k.flag_Z = false;
    return (uint8_t)r;
}
#define M68K_ABCD(dst, src) do { (dst) = ((dst) & ~0xFFu) | m68k_abcd((uint8_t)(dst), (uint8_t)(src)); } while (0)
#define M68K_SBCD(dst, src) do { (dst) = ((dst) & ~0xFFu) | m68k_sbcd((uint8_t)(dst), (uint8_t)(src)); } while (0)
#define M68K_NBCD(dst) do { (dst) = ((dst) & ~0xFFu) | m68k_nbcd((uint8_t)(dst)); } while (0)

/* ================================================================
 * Condition code macros (all 16 M68K conditions)
 * ================================================================ */

#define M68K_CC_T   (1)                                             /* True (always) */
#define M68K_CC_F   (0)                                             /* False (never) */
#define M68K_CC_HI  (!g_m68k.flag_C && !g_m68k.flag_Z)             /* High (unsigned >) */
#define M68K_CC_LS  (g_m68k.flag_C || g_m68k.flag_Z)               /* Low or Same */
#define M68K_CC_CC  (!g_m68k.flag_C)                                /* Carry Clear */
#define M68K_CC_CS  (g_m68k.flag_C)                                 /* Carry Set */
#define M68K_CC_NE  (!g_m68k.flag_Z)                                /* Not Equal */
#define M68K_CC_EQ  (g_m68k.flag_Z)                                 /* Equal */
#define M68K_CC_VC  (!g_m68k.flag_V)                                /* Overflow Clear */
#define M68K_CC_VS  (g_m68k.flag_V)                                 /* Overflow Set */
#define M68K_CC_PL  (!g_m68k.flag_N)                                /* Plus */
#define M68K_CC_MI  (g_m68k.flag_N)                                 /* Minus */
#define M68K_CC_GE  (g_m68k.flag_N == g_m68k.flag_V)               /* Greater or Equal (signed) */
#define M68K_CC_LT  (g_m68k.flag_N != g_m68k.flag_V)               /* Less Than (signed) */
#define M68K_CC_GT  (!g_m68k.flag_Z && (g_m68k.flag_N == g_m68k.flag_V))  /* Greater Than (signed) */
#define M68K_CC_LE  (g_m68k.flag_Z || (g_m68k.flag_N != g_m68k.flag_V))   /* Less or Equal (signed) */

/* ================================================================
 * Logical operation macros (AND, OR, EOR, NOT)
 * ================================================================ */

#define M68K_AND8(dst, src) do { \
    uint8_t _res = (uint8_t)(dst) & (uint8_t)(src); \
    g_m68k.flag_C = false; g_m68k.flag_V = false; \
    m68k_update_nz8(_res); \
    (dst) = ((dst) & ~0xFFu) | (uint8_t)(_res); \
} while(0)

#define M68K_AND16(dst, src) do { \
    uint16_t _res = (uint16_t)(dst) & (uint16_t)(src); \
    g_m68k.flag_C = false; g_m68k.flag_V = false; \
    m68k_update_nz16(_res); \
    (dst) = ((dst) & ~0xFFFFu) | (uint16_t)(_res); \
} while(0)

#define M68K_AND32(dst, src) do { \
    uint32_t _res = (uint32_t)(dst) & (uint32_t)(src); \
    g_m68k.flag_C = false; g_m68k.flag_V = false; \
    m68k_update_nz32(_res); \
    (dst) = _res; \
} while(0)

#define M68K_OR8(dst, src) do { \
    uint8_t _res = (uint8_t)(dst) | (uint8_t)(src); \
    g_m68k.flag_C = false; g_m68k.flag_V = false; \
    m68k_update_nz8(_res); \
    (dst) = ((dst) & ~0xFFu) | (uint8_t)(_res); \
} while(0)

#define M68K_OR16(dst, src) do { \
    uint16_t _res = (uint16_t)(dst) | (uint16_t)(src); \
    g_m68k.flag_C = false; g_m68k.flag_V = false; \
    m68k_update_nz16(_res); \
    (dst) = ((dst) & ~0xFFFFu) | (uint16_t)(_res); \
} while(0)

#define M68K_OR32(dst, src) do { \
    uint32_t _res = (uint32_t)(dst) | (uint32_t)(src); \
    g_m68k.flag_C = false; g_m68k.flag_V = false; \
    m68k_update_nz32(_res); \
    (dst) = _res; \
} while(0)

#define M68K_EOR8(dst, src) do { \
    uint8_t _res = (uint8_t)(dst) ^ (uint8_t)(src); \
    g_m68k.flag_C = false; g_m68k.flag_V = false; \
    m68k_update_nz8(_res); \
    (dst) = ((dst) & ~0xFFu) | (uint8_t)(_res); \
} while(0)

#define M68K_EOR16(dst, src) do { \
    uint16_t _res = (uint16_t)(dst) ^ (uint16_t)(src); \
    g_m68k.flag_C = false; g_m68k.flag_V = false; \
    m68k_update_nz16(_res); \
    (dst) = ((dst) & ~0xFFFFu) | (uint16_t)(_res); \
} while(0)

#define M68K_EOR32(dst, src) do { \
    uint32_t _res = (uint32_t)(dst) ^ (uint32_t)(src); \
    g_m68k.flag_C = false; g_m68k.flag_V = false; \
    m68k_update_nz32(_res); \
    (dst) = _res; \
} while(0)

#define M68K_NOT8(dst) do { \
    uint8_t _res = ~(uint8_t)(dst); \
    g_m68k.flag_C = false; g_m68k.flag_V = false; \
    m68k_update_nz8(_res); \
    (dst) = ((dst) & ~0xFFu) | (uint8_t)(_res); \
} while(0)

#define M68K_NOT16(dst) do { \
    uint16_t _res = ~(uint16_t)(dst); \
    g_m68k.flag_C = false; g_m68k.flag_V = false; \
    m68k_update_nz16(_res); \
    (dst) = ((dst) & ~0xFFFFu) | (uint16_t)(_res); \
} while(0)

#define M68K_NOT32(dst) do { \
    uint32_t _res = ~(uint32_t)(dst); \
    g_m68k.flag_C = false; g_m68k.flag_V = false; \
    m68k_update_nz32(_res); \
    (dst) = _res; \
} while(0)

/* --- BTST / BSET / BCLR / BCHG --- */
#define M68K_BTST32(val, bit) do { \
    g_m68k.flag_Z = !((uint32_t)(val) & (1u << ((bit) & 31))); \
} while(0)

#define M68K_BTST8(val, bit) do { \
    g_m68k.flag_Z = !((uint8_t)(val) & (1u << ((bit) & 7))); \
} while(0)

#define M68K_BSET32(dst, bit) do { \
    uint32_t _b = 1u << ((bit) & 31); \
    g_m68k.flag_Z = !((uint32_t)(dst) & _b); \
    (dst) |= _b; \
} while(0)

#define M68K_BCLR32(dst, bit) do { \
    uint32_t _b = 1u << ((bit) & 31); \
    g_m68k.flag_Z = !((uint32_t)(dst) & _b); \
    (dst) &= ~_b; \
} while(0)

#define M68K_BCHG32(dst, bit) do { \
    uint32_t _b = 1u << ((bit) & 31); \
    g_m68k.flag_Z = !((uint32_t)(dst) & _b); \
    (dst) ^= _b; \
} while(0)

/* --- TST (test: update N,Z; clear C,V) --- */
#define M68K_TST8(val) do { \
    g_m68k.flag_C = false; g_m68k.flag_V = false; \
    m68k_update_nz8((uint8_t)(val)); \
} while(0)

#define M68K_TST16(val) do { \
    g_m68k.flag_C = false; g_m68k.flag_V = false; \
    m68k_update_nz16((uint16_t)(val)); \
} while(0)

#define M68K_TST32(val) do { \
    g_m68k.flag_C = false; g_m68k.flag_V = false; \
    m68k_update_nz32((uint32_t)(val)); \
} while(0)

/* ================================================================
 * Shift and rotate macros
 *
 * count is masked to 0-63 per M68K spec (shift count from register).
 * For immediate shifts, count is 1-8.
 * ================================================================ */

/* --- LSL (Logical Shift Left) --- */
#define M68K_LSL8(dst, count) do { \
    uint8_t _cnt = (uint8_t)((count) & 63); \
    uint8_t _d = (uint8_t)(dst); \
    if (_cnt == 0) { \
        g_m68k.flag_C = false; \
    } else if (_cnt <= 8) { \
        g_m68k.flag_C = g_m68k.flag_X = ((_d >> (8 - _cnt)) & 1) != 0; \
        _d <<= _cnt; \
    } else { \
        g_m68k.flag_C = g_m68k.flag_X = false; \
        _d = 0; \
    } \
    g_m68k.flag_V = false; \
    m68k_update_nz8(_d); \
    (dst) = ((dst) & ~0xFFu) | (uint8_t)(_d); \
} while(0)

#define M68K_LSL16(dst, count) do { \
    uint8_t _cnt = (uint8_t)((count) & 63); \
    uint16_t _d = (uint16_t)(dst); \
    if (_cnt == 0) { \
        g_m68k.flag_C = false; \
    } else if (_cnt <= 16) { \
        g_m68k.flag_C = g_m68k.flag_X = ((_d >> (16 - _cnt)) & 1) != 0; \
        _d <<= _cnt; \
    } else { \
        g_m68k.flag_C = g_m68k.flag_X = false; \
        _d = 0; \
    } \
    g_m68k.flag_V = false; \
    m68k_update_nz16(_d); \
    (dst) = ((dst) & ~0xFFFFu) | (uint16_t)(_d); \
} while(0)

#define M68K_LSL32(dst, count) do { \
    uint8_t _cnt = (uint8_t)((count) & 63); \
    uint32_t _d = (uint32_t)(dst); \
    if (_cnt == 0) { \
        g_m68k.flag_C = false; \
    } else if (_cnt <= 32) { \
        g_m68k.flag_C = g_m68k.flag_X = ((_d >> (32 - _cnt)) & 1) != 0; \
        _d = (_cnt == 32) ? 0 : (_d << _cnt); \
    } else { \
        g_m68k.flag_C = g_m68k.flag_X = false; \
        _d = 0; \
    } \
    g_m68k.flag_V = false; \
    m68k_update_nz32(_d); \
    (dst) = _d; \
} while(0)

/* --- ASL (Arithmetic Shift Left): LSL's result and C/X; V when the sign bit changed at any
 * point of the shift (the top cnt+1 bits were not all equal; any set bit for cnt >= size) --- */
static inline bool m68k_asl_v(uint32_t d, unsigned cnt, unsigned bits) {
    uint32_t all = bits == 32 ? 0xFFFFFFFFu : (1u << bits) - 1;
    d &= all;
    if (cnt == 0) return false;
    if (cnt >= bits) return d != 0;
    uint32_t m = (all << (bits - 1 - cnt)) & all;
    return (d & m) != 0 && (d & m) != m;
}
#define M68K_ASL8(dst, count) do { bool _asl_v = m68k_asl_v((uint8_t)(dst), (count) & 63, 8); \
    M68K_LSL8(dst, count); g_m68k.flag_V = _asl_v; } while (0)
#define M68K_ASL16(dst, count) do { bool _asl_v = m68k_asl_v((uint16_t)(dst), (count) & 63, 16); \
    M68K_LSL16(dst, count); g_m68k.flag_V = _asl_v; } while (0)
#define M68K_ASL32(dst, count) do { bool _asl_v = m68k_asl_v((uint32_t)(dst), (count) & 63, 32); \
    M68K_LSL32(dst, count); g_m68k.flag_V = _asl_v; } while (0)

/* --- LSR (Logical Shift Right) --- */
#define M68K_LSR8(dst, count) do { \
    uint8_t _cnt = (uint8_t)((count) & 63); \
    uint8_t _d = (uint8_t)(dst); \
    if (_cnt == 0) { \
        g_m68k.flag_C = false; \
    } else if (_cnt <= 8) { \
        g_m68k.flag_C = g_m68k.flag_X = ((_d >> (_cnt - 1)) & 1) != 0; \
        _d >>= _cnt; \
    } else { \
        g_m68k.flag_C = g_m68k.flag_X = false; \
        _d = 0; \
    } \
    g_m68k.flag_V = false; \
    m68k_update_nz8(_d); \
    (dst) = ((dst) & ~0xFFu) | (uint8_t)(_d); \
} while(0)

#define M68K_LSR16(dst, count) do { \
    uint8_t _cnt = (uint8_t)((count) & 63); \
    uint16_t _d = (uint16_t)(dst); \
    if (_cnt == 0) { \
        g_m68k.flag_C = false; \
    } else if (_cnt <= 16) { \
        g_m68k.flag_C = g_m68k.flag_X = ((_d >> (_cnt - 1)) & 1) != 0; \
        _d >>= _cnt; \
    } else { \
        g_m68k.flag_C = g_m68k.flag_X = false; \
        _d = 0; \
    } \
    g_m68k.flag_V = false; \
    m68k_update_nz16(_d); \
    (dst) = ((dst) & ~0xFFFFu) | (uint16_t)(_d); \
} while(0)

#define M68K_LSR32(dst, count) do { \
    uint8_t _cnt = (uint8_t)((count) & 63); \
    uint32_t _d = (uint32_t)(dst); \
    if (_cnt == 0) { \
        g_m68k.flag_C = false; \
    } else if (_cnt <= 32) { \
        g_m68k.flag_C = g_m68k.flag_X = ((_d >> (_cnt - 1)) & 1) != 0; \
        _d = (_cnt == 32) ? 0 : (_d >> _cnt); \
    } else { \
        g_m68k.flag_C = g_m68k.flag_X = false; \
        _d = 0; \
    } \
    g_m68k.flag_V = false; \
    m68k_update_nz32(_d); \
    (dst) = _d; \
} while(0)

/* --- ASR (Arithmetic Shift Right — preserves sign) --- */
#define M68K_ASR8(dst, count) do { \
    uint8_t _cnt = (uint8_t)((count) & 63); \
    int8_t _d = (int8_t)(uint8_t)(dst); \
    if (_cnt == 0) { \
        g_m68k.flag_C = false; \
    } else { \
        uint8_t _sc = (_cnt > 8) ? 8 : _cnt; \
        g_m68k.flag_C = g_m68k.flag_X = ((_d >> (_sc - 1)) & 1) != 0; \
        _d >>= _sc; \
    } \
    g_m68k.flag_V = false; \
    m68k_update_nz8((uint8_t)_d); \
    (dst) = ((dst) & ~0xFFu) | (uint8_t)((uint8_t)_d); \
} while(0)

#define M68K_ASR16(dst, count) do { \
    uint8_t _cnt = (uint8_t)((count) & 63); \
    int16_t _d = (int16_t)(uint16_t)(dst); \
    if (_cnt == 0) { \
        g_m68k.flag_C = false; \
    } else { \
        uint8_t _sc = (_cnt > 16) ? 16 : _cnt; \
        g_m68k.flag_C = g_m68k.flag_X = ((_d >> (_sc - 1)) & 1) != 0; \
        _d >>= _sc; \
    } \
    g_m68k.flag_V = false; \
    m68k_update_nz16((uint16_t)_d); \
    (dst) = ((dst) & ~0xFFFFu) | (uint16_t)((uint16_t)_d); \
} while(0)

#define M68K_ASR32(dst, count) do { \
    uint8_t _cnt = (uint8_t)((count) & 63); \
    int32_t _d = (int32_t)(uint32_t)(dst); \
    if (_cnt == 0) { \
        g_m68k.flag_C = false; \
    } else { \
        uint8_t _sc = (_cnt > 32) ? 32 : _cnt; \
        g_m68k.flag_C = g_m68k.flag_X = ((_d >> (_sc - 1)) & 1) != 0; \
        _d = (_sc == 32) ? (_d >> 31) : (_d >> _sc); \
    } \
    g_m68k.flag_V = false; \
    m68k_update_nz32((uint32_t)_d); \
    (dst) = (uint32_t)_d; \
} while(0)

/* --- ROL (Rotate Left) --- */
#define M68K_ROL8(dst, count) do { \
    uint8_t _cnt = (uint8_t)((count) & 63) % 8; \
    uint8_t _d = (uint8_t)(dst); \
    if ((count) & 63) { \
        _d = (_d << _cnt) | (_d >> (8 - _cnt)); \
        g_m68k.flag_C = (_d & 1) != 0; \
    } else { \
        g_m68k.flag_C = false; \
    } \
    g_m68k.flag_V = false; \
    m68k_update_nz8(_d); \
    (dst) = ((dst) & ~0xFFu) | (uint8_t)(_d); \
} while(0)

#define M68K_ROL16(dst, count) do { \
    uint8_t _cnt = (uint8_t)((count) & 63) % 16; \
    uint16_t _d = (uint16_t)(dst); \
    if ((count) & 63) { \
        _d = (_d << _cnt) | (_d >> (16 - _cnt)); \
        g_m68k.flag_C = (_d & 1) != 0; \
    } else { \
        g_m68k.flag_C = false; \
    } \
    g_m68k.flag_V = false; \
    m68k_update_nz16(_d); \
    (dst) = ((dst) & ~0xFFFFu) | (uint16_t)(_d); \
} while(0)

#define M68K_ROL32(dst, count) do { \
    uint8_t _cnt = (uint8_t)((count) & 63) % 32; \
    uint32_t _d = (uint32_t)(dst); \
    if ((count) & 63) { \
        _d = (_d << _cnt) | (_d >> (32 - _cnt)); \
        g_m68k.flag_C = (_d & 1) != 0; \
    } else { \
        g_m68k.flag_C = false; \
    } \
    g_m68k.flag_V = false; \
    m68k_update_nz32(_d); \
    (dst) = _d; \
} while(0)

/* --- ROR (Rotate Right) --- */
#define M68K_ROR8(dst, count) do { \
    uint8_t _cnt = (uint8_t)((count) & 63) % 8; \
    uint8_t _d = (uint8_t)(dst); \
    if ((count) & 63) { \
        _d = (_d >> _cnt) | (_d << (8 - _cnt)); \
        g_m68k.flag_C = (_d & 0x80) != 0; \
    } else { \
        g_m68k.flag_C = false; \
    } \
    g_m68k.flag_V = false; \
    m68k_update_nz8(_d); \
    (dst) = ((dst) & ~0xFFu) | (uint8_t)(_d); \
} while(0)

#define M68K_ROR16(dst, count) do { \
    uint8_t _cnt = (uint8_t)((count) & 63) % 16; \
    uint16_t _d = (uint16_t)(dst); \
    if ((count) & 63) { \
        _d = (_d >> _cnt) | (_d << (16 - _cnt)); \
        g_m68k.flag_C = (_d & 0x8000) != 0; \
    } else { \
        g_m68k.flag_C = false; \
    } \
    g_m68k.flag_V = false; \
    m68k_update_nz16(_d); \
    (dst) = ((dst) & ~0xFFFFu) | (uint16_t)(_d); \
} while(0)

#define M68K_ROR32(dst, count) do { \
    uint8_t _cnt = (uint8_t)((count) & 63) % 32; \
    uint32_t _d = (uint32_t)(dst); \
    if ((count) & 63) { \
        _d = (_d >> _cnt) | (_d << (32 - _cnt)); \
        g_m68k.flag_C = (_d & 0x80000000u) != 0; \
    } else { \
        g_m68k.flag_C = false; \
    } \
    g_m68k.flag_V = false; \
    m68k_update_nz32(_d); \
    (dst) = _d; \
} while(0)

/* --- ROXL (Rotate Left through eXtend) --- */
#define M68K_ROXL8(dst, count) do { \
    uint8_t _cnt = (uint8_t)((count) & 63) % 9; \
    uint8_t _d = (uint8_t)(dst); \
    for (uint8_t _i = 0; _i < _cnt; _i++) { \
        bool _old_x = g_m68k.flag_X; \
        g_m68k.flag_X = g_m68k.flag_C = (_d & 0x80) != 0; \
        _d = (_d << 1) | (_old_x ? 1u : 0u); \
    } \
    if (!_cnt) g_m68k.flag_C = g_m68k.flag_X;   /* count 0 or a multiple of size+1 */ \
    g_m68k.flag_V = false; \
    m68k_update_nz8(_d); \
    (dst) = ((dst) & ~0xFFu) | (uint8_t)(_d); \
} while(0)

#define M68K_ROXL16(dst, count) do { \
    uint8_t _cnt = (uint8_t)((count) & 63) % 17; \
    uint16_t _d = (uint16_t)(dst); \
    for (uint8_t _i = 0; _i < _cnt; _i++) { \
        bool _old_x = g_m68k.flag_X; \
        g_m68k.flag_X = g_m68k.flag_C = (_d & 0x8000) != 0; \
        _d = (_d << 1) | (_old_x ? 1u : 0u); \
    } \
    if (!_cnt) g_m68k.flag_C = g_m68k.flag_X;   /* count 0 or a multiple of size+1 */ \
    g_m68k.flag_V = false; \
    m68k_update_nz16(_d); \
    (dst) = ((dst) & ~0xFFFFu) | (uint16_t)(_d); \
} while(0)

#define M68K_ROXL32(dst, count) do { \
    uint8_t _cnt = (uint8_t)((count) & 63) % 33; \
    uint32_t _d = (uint32_t)(dst); \
    for (uint8_t _i = 0; _i < _cnt; _i++) { \
        bool _old_x = g_m68k.flag_X; \
        g_m68k.flag_X = g_m68k.flag_C = (_d & 0x80000000u) != 0; \
        _d = (_d << 1) | (_old_x ? 1u : 0u); \
    } \
    if (!_cnt) g_m68k.flag_C = g_m68k.flag_X;   /* count 0 or a multiple of size+1 */ \
    g_m68k.flag_V = false; \
    m68k_update_nz32(_d); \
    (dst) = _d; \
} while(0)

/* --- ROXR (Rotate Right through eXtend) --- */
#define M68K_ROXR8(dst, count) do { \
    uint8_t _cnt = (uint8_t)((count) & 63) % 9; \
    uint8_t _d = (uint8_t)(dst); \
    for (uint8_t _i = 0; _i < _cnt; _i++) { \
        bool _old_x = g_m68k.flag_X; \
        g_m68k.flag_X = g_m68k.flag_C = (_d & 1) != 0; \
        _d = (_d >> 1) | (_old_x ? 0x80u : 0u); \
    } \
    if (!_cnt) g_m68k.flag_C = g_m68k.flag_X;   /* count 0 or a multiple of size+1 */ \
    g_m68k.flag_V = false; \
    m68k_update_nz8(_d); \
    (dst) = ((dst) & ~0xFFu) | (uint8_t)(_d); \
} while(0)

#define M68K_ROXR16(dst, count) do { \
    uint8_t _cnt = (uint8_t)((count) & 63) % 17; \
    uint16_t _d = (uint16_t)(dst); \
    for (uint8_t _i = 0; _i < _cnt; _i++) { \
        bool _old_x = g_m68k.flag_X; \
        g_m68k.flag_X = g_m68k.flag_C = (_d & 1) != 0; \
        _d = (_d >> 1) | (_old_x ? 0x8000u : 0u); \
    } \
    if (!_cnt) g_m68k.flag_C = g_m68k.flag_X;   /* count 0 or a multiple of size+1 */ \
    g_m68k.flag_V = false; \
    m68k_update_nz16(_d); \
    (dst) = ((dst) & ~0xFFFFu) | (uint16_t)(_d); \
} while(0)

#define M68K_ROXR32(dst, count) do { \
    uint8_t _cnt = (uint8_t)((count) & 63) % 33; \
    uint32_t _d = (uint32_t)(dst); \
    for (uint8_t _i = 0; _i < _cnt; _i++) { \
        bool _old_x = g_m68k.flag_X; \
        g_m68k.flag_X = g_m68k.flag_C = (_d & 1) != 0; \
        _d = (_d >> 1) | (_old_x ? 0x80000000u : 0u); \
    } \
    if (!_cnt) g_m68k.flag_C = g_m68k.flag_X;   /* count 0 or a multiple of size+1 */ \
    g_m68k.flag_V = false; \
    m68k_update_nz32(_d); \
    (dst) = _d; \
} while(0)

/* --- SWAP (swap halves of data register) --- */
#define M68K_SWAP(dst) do { \
    uint32_t _d = (uint32_t)(dst); \
    _d = (_d >> 16) | (_d << 16); \
    g_m68k.flag_C = false; g_m68k.flag_V = false; \
    m68k_update_nz32(_d); \
    (dst) = _d; \
} while(0)

/* --- EXT (sign extend) --- */
#define M68K_EXT16(dst) do { \
    int16_t _res = (int8_t)(uint8_t)(dst); \
    g_m68k.flag_C = false; g_m68k.flag_V = false; \
    m68k_update_nz16((uint16_t)_res); \
    (dst) = ((dst) & ~0xFFFFu) | (uint16_t)((uint16_t)_res); \
} while(0)

#define M68K_EXT32(dst) do { \
    int32_t _res = (int16_t)(uint16_t)(dst); \
    g_m68k.flag_C = false; g_m68k.flag_V = false; \
    m68k_update_nz32((uint32_t)_res); \
    (dst) = (uint32_t)_res; \
} while(0)

/* ================================================================
 * CPU lifecycle
 * ================================================================ */

/* Reset CPU to power-on state */
void recomp_m68k_reset(void);

/* Handle exceptions (interrupts, traps, etc.) */
void recomp_m68k_exception(uint8_t vector);

/* Run an interrupt handler (e.g. the level-6 VBlank vector) from inside
 * the main thread's execution, the way the hardware does: the interrupted
 * code's entire context, SR and flags included, is restored afterwards.
 * Call this from a VBlank callback rather than func_table_call. If SR
 * masks the level it is held pending and taken by the bus clock once the
 * mask drops. */
void recomp_m68k_interrupt(uint8_t level);
extern uint8_t g_m68k_irq_pending;

#endif /* GENRECOMP_M68K_H */
