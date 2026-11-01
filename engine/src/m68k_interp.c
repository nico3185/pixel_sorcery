/*
 * m68k_interp.c -- the port's 68000 interpreter: the floor under the recompiled code.
 *
 * When the code jumps to an address no generated function covers (a dispatch miss), the
 * port interprets from there instead of stopping: through the routine and everything it
 * calls that is not compiled either, back to compiled code at the outermost RTS. The
 * address is reported (stderr, PS_INTERP_LOG) so the next generation compiles it in.
 *
 * Parity with the generated C, by construction where possible:
 *   - flags and results through the same M68K_* macros (m68k.h),
 *   - the same cycle accounting: M68K_PRE, then the opcode's base cost from the
 *     generator's table (recomp_cycles.c) with the generator's adjustments (timed() in
 *     tools/recompiler/generate.py), charged through CYC so scanline events and
 *     interrupts land on the same instruction boundaries,
 *   - the same call conventions: compiled callees through func_table_jsr, the outermost
 *     RTS through func_table_rts, a jump into compiled code as a trampoline tail call,
 *     TRAP/CHK through recomp_m68k_exception, RTE as a return.
 * An opcode it cannot execute takes the 68000's own illegal-instruction exception
 * (vector 4), and is reported. PS_INTERP=all interprets everything (func_table_lookup
 * misses on purpose): the co-simulation check, compared with the original side by side.
 * Design after segagenesisrecomp's Tier-3 interpreter; the code is PixelSorcery's own.
 */
#include "genrecomp/m68k.h"
#include "genrecomp/bus.h"
#include "genrecomp/func_table.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ps_portable.h"
#ifdef PS_SUBCPU
extern const uint16_t ps_m68k_cycles[0x10000];
#define ps_m68k_nhooks 0
static uint32_t run_hook(uint32_t pc) { (void)pc; return 0; }
#else
/* generated per title (recomp_cycles.c); tools without generated code (the reference
 * runner, tests) get this empty default and never interpret */
#if defined(_MSC_VER)
extern const uint16_t ps_m68k_cycles[0x10000];
const uint16_t ps_m68k_cycles_ps_default[0x10000] = { 0 };
PS_ALTNAME(ps_m68k_cycles, ps_m68k_cycles_ps_default)
#else
__attribute__((weak)) const uint16_t ps_m68k_cycles[0x10000] = { 0 };
#endif
/* title hooks (recomp.json "hooks"), generated per title (recomp_hooks.c, sorted by pc): each
 * returns where to go on, 0 = run the instruction. Weak empty default as above. */
#if defined(_MSC_VER)
extern const uint32_t *ps_m68k_hook_pc; extern uint32_t (*const *ps_m68k_hook_fn)(void); extern int ps_m68k_nhooks;
const uint32_t *ps_m68k_hook_pc_ps_default = NULL; uint32_t (*const *ps_m68k_hook_fn_ps_default)(void) = NULL;
int ps_m68k_nhooks_ps_default = 0;
PS_ALTNAME(ps_m68k_hook_pc, ps_m68k_hook_pc_ps_default)
PS_ALTNAME(ps_m68k_hook_fn, ps_m68k_hook_fn_ps_default)
PS_ALTNAME(ps_m68k_nhooks, ps_m68k_nhooks_ps_default)
#else
__attribute__((weak)) const uint32_t *ps_m68k_hook_pc = NULL;
__attribute__((weak)) uint32_t (*const *ps_m68k_hook_fn)(void) = NULL;
__attribute__((weak)) int ps_m68k_nhooks = 0;
#endif
static uint32_t run_hook(uint32_t pc) {
    int lo = 0, hi = ps_m68k_nhooks - 1;
    if (hi < 0 || pc < ps_m68k_hook_pc[0] || pc > ps_m68k_hook_pc[hi]) return 0;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (ps_m68k_hook_pc[m] == pc) {
            uint32_t t = ps_m68k_hook_fn[m]();
            if (t == 1) {                       /* a skip whose length the generator never saw */
                static uint32_t warned;
                if (warned != pc) { warned = pc; fprintf(stderr, "interp: hook at $%06X skips an instruction never converted; run as is\n", pc); }
                return 0;
            }
            return t;
        }
        if (ps_m68k_hook_pc[m] < pc) lo = m + 1; else hi = m - 1;
    }
    return 0;
}
#endif
/* PS_SUBCPU (s68k_interp.c): the same interpreter as the Mega CD's sub CPU - runs are slices of a
 * free-running CPU, not calls: an RTS always pops, and the instruction boundary (s68k_boundary:
 * exceptions taken, interrupts, STOP, the end of the slice) decides where to go on, 0 = stop */
#ifdef PS_SUBCPU
#define SUB_DEPTH0(d) 0
uint32_t s68k_boundary(uint32_t next);
void s68k_stop(void);
#else
#define SUB_DEPTH0(d) ((d) == 0)
#endif
void func_table_tail(uint32_t m68k_addr);
void recomp_m68k_exception(uint8_t vector);

static uint32_t s_pc;                               /* the instruction being executed */
static uint32_t s_next;                             /* fetch pointer */
/* PS_INTERP_COVER=file: at exit, one bit per even address of the 24-bit space (1 MB) for every
 * instruction the interpreter ran - the code not compiled yet (SHCD: by program region) */
static uint8_t *s_icover; static int s_icover_init;
static void icover_write(void) {
    FILE *f = fopen(getenv("PS_INTERP_COVER"), "wb");
    if (f) { fwrite(s_icover, 1, 1 << 20, f); fclose(f); }
}
static void icover_init(void) {
    s_icover_init = 1;
    const char *e = getenv("PS_INTERP_COVER");
    if (e && *e && (s_icover = calloc(1, 1 << 20))) atexit(icover_write);
}
static long s_entered, s_insns;

static uint16_t fetch16(void) { uint16_t v = bus_read16(s_next); s_next += 2; return v; }
static uint32_t fetch32(void) { uint32_t v = bus_read32(s_next); s_next += 4; return v; }

/* ------------------------------------------------------------------ operands */
enum { EA_D, EA_A, EA_MEM, EA_IMM };
typedef struct { int kind, reg; uint32_t addr, imm; } Ea;

static int size_bytes(int sz) { return sz == 8 ? 1 : sz == 16 ? 2 : 4; }

static uint32_t index_value(uint16_t ext) {
    int r = (ext >> 12) & 7;
    uint32_t v = (ext & 0x8000) ? g_m68k.a[r] : g_m68k.d[r];
    if (!(ext & 0x0800)) v = (uint32_t)(int32_t)(int16_t)v;
    return v + (uint32_t)(int32_t)(int8_t)(ext & 0xFF);
}

/* decode an effective address (mode, reg) for an operand of sz bits; (An)+ and -(An)
 * update the register here, as the operand is taken */
static Ea ea_decode(int mode, int reg, int sz) {
    Ea e = { EA_MEM, reg, 0, 0 };
    int step = size_bytes(sz);
    if (step == 1 && reg == 7) step = 2;            /* the stack stays word-aligned */
    switch (mode) {
    case 0: e.kind = EA_D; break;
    case 1: e.kind = EA_A; break;
    case 2: e.addr = g_m68k.a[reg]; break;
    case 3: e.addr = g_m68k.a[reg]; g_m68k.a[reg] += (uint32_t)step; break;
    case 4: g_m68k.a[reg] -= (uint32_t)step; e.addr = g_m68k.a[reg]; break;
    case 5: e.addr = g_m68k.a[reg] + (uint32_t)(int32_t)(int16_t)fetch16(); break;
    case 6: { uint16_t x = fetch16(); e.addr = g_m68k.a[reg] + index_value(x); break; }
    case 7:
        switch (reg) {
        case 0: e.addr = (uint32_t)(int32_t)(int16_t)fetch16(); break;
        case 1: e.addr = fetch32(); break;
        case 2: { uint32_t b = s_next; e.addr = b + (uint32_t)(int32_t)(int16_t)fetch16(); break; }
        case 3: { uint32_t b = s_next; uint16_t x = fetch16(); e.addr = b + index_value(x); break; }
        case 4: e.kind = EA_IMM; e.imm = sz == 32 ? fetch32() : sz == 16 ? fetch16() : (fetch16() & 0xFF); break;
        default: e.kind = -1;
        }
        break;
    }
    return e;                                   /* 32 bits, as a register holds it (LEA, PEA) */
}

/* extension words still to fetch after a read (MOVE: its destination's), so that a memory read
 * sees the prefetch address the generated code gives it - the instruction's end (VDP status:
 * its upper bits are the next opcode word; SHK's VBlank tested them, PS_INTERP=random:30:2) */
static int s_ext_after;
static uint32_t ea_read(const Ea *e, int sz) {
    if (e->kind != EA_D && e->kind != EA_A && e->kind != EA_IMM) g_m68k_pf = s_next + 2u * (unsigned)s_ext_after;
    switch (e->kind) {
    case EA_D: return sz == 32 ? g_m68k.d[e->reg] : g_m68k.d[e->reg] & (sz == 16 ? 0xFFFF : 0xFF);
    case EA_A: return sz == 32 ? g_m68k.a[e->reg] : g_m68k.a[e->reg] & 0xFFFF;
    case EA_IMM: return e->imm;
    default: return sz == 32 ? bus_read32(e->addr) : sz == 16 ? bus_read16(e->addr) : bus_read8(e->addr);
    }
}

static void ea_write(const Ea *e, int sz, uint32_t v) {
    switch (e->kind) {
    case EA_D:
        if (sz == 32) g_m68k.d[e->reg] = v;
        else if (sz == 16) g_m68k.d[e->reg] = (g_m68k.d[e->reg] & 0xFFFF0000u) | (v & 0xFFFF);
        else g_m68k.d[e->reg] = (g_m68k.d[e->reg] & 0xFFFFFF00u) | (v & 0xFF);
        break;
    case EA_A: g_m68k.a[e->reg] = sz == 16 ? (uint32_t)(int32_t)(int16_t)v : v; break;
    case EA_MEM:
        if (sz == 32) bus_write32(e->addr, v); else if (sz == 16) bus_write16(e->addr, (uint16_t)v);
        else bus_write8(e->addr, (uint8_t)v);
        break;
    }
}

static bool cond(int cc) {
    switch (cc) {
    case 0: return M68K_CC_T;  case 1: return M68K_CC_F;  case 2: return M68K_CC_HI; case 3: return M68K_CC_LS;
    case 4: return M68K_CC_CC; case 5: return M68K_CC_CS; case 6: return M68K_CC_NE; case 7: return M68K_CC_EQ;
    case 8: return M68K_CC_VC; case 9: return M68K_CC_VS; case 10: return M68K_CC_PL; case 11: return M68K_CC_MI;
    case 12: return M68K_CC_GE; case 13: return M68K_CC_LT; case 14: return M68K_CC_GT; default: return M68K_CC_LE;
    }
}

static void set_nz(uint32_t v, int sz) {
    if (sz == 8) m68k_update_nz8((uint8_t)v); else if (sz == 16) m68k_update_nz16((uint16_t)v); else m68k_update_nz32(v);
}

/* ------------------------------------------------------------------ arithmetic */
/* dst op= src through the generated code's own macros; returns the new value */
#define BIN(OP, sz, d, s) do { uint32_t _v = (d); \
    if ((sz) == 8) { OP##8(_v, (s)); } else if ((sz) == 16) { OP##16(_v, (s)); } else { OP##32(_v, (s)); } \
    (d) = _v; } while (0)

static uint32_t alu(int op, int sz, uint32_t d, uint32_t s) {
    switch (op) {
    case 0: BIN(M68K_OR, sz, d, s); break;
    case 1: BIN(M68K_AND, sz, d, s); break;
    case 2: BIN(M68K_SUB, sz, d, s); break;
    case 3: BIN(M68K_ADD, sz, d, s); break;
    case 5: BIN(M68K_EOR, sz, d, s); break;
    case 6: BIN(M68K_CMP, sz, d, s); break;
    case 7: BIN(M68K_ADDX, sz, d, s); break;
    case 8: BIN(M68K_SUBX, sz, d, s); break;
    }
    return d;
}

static uint32_t shift(int kind, int left, int sz, uint32_t d, unsigned cnt) {
    uint32_t v = d;
    switch (kind * 2 + left) {
    case 0: if (sz == 8) { M68K_ASR8(v, cnt); } else if (sz == 16) { M68K_ASR16(v, cnt); } else { M68K_ASR32(v, cnt); } break;
    case 1: if (sz == 8) { M68K_ASL8(v, cnt); } else if (sz == 16) { M68K_ASL16(v, cnt); } else { M68K_ASL32(v, cnt); } break;
    case 3: if (sz == 8) { M68K_LSL8(v, cnt); } else if (sz == 16) { M68K_LSL16(v, cnt); } else { M68K_LSL32(v, cnt); } break;
    case 2: if (sz == 8) { M68K_LSR8(v, cnt); } else if (sz == 16) { M68K_LSR16(v, cnt); } else { M68K_LSR32(v, cnt); } break;
    case 4: if (sz == 8) { M68K_ROXR8(v, cnt); } else if (sz == 16) { M68K_ROXR16(v, cnt); } else { M68K_ROXR32(v, cnt); } break;
    case 5: if (sz == 8) { M68K_ROXL8(v, cnt); } else if (sz == 16) { M68K_ROXL16(v, cnt); } else { M68K_ROXL32(v, cnt); } break;
    case 6: if (sz == 8) { M68K_ROR8(v, cnt); } else if (sz == 16) { M68K_ROR16(v, cnt); } else { M68K_ROR32(v, cnt); } break;
    case 7: if (sz == 8) { M68K_ROL8(v, cnt); } else if (sz == 16) { M68K_ROL16(v, cnt); } else { M68K_ROL32(v, cnt); } break;
    }
    return v;
}

/* ------------------------------------------------------------------ the loop */
enum { RUN, LEAVE };

static void report_unknown(uint16_t op) {
    static int n;
    if (n++ < 20) fprintf(stderr, "m68k_interp: no implementation for opcode %04X at $%06X: illegal-instruction exception\n", op, s_pc);
}

/* Run from entry until the outermost RTS/RTE (depth 0); calls into compiled code
 * stay compiled. Returns like a generated function returns. */
/* Inside the loop, costs are added (ICYC) and due events are taken at the next instruction
 * boundary, where the PC an interrupt returns to is exactly s_next (a handler may rewrite it);
 * before leaving the loop (IEV0) they are taken as generated code takes them. */
#define ICYC(n) do { if (g_cyc_skip) g_cyc_skip = 0; else g_cyc += (unsigned)(n); } while (0)
#define IEV0() do { if ((int)(g_cyc - g_cyc_event) >= 0) { g_m68k_ipc = 0; bus_cycle_event(); } } while (0)
/* an event at a hand-over to compiled code: an interrupt taken here returns to `pc` (where the
 * 68000 goes next - a branch's or call's target, a return's address), as the compiled code's
 * CYCP records it; with 0 a handler reading its stacked PC saw 0 (SHK's VBlank checks whether
 * it interrupted the Kosinski decompressor, \$1C26-: PS_INTERP=random:30:3 diverged) */
#define IEVP(t) do { if ((int)(g_cyc - g_cyc_event) >= 0) { g_m68k_ipc = (t); bus_cycle_event(); g_m68k_ipc = 0; \
                         if (g_m68k_redirect) { (t) = g_m68k_redirect; g_m68k_redirect = 0; } } } while (0)   /* t: an lvalue */
static void interp_loop(uint32_t entry);
/* a call into compiled code; when it returns past this run's own pending calls (the callee
 * dropped return addresses: SHK's objects pop theirs into a field, move.l (sp)+,$34(a0)), resume
 * at the address it returned to, at that call's level (PS_INTERP=random:30:2 diverged) */
extern uint32_t g_rts_orphan; extern int g_jsr_from_interp;
static bool interp_jsr(uint32_t t, int *depth, const uint32_t *rst) {
    g_jsr_from_interp = 1;                               /* marks the frame func_table_jsr opens */
    bool ok = func_table_jsr(t, s_next);
    g_jsr_from_interp = 0;
    if (ok && g_rts_orphan) {
        uint32_t r = g_rts_orphan; g_rts_orphan = 0;
        int k = *depth - 1;
        while (k >= 0 && !(k < 256 && rst[k] == r)) k--;
        if (k >= 0) *depth = k;
        s_next = r;
    }
    return ok;
}
void m68k_interp_run(uint32_t entry) {
    /* runs nest: an interrupt taken inside an interpreted instruction (in CYC) may
     * interpret its handler; the outer run's position is kept across it */
    uint32_t pc = s_pc, next = s_next;
    interp_loop(entry);
    s_pc = pc; s_next = next;
}

static void interp_loop(uint32_t entry) {
    int depth = 0;
    uint32_t rst[256];                                   /* return addresses of this run's own calls */
    s_entered++;
    s_next = entry;
    for (;;) {
#ifdef PS_SUBCPU
        if (!(s_next = s68k_boundary(s_next))) return;
        (void)s_icover_init; (void)icover_init;
#else
        if (!s_icover_init) icover_init();
        if ((int)(g_cyc - g_cyc_event) >= 0) {
            g_m68k_ipc = s_next; g_m68k_ipc_kind = 1; bus_cycle_event(); g_m68k_ipc = 0; g_m68k_ipc_kind = 0;
            if (g_m68k_redirect) { s_next = g_m68k_redirect; g_m68k_redirect = 0; }
        }
#endif
        s_pc = s_next;
        if (s_icover) s_icover[(s_pc & 0xFFFFFF) >> 4] |= (uint8_t)(1u << ((s_pc >> 1) & 7));
        if (ps_m68k_nhooks) { uint32_t t = run_hook(s_pc); if (t) { s_next = t; continue; } }
        uint16_t op = fetch16();
        unsigned base = ps_m68k_cycles[op];
        s_insns++;
        ITRACE(s_pc);
        M68K_PRE();
        g_m68k_pf = s_pc + 2; g_m68k_icost = base;   /* status reads, as generated code sets them */
        int hi = op >> 12;
        int sz2 = (op >> 6) & 3, sz = sz2 == 0 ? 8 : sz2 == 1 ? 16 : 32;
        int mode = (op >> 3) & 7, reg = op & 7, rx = (op >> 9) & 7;

        switch (hi) {
        case 0x0: {
            if ((op & 0xF138) == 0x0108) {                      /* MOVEP */
                uint32_t a = g_m68k.a[reg] + (uint32_t)(int32_t)(int16_t)fetch16();
                int n = (op & 0x40) ? 4 : 2;
                if (op & 0x80) { uint32_t v = g_m68k.d[rx];
                    for (int i = 0; i < n; i++) bus_write8(a + 2 * (uint32_t)i, (uint8_t)(v >> (8 * (n - 1 - i)))); }
                else { uint32_t v = 0; for (int i = 0; i < n; i++) v = v << 8 | bus_read8(a + 2 * (uint32_t)i);
                    if (n == 2) g_m68k.d[rx] = (g_m68k.d[rx] & 0xFFFF0000u) | v; else g_m68k.d[rx] = v; }
                ICYC(base); break;
            }
            if ((op & 0x0100) || (op & 0x0F00) == 0x0800) {      /* bit ops: dynamic Dn or static #n */
                int kind = (op >> 6) & 3;                         /* BTST BCHG BCLR BSET */
                unsigned bit, extra = 0;
                if (op & 0x0100) { bit = g_m68k.d[rx]; if (mode == 0 && kind && (bit & 31) >= 16) g_cyc += 14; }
                else { bit = fetch16() & 0xFF; if (mode == 0 && kind && (bit & 31) >= 16) extra = 14; }
                Ea e = ea_decode(mode, reg, mode == 0 ? 32 : 8);
                uint32_t v = ea_read(&e, mode == 0 ? 32 : 8);
                if (mode == 0) {
                    switch (kind) { case 0: M68K_BTST32(v, bit); break; case 1: M68K_BCHG32(v, bit); break;
                                    case 2: M68K_BCLR32(v, bit); break; default: M68K_BSET32(v, bit); }
                } else {
                    unsigned b = bit & 7; g_m68k.flag_Z = !(v & (1u << b));
                    if (kind == 1) v ^= 1u << b; else if (kind == 2) v &= ~(1u << b); else if (kind == 3) v |= 1u << b;
                }
                if (kind) ea_write(&e, mode == 0 ? 32 : 8, v);
                ICYC(base + extra); break;
            }
            int which = (op >> 9) & 7;                           /* ORI ANDI SUBI ADDI . EORI CMPI */
            if (sz2 == 3 || which == 4 || which == 7) { report_unknown(op); g_m68k.pc = s_pc; ICYC(base); IEV0(); recomp_m68k_exception(4); break; }
            uint32_t imm = sz == 32 ? fetch32() : sz == 16 ? fetch16() : (fetch16() & 0xFF);
            if (mode == 7 && reg == 4) {                         /* to CCR / SR */
                uint16_t sr = m68k_get_sr();
                uint16_t v = which == 0 ? sr | imm : which == 1 ? sr & imm : sr ^ imm;
                if (sz == 8) m68k_set_ccr((uint8_t)v); else m68k_set_sr(v);
                ICYC(base); break;
            }
            static const int map[8] = { 0, 1, 2, 3, -1, 5, 6, -1 };
            Ea e = ea_decode(mode, reg, sz);
            uint32_t d = ea_read(&e, sz);
            uint32_t r = alu(map[which], sz, d, imm);
            if (which != 6) ea_write(&e, sz, r);
            ICYC(base); break;
        }
        case 0x1: case 0x2: case 0x3: {                          /* MOVE / MOVEA */
            int msz = hi == 1 ? 8 : hi == 3 ? 16 : 32;
            Ea s = ea_decode(mode, reg, msz);
            {   int dm = (op >> 6) & 7;                          /* the destination's extension words */
                s_ext_after = (dm == 5 || dm == 6 || (dm == 7 && rx == 0)) ? 1 : (dm == 7 && rx == 1) ? 2 : 0; }
            uint32_t v = ea_read(&s, msz);
            s_ext_after = 0;
            int dmode = (op >> 6) & 7;
            if (dmode == 1) { g_m68k.a[rx] = msz == 16 ? (uint32_t)(int32_t)(int16_t)v : v; ICYC(base); break; }
            Ea d = ea_decode(dmode, rx, msz);
            ea_write(&d, msz, v);
            g_m68k.flag_V = false; g_m68k.flag_C = false; set_nz(v, msz);
            ICYC(base); break;
        }
        case 0x4: {
            if (op == 0x4E75 || op == 0x4E77) {                  /* RTS / RTR */
                ICYC(base);
                if (op == 0x4E77) { m68k_set_ccr((uint8_t)bus_read16(g_m68k.a[7])); g_m68k.a[7] += 2; }
                if (SUB_DEPTH0(depth)) {
                    if ((int)(g_cyc - g_cyc_event) >= 0) {      /* an interrupt after the RTS (see CYCR) */
                        g_m68k_ipc = bus_read32(g_m68k.a[7]) & 0xFFFFFF; bus_cycle_event(); g_m68k_ipc = 0;
                        if (g_m68k_redirect) { uint32_t t = g_m68k_redirect; g_m68k_redirect = 0; g_m68k.a[7] += 4; func_table_tail(t); return; }
                    }
                    func_table_rts(); return;
                }
                s_next = bus_read32(g_m68k.a[7]) & 0xFFFFFF; g_m68k.a[7] += 4; depth--;
                break;
            }
            if (op == 0x4E73) {                                   /* RTE: as generated code */
                uint32_t t;
                if (!m68k_rte(&t)) { CYC(base); IEV0(); return; }
                ICYC(base); s_next = t; break;
            }
            if (op == 0x4E71) { ICYC(base); break; }               /* NOP */
            if (op == 0x4E70) { g_cyc += 132 * 7; g_refresh += 128 * 7; ICYC(base); break; }   /* RESET (external devices only): 132 clocks */
#ifdef PS_SUBCPU
            if (op == 0x4E72) { m68k_set_sr(fetch16()); ICYC(base); s68k_stop(); break; }   /* STOP: waits for an interrupt */
#else
            if (op == 0x4E72) { m68k_set_sr(fetch16()); ICYC(base); break; }   /* STOP: runs on to the interrupt */
#endif
            if (op == 0x4E76) { ICYC(base); if (g_m68k.flag_V) { g_m68k.pc = s_next; IEV0(); recomp_m68k_exception(7); } else g_cyc += 4 * 7; break; }
            if ((op & 0xFFF0) == 0x4E40) { ICYC(base); g_m68k.pc = s_next; IEV0(); recomp_m68k_exception((uint8_t)(32 + (op & 15))); break; }
            if ((op & 0xFFF8) == 0x4E50) {                         /* LINK */
                int16_t dsp = (int16_t)fetch16();
                g_m68k.a[7] -= 4; bus_write32(g_m68k.a[7], g_m68k.a[reg]);
                g_m68k.a[reg] = g_m68k.a[7]; g_m68k.a[7] += (uint32_t)(int32_t)dsp;
                ICYC(base); break;
            }
            if ((op & 0xFFF8) == 0x4E58) {                         /* UNLK */
                if (reg == 7) g_m68k.a[7] = bus_read32(g_m68k.a[7]);   /* UNLK A7: the load overwrites SP */
                else { g_m68k.a[7] = g_m68k.a[reg]; g_m68k.a[reg] = bus_read32(g_m68k.a[7]); g_m68k.a[7] += 4; }
                ICYC(base); break;
            }
            if ((op & 0xFFF0) == 0x4E60) {                         /* MOVE USP */
                if (op & 8) g_m68k.a[reg] = g_m68k.usp; else g_m68k.usp = g_m68k.a[reg];
                ICYC(base); break;
            }
            if ((op & 0xFF80) == 0x4E80) {                         /* JSR / JMP */
                Ea e = ea_decode(mode, reg, 32);
                uint32_t t = e.addr & 0xFFFFFF;
                ICYC(base);
                if (op & 0x40) {                                    /* JMP */
                    if (depth == 0 && func_table_lookup(t)) { IEVP(t); func_table_tail(t); return; }
                    s_next = t; break;
                }
                if (func_table_lookup(t)) { IEVP(t); if (!interp_jsr(t, &depth, rst)) { IEV0(); return; } break; }
                g_m68k.a[7] -= 4; bus_write32(g_m68k.a[7], s_next); if (depth < 256) rst[depth] = s_next; depth++; s_next = t;
                break;
            }
            if ((op & 0xF1C0) == 0x41C0) {                         /* LEA */
                Ea e = ea_decode(mode, reg, 32); g_m68k.a[rx] = e.addr; ICYC(base); break;
            }
            if ((op & 0xF1C0) == 0x4180) {                         /* CHK.W */
                Ea e = ea_decode(mode, reg, 16);
                int16_t b = (int16_t)ea_read(&e, 16), v = (int16_t)g_m68k.d[rx];
                ICYC(base);
                g_m68k.flag_Z = v == 0; g_m68k.flag_V = g_m68k.flag_C = false;   /* as the generator */
                if (v < 0 || v > b) { g_m68k.flag_N = v < 0; g_m68k.pc = s_next; IEV0(); recomp_m68k_exception(6); }
                else g_cyc += 10 * 7;                                /* in bounds: 10 clocks over the table's */
                break;
            }
            if ((op & 0xFFC0) == 0x4840) {                         /* PEA / SWAP */
                if (mode == 0) { M68K_SWAP(g_m68k.d[reg]); ICYC(base); break; }
                Ea e = ea_decode(mode, reg, 32); g_m68k.a[7] -= 4; bus_write32(g_m68k.a[7], e.addr); ICYC(base); break;   /* e.addr taken before the push */
            }
            if ((op & 0xFFB8) == 0x4880) {                         /* EXT.W / EXT.L */
                if (op & 0x40) { M68K_EXT32(g_m68k.d[reg]); } else { M68K_EXT16(g_m68k.d[reg]); }
                ICYC(base); break;
            }
            if ((op & 0xFB80) == 0x4880) {                         /* MOVEM */
                uint16_t mask = fetch16();
                int msz = (op & 0x40) ? 32 : 16, step = msz / 8, n = 0;
                if (op & 0x0400) {                                  /* memory -> registers */
                    Ea e = mode == 3 ? (Ea){ EA_MEM, reg, g_m68k.a[reg], 0 } : ea_decode(mode, reg, msz);
                    uint32_t a = e.addr;
                    for (int i = 0; i < 16; i++) if (mask & (1u << i)) {
                        uint32_t v = msz == 32 ? bus_read32(a) : (uint32_t)(int32_t)(int16_t)bus_read16(a);
                        if (i < 8) g_m68k.d[i] = v; else g_m68k.a[i - 8] = v;
                        a += (uint32_t)step; n++;
                    }
                    if (mode == 3) g_m68k.a[reg] = a;
                } else if (mode == 4) {                             /* registers -> -(An), mask reversed */
                    uint32_t a = g_m68k.a[reg];
                    for (int i = 0; i < 16; i++) if (mask & (1u << i)) {
                        int r = 15 - i; a -= (uint32_t)step;
                        uint32_t v = r < 8 ? g_m68k.d[r] : g_m68k.a[r - 8];
                        if (msz == 32) bus_write32(a, v); else bus_write16(a, (uint16_t)v);
                        n++;
                    }
                    g_m68k.a[reg] = a;
                } else {
                    Ea e = ea_decode(mode, reg, msz); uint32_t a = e.addr;
                    for (int i = 0; i < 16; i++) if (mask & (1u << i)) {
                        uint32_t v = i < 8 ? g_m68k.d[i] : g_m68k.a[i - 8];
                        if (msz == 32) bus_write32(a, v); else bus_write16(a, (uint16_t)v);
                        a += (uint32_t)step; n++;
                    }
                }
                unsigned per = (unsigned)n * (msz == 32 ? 8 : 4) * 7;
                if (msz == 32) { g_cyc += per; M68K_SKIPREF(); ICYC(base); } else ICYC(base + per);
                break;
            }
            if ((op & 0xFFC0) == 0x40C0) { Ea e = ea_decode(mode, reg, 16); ea_write(&e, 16, m68k_get_sr()); ICYC(base); break; }  /* MOVE SR,<ea> */
            if ((op & 0xFFC0) == 0x44C0) { Ea e = ea_decode(mode, reg, 16); m68k_set_ccr((uint8_t)ea_read(&e, 16)); ICYC(base); break; }
            if ((op & 0xFFC0) == 0x46C0) { Ea e = ea_decode(mode, reg, 16); m68k_set_sr((uint16_t)ea_read(&e, 16)); ICYC(base); break; }
            if ((op & 0xFFC0) == 0x4AC0) {                         /* TAS / ILLEGAL */
                if (op == 0x4AFC) { report_unknown(op); g_m68k.pc = s_pc; ICYC(base); IEV0(); recomp_m68k_exception(4); break; }
                Ea e = ea_decode(mode, reg, 8); uint32_t v = ea_read(&e, 8);
                /* memory: the Mega Drive's bus never completes TAS's write-back (as the generator) */
                M68K_TST8(v); if (mode == 0) ea_write(&e, 8, v | 0x80); ICYC(base); break;
            }
            if ((op & 0xFFC0) == 0x4800) {                         /* NBCD */
                Ea e = ea_decode(mode, reg, 8); uint32_t v = ea_read(&e, 8);
                M68K_NBCD(v); ea_write(&e, 8, v); ICYC(base); break;
            }
            if (sz2 != 3) {
                int k = (op >> 8) & 0xF;                            /* 0 NEGX, 2 CLR, 4 NEG, 6 NOT, A TST */
                Ea e = ea_decode(mode, reg, sz);
                if (k == 2) { ea_write(&e, sz, 0); g_m68k.flag_N = false; g_m68k.flag_Z = true; g_m68k.flag_V = false; g_m68k.flag_C = false; ICYC(base); break; }
                uint32_t v = ea_read(&e, sz);
                if (k == 0xA) { if (sz == 8) { M68K_TST8(v); } else if (sz == 16) { M68K_TST16(v); } else { M68K_TST32(v); } ICYC(base); break; }
                if (k == 0) { if (sz == 8) { M68K_NEGX8(v); } else if (sz == 16) { M68K_NEGX16(v); } else { M68K_NEGX32(v); } }
                else if (k == 4) { if (sz == 8) { M68K_NEG8(v); } else if (sz == 16) { M68K_NEG16(v); } else { M68K_NEG32(v); } }
                else if (k == 6) { if (sz == 8) { M68K_NOT8(v); } else if (sz == 16) { M68K_NOT16(v); } else { M68K_NOT32(v); } }
                else { report_unknown(op); g_m68k.pc = s_pc; ICYC(base); IEV0(); recomp_m68k_exception(4); break; }
                ea_write(&e, sz, v); ICYC(base); break;
            }
            report_unknown(op); g_m68k.pc = s_pc; ICYC(base); IEV0(); recomp_m68k_exception(4); break;
        }
        case 0x5: {
            if (sz2 == 3) {
                int cc = (op >> 8) & 0xF;
                if (mode == 1) {                                     /* DBcc */
                    uint32_t t = s_next + (uint32_t)(int32_t)(int16_t)fetch16();
                    if (!cond(cc)) {
                        uint16_t c = (uint16_t)(g_m68k.d[reg] - 1);
                        g_m68k.d[reg] = (g_m68k.d[reg] & 0xFFFF0000u) | c;
                        if (c != 0xFFFF) { ICYC(base - 14); s_next = t; break; }
                        ICYC(base + 14);
                    } else ICYC(base);
                    break;
                }
                Ea e = ea_decode(mode, reg, 8);                      /* Scc */
                bool t = cond(cc);
                ea_write(&e, 8, t ? 0xFF : 0);
                ICYC(base);
                if (mode == 0 && t && cc) ICYC(14);              /* ST Dn: 6 in the table already */
                break;
            }
            unsigned q = rx ? (unsigned)rx : 8;                      /* ADDQ / SUBQ */
            if (mode == 1) { if (op & 0x100) g_m68k.a[reg] -= q; else g_m68k.a[reg] += q; ICYC(base); break; }
            Ea e = ea_decode(mode, reg, sz);
            uint32_t r = alu((op & 0x100) ? 2 : 3, sz, ea_read(&e, sz), q);
            ea_write(&e, sz, r); ICYC(base); break;
        }
        case 0x6: {                                                  /* Bcc / BRA / BSR */
            int cc = (op >> 8) & 0xF;
            uint32_t b = s_next;
            int32_t dsp = (int8_t)(op & 0xFF);
            if (dsp == 0) dsp = (int16_t)fetch16();
            uint32_t t = (b + (uint32_t)dsp) & 0xFFFFFF;
            if (cc == 1) {                                           /* BSR */
                ICYC(base);
                if (func_table_lookup(t)) { IEVP(t); if (!interp_jsr(t, &depth, rst)) { IEV0(); return; } break; }
                g_m68k.a[7] -= 4; bus_write32(g_m68k.a[7], s_next); if (depth < 256) rst[depth] = s_next; depth++; s_next = t; break;
            }
            if (cc == 0 || cond(cc)) {
                ICYC(base);
                if (depth == 0 && t != entry && func_table_lookup(t)) { IEVP(t); func_table_tail(t); return; }
                s_next = t;
            } else ICYC(base + ((op & 0xFF) == 0 ? 14 : -14));
            break;
        }
        case 0x7:                                                    /* MOVEQ */
            g_m68k.d[rx] = (uint32_t)(int32_t)(int8_t)(op & 0xFF);
            g_m68k.flag_V = false; g_m68k.flag_C = false; set_nz(g_m68k.d[rx], 32);
            ICYC(base); break;
        case 0x8: case 0xC: {                                        /* OR / DIV / SBCD ; AND / MUL / ABCD / EXG */
            if (sz2 == 3) {
                Ea e = ea_decode(mode, reg, 16); uint32_t s = ea_read(&e, 16);
                if (hi == 0x8) {
                    bool zero = (uint16_t)s == 0;
                    if (op & 0x100) { M68K_DIVS(g_m68k.d[rx], s); } else { M68K_DIVU(g_m68k.d[rx], s); }
                    ICYC(base); (void)zero;
                } else {
                    if (op & 0x100) { M68K_MULS(g_m68k.d[rx], s); } else { M68K_MULU(g_m68k.d[rx], s); }
                    ICYC(base);
                }
                break;
            }
            if ((op & 0x1F0) == 0x100) {                             /* SBCD / ABCD */
                if (op & 8) {
                    int st = reg == 7 ? 2 : 1, dt = rx == 7 ? 2 : 1;
                    g_m68k.a[reg] -= (uint32_t)st; uint32_t s = bus_read8(g_m68k.a[reg]);
                    g_m68k.a[rx] -= (uint32_t)dt; uint32_t d = bus_read8(g_m68k.a[rx]);
                    if (hi == 0x8) { M68K_SBCD(d, s); } else { M68K_ABCD(d, s); }
                    bus_write8(g_m68k.a[rx], (uint8_t)d);
                } else {
                    uint32_t d = g_m68k.d[rx];
                    if (hi == 0x8) { M68K_SBCD(d, g_m68k.d[reg]); } else { M68K_ABCD(d, g_m68k.d[reg]); }
                    g_m68k.d[rx] = (g_m68k.d[rx] & 0xFFFFFF00u) | (d & 0xFF);
                }
                ICYC(base); break;
            }
            if (hi == 0xC && (op & 0x130) == 0x100 && ((op >> 3) & 0x1F) >= 8) {   /* EXG */
                int m = (op >> 3) & 0x1F;
                uint32_t *x = m == 0x08 ? &g_m68k.d[rx] : m == 0x09 ? &g_m68k.a[rx] : &g_m68k.d[rx];
                uint32_t *y = m == 0x08 ? &g_m68k.d[reg] : &g_m68k.a[reg];
                uint32_t t = *x; *x = *y; *y = t; ICYC(base); break;
            }
            Ea e = ea_decode(mode, reg, sz);
            int aop = hi == 0x8 ? 0 : 1;
            if (op & 0x100) { uint32_t r = alu(aop, sz, ea_read(&e, sz), g_m68k.d[rx]); ea_write(&e, sz, r); }
            else { uint32_t r = alu(aop, sz, g_m68k.d[rx], ea_read(&e, sz)); Ea d = { EA_D, rx, 0, 0 }; ea_write(&d, sz, r); }
            ICYC(base); break;
        }
        case 0x9: case 0xD: {                                        /* SUB / ADD (A, X) */
            int add = hi == 0xD;
            if (sz2 == 3) {                                          /* SUBA / ADDA */
                int asz = (op & 0x100) ? 32 : 16;
                Ea e = ea_decode(mode, reg, asz); uint32_t s = ea_read(&e, asz);
                if (asz == 16) s = (uint32_t)(int32_t)(int16_t)s;
                g_m68k.a[rx] = add ? g_m68k.a[rx] + s : g_m68k.a[rx] - s;
                ICYC(base); break;
            }
            if ((op & 0x130) == 0x100) {                             /* SUBX / ADDX */
                if (op & 8) {
                    int st = size_bytes(sz), ss = (reg == 7 && st == 1) ? 2 : st, ds = (rx == 7 && st == 1) ? 2 : st;
                    g_m68k.a[reg] -= (uint32_t)ss; uint32_t s = sz == 32 ? bus_read32(g_m68k.a[reg]) : sz == 16 ? bus_read16(g_m68k.a[reg]) : bus_read8(g_m68k.a[reg]);
                    g_m68k.a[rx] -= (uint32_t)ds; uint32_t d = sz == 32 ? bus_read32(g_m68k.a[rx]) : sz == 16 ? bus_read16(g_m68k.a[rx]) : bus_read8(g_m68k.a[rx]);
                    d = alu(add ? 7 : 8, sz, d, s);
                    Ea w = { EA_MEM, rx, g_m68k.a[rx] & 0xFFFFFF, 0 }; ea_write(&w, sz, d);
                } else {
                    Ea dd = { EA_D, rx, 0, 0 };
                    ea_write(&dd, sz, alu(add ? 7 : 8, sz, ea_read(&dd, sz), g_m68k.d[reg] & (sz == 32 ? 0xFFFFFFFFu : sz == 16 ? 0xFFFF : 0xFF)));
                }
                ICYC(base); break;
            }
            Ea e = ea_decode(mode, reg, sz);
            if (op & 0x100) { uint32_t r = alu(add ? 3 : 2, sz, ea_read(&e, sz), g_m68k.d[rx]); ea_write(&e, sz, r); }
            else { Ea d = { EA_D, rx, 0, 0 }; uint32_t r = alu(add ? 3 : 2, sz, ea_read(&d, sz), ea_read(&e, sz)); ea_write(&d, sz, r); }
            ICYC(base); break;
        }
        case 0xB: {                                                  /* CMP / CMPA / EOR / CMPM */
            if (sz2 == 3) {
                int asz = (op & 0x100) ? 32 : 16;
                Ea e = ea_decode(mode, reg, asz); uint32_t s = ea_read(&e, asz);
                if (asz == 16) s = (uint32_t)(int32_t)(int16_t)s;
                uint32_t d = g_m68k.a[rx]; M68K_CMP32(d, s); ICYC(base); break;
            }
            if ((op & 0x138) == 0x108) {                             /* CMPM (Ay)+,(Ax)+ */
                Ea s = ea_decode(3, reg, sz), d = ea_decode(3, rx, sz);
                alu(6, sz, ea_read(&d, sz), ea_read(&s, sz)); ICYC(base); break;
            }
            Ea e = ea_decode(mode, reg, sz);
            if (op & 0x100) { uint32_t r = alu(5, sz, ea_read(&e, sz), g_m68k.d[rx]); ea_write(&e, sz, r); }
            else { Ea d = { EA_D, rx, 0, 0 }; alu(6, sz, ea_read(&d, sz), ea_read(&e, sz)); }
            ICYC(base); break;
        }
        case 0xE: {                                                  /* shifts / rotates */
            if (sz2 == 3) {                                          /* memory, by one */
                int kind = (op >> 9) & 3, left = (op >> 8) & 1;
                Ea e = ea_decode(mode, reg, 16);
                ea_write(&e, 16, shift(kind, left, 16, ea_read(&e, 16), 1));
                ICYC(base); break;
            }
            int kind = (op >> 3) & 3, left = (op >> 8) & 1;
            unsigned cnt; unsigned extra = 0;
            if (op & 0x20) { cnt = g_m68k.d[rx] & 63; if (cnt) { g_cyc += cnt * 14; M68K_SKIPREF(); } }
            else { cnt = rx ? (unsigned)rx : 8; extra = cnt * 14; }
            Ea d = { EA_D, reg, 0, 0 };
            ea_write(&d, sz, shift(kind, left, sz, ea_read(&d, sz), cnt));
            ICYC(base + extra); break;
        }
        default:                                                     /* line A / line F */
            report_unknown(op); g_m68k.pc = s_pc; ICYC(base);
            IEV0(); recomp_m68k_exception(hi == 0xA ? 10 : 11);
            break;
        }
    }
}

long m68k_interp_entered(void) { return s_entered; }
long m68k_interp_instructions(void) { return s_insns; }
