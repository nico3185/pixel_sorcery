/*
 * PixelSorcery Z80 runtime for statically recompiled sound drivers.
 *
 * Generated code (engine/tools/z80recomp/z80recomp.py) is one C function per driver in
 * which every Z80 address is a label. This header supplies the register file, flag
 * arithmetic (as inline C) and the control-flow macros the generator emits. Memory and
 * port accesses go to ps_audio.c (Z80 RAM, YM2612, PSG, bank register, 68K window).
 * Time is counted in Genesis master clocks (Z80 T-state = 15 master clocks).
 */
#ifndef PS_Z80RT_H
#define PS_Z80RT_H

#include <stdint.h>

typedef struct PsZ80 {
    uint8_t a, f, b, c, d, e, h, l;
    uint8_t a_, f_, b_, c_, d_, e_, h_, l_;
    uint8_t ixh, ixl, iyh, iyl, i, r;
    uint16_t sp, pc;
    uint8_t iff1, iff2, im, ei_block, halted;
    uint8_t irq_line;               /* /INT asserted (VBlank) */
    int64_t cyc, cyc_end;           /* master clocks */
    uint8_t *ram;                   /* 8 KB Z80 RAM (aliases the hardware-side buffer) */
    uint16_t bank;                  /* 68K window = bank << 15 */
    uint32_t misses;
    void *user;
} PsZ80;

enum { FC = 0x01, FN = 0x02, FP = 0x04, FV = 0x04, F3 = 0x08, FH = 0x10, F5 = 0x20, FZ = 0x40, FS = 0x80 };

extern uint8_t ps_z80_sz53[256], ps_z80_sz53p[256], ps_z80_parity[256];
void ps_z80rt_init_tables(void);

/* provided by ps_audio.c */
uint8_t ps_z80_rd_io(PsZ80 *z, uint16_t a);
void ps_z80_wr_io(PsZ80 *z, uint16_t a, uint8_t v);
void ps_z80_miss(PsZ80 *z);
extern int ps_z80_trace_on;          /* PS_Z80_TRACE=path: log PC + registers per instruction */
void ps_z80_trace(PsZ80 *z, uint16_t pc);

static inline uint8_t ps_z80_rd(PsZ80 *z, uint16_t a)
{
    return a < 0x4000 ? z->ram[a & 0x1FFF] : ps_z80_rd_io(z, a);
}
extern int ps_z80_watch;                 /* PS_Z80_WATCH=addr: report writes there (debug) */
void ps_z80_watch_hit(PsZ80 *z, uint16_t a, uint8_t v);
static inline void ps_z80_wr(PsZ80 *z, uint16_t a, uint8_t v)
{
    if (ps_z80_watch >= 0 && ((a & 0x1FFF) == ps_z80_watch || (a & 0x1FFF) == ps_z80_watch + 1))
        ps_z80_watch_hit(z, a, v);
    if (a < 0x4000) z->ram[a & 0x1FFF] = v; else ps_z80_wr_io(z, a, v);
}

#define RD(a)    ps_z80_rd(z, (uint16_t)(a))
#define WR(a, v) ps_z80_wr(z, (uint16_t)(a), (uint8_t)(v))
#define IN(p)    ((uint8_t)0xFF)
#define OUT(p, v) ((void)(p), (void)(v))
#define CYC(t)   (z->cyc += (t) * 15)

/* instruction boundary: EI shadow, interrupt, cycle budget */
/* Slice budget first, so an EI shadow survives a slice boundary (as in GPGX's z80_run). */
#define CHK(pc_) do { \
    if (z->cyc >= z->cyc_end) { z->pc = (pc_); return; } \
    if (z->ei_block) z->ei_block = 0; \
    else if (z->irq_line && z->iff1) { z->pc = (pc_); goto take_int; } \
    if (ps_z80_trace_on) ps_z80_trace(z, (pc_)); \
} while (0)
#define JUMPX(t) do { z->pc = (uint16_t)(t); goto dispatch; } while (0)
#define RET()    JUMPX(POP16(z))
#define HALT(next) do { z->pc = (next); z->halted = 1; goto halted; } while (0)
#define TAKE_INT() do { z->halted = 0; z->iff1 = z->iff2 = 0; \
    PUSH16(z, z->pc); z->pc = (z->im == 2) ? (uint16_t)(RD((z->i << 8) | 0xFF) | (RD(((z->i << 8) | 0xFF) + 1) << 8)) : 0x0038; \
    CYC(13); } while (0)

#define GET_BC(z) ((uint16_t)(((z)->b << 8) | (z)->c))
#define GET_DE(z) ((uint16_t)(((z)->d << 8) | (z)->e))
#define GET_HL(z) ((uint16_t)(((z)->h << 8) | (z)->l))
#define GET_AF(z) ((uint16_t)(((z)->a << 8) | (z)->f))
#define GET_IX(z) ((uint16_t)(((z)->ixh << 8) | (z)->ixl))
#define GET_IY(z) ((uint16_t)(((z)->iyh << 8) | (z)->iyl))
#define GET_SP(z) ((z)->sp)
#define SET_PAIR(hi, lo, v) do { uint16_t _v = (uint16_t)(v); (hi) = (uint8_t)(_v >> 8); (lo) = (uint8_t)_v; } while (0)
#define SET_BC(z, v) SET_PAIR((z)->b, (z)->c, v)
#define SET_DE(z, v) SET_PAIR((z)->d, (z)->e, v)
#define SET_HL(z, v) SET_PAIR((z)->h, (z)->l, v)
#define SET_AF(z, v) SET_PAIR((z)->a, (z)->f, v)
#define SET_IX(z, v) SET_PAIR((z)->ixh, (z)->ixl, v)
#define SET_IY(z, v) SET_PAIR((z)->iyh, (z)->iyl, v)
#define SET_SP(z, v) ((z)->sp = (uint16_t)(v))

static inline void PUSH16(PsZ80 *z, uint16_t v)
{
    z->sp--; WR(z->sp, v >> 8); z->sp--; WR(z->sp, v & 0xFF);
}
static inline uint16_t POP16(PsZ80 *z)
{
    uint16_t lo = RD(z->sp); z->sp++; uint16_t hi = RD(z->sp); z->sp++;
    return (uint16_t)(lo | (hi << 8));
}

static const uint8_t ps_hc_add[8] = { 0, FH, FH, FH, 0, 0, 0, FH };
static const uint8_t ps_hc_sub[8] = { 0, 0, FH, 0, FH, 0, FH, FH };
static const uint8_t ps_ov_add[8] = { 0, 0, 0, FV, FV, 0, 0, 0 };
static const uint8_t ps_ov_sub[8] = { 0, FV, 0, 0, 0, 0, FV, 0 };

static inline void ADD8(PsZ80 *z, uint8_t v)
{
    uint16_t t = z->a + v;
    uint8_t lk = ((z->a & 0x88) >> 3) | ((v & 0x88) >> 2) | ((t & 0x88) >> 1);
    z->a = (uint8_t)t;
    z->f = ((t & 0x100) ? FC : 0) | ps_hc_add[lk & 7] | ps_ov_add[lk >> 4] | ps_z80_sz53[z->a];
}
static inline void ADC8(PsZ80 *z, uint8_t v)
{
    uint16_t t = z->a + v + (z->f & FC);
    uint8_t lk = ((z->a & 0x88) >> 3) | ((v & 0x88) >> 2) | ((t & 0x88) >> 1);
    z->a = (uint8_t)t;
    z->f = ((t & 0x100) ? FC : 0) | ps_hc_add[lk & 7] | ps_ov_add[lk >> 4] | ps_z80_sz53[z->a];
}
static inline void SUB8(PsZ80 *z, uint8_t v)
{
    uint16_t t = (uint16_t)(z->a - v);
    uint8_t lk = ((z->a & 0x88) >> 3) | ((v & 0x88) >> 2) | ((t & 0x88) >> 1);
    z->a = (uint8_t)t;
    z->f = ((t & 0x100) ? FC : 0) | FN | ps_hc_sub[lk & 7] | ps_ov_sub[lk >> 4] | ps_z80_sz53[z->a];
}
static inline void SBC8(PsZ80 *z, uint8_t v)
{
    uint16_t t = (uint16_t)(z->a - v - (z->f & FC));
    uint8_t lk = ((z->a & 0x88) >> 3) | ((v & 0x88) >> 2) | ((t & 0x88) >> 1);
    z->a = (uint8_t)t;
    z->f = ((t & 0x100) ? FC : 0) | FN | ps_hc_sub[lk & 7] | ps_ov_sub[lk >> 4] | ps_z80_sz53[z->a];
}
static inline void CP8(PsZ80 *z, uint8_t v)
{
    uint16_t t = (uint16_t)(z->a - v);
    uint8_t lk = ((z->a & 0x88) >> 3) | ((v & 0x88) >> 2) | ((t & 0x88) >> 1);
    z->f = ((t & 0x100) ? FC : ((t & 0xFF) ? 0 : FZ)) | FN | ps_hc_sub[lk & 7] | ps_ov_sub[lk >> 4]
         | (v & (F3 | F5)) | (t & FS);
}
static inline void AND8(PsZ80 *z, uint8_t v) { z->a &= v; z->f = FH | ps_z80_sz53p[z->a]; }
static inline void XOR8(PsZ80 *z, uint8_t v) { z->a ^= v; z->f = ps_z80_sz53p[z->a]; }
static inline void OR8(PsZ80 *z, uint8_t v)  { z->a |= v; z->f = ps_z80_sz53p[z->a]; }

static inline uint8_t INC8(PsZ80 *z, uint8_t r)
{
    r++;
    z->f = (z->f & FC) | (r == 0x80 ? FV : 0) | ((r & 0x0F) ? 0 : FH) | ps_z80_sz53[r];
    return r;
}
static inline uint8_t DEC8(PsZ80 *z, uint8_t r)
{
    z->f = (z->f & FC) | ((r & 0x0F) ? 0 : FH) | FN;
    r--;
    z->f |= (r == 0x7F ? FV : 0) | ps_z80_sz53[r];
    return r;
}
static inline uint16_t ADD16(PsZ80 *z, uint16_t a, uint16_t v)
{
    uint32_t t = (uint32_t)a + v;
    uint8_t lk = ((a & 0x0800) >> 11) | ((v & 0x0800) >> 10) | ((t & 0x0800) >> 9);
    z->f = (z->f & (FV | FZ | FS)) | ((t & 0x10000) ? FC : 0) | ((t >> 8) & (F3 | F5)) | ps_hc_add[lk];
    return (uint16_t)t;
}
static inline uint16_t ADC16(PsZ80 *z, uint16_t a, uint16_t v)
{
    uint32_t t = (uint32_t)a + v + (z->f & FC);
    uint8_t lk = ((a & 0x8800) >> 11) | ((v & 0x8800) >> 10) | ((t & 0x8800) >> 9);
    uint16_t r = (uint16_t)t;
    z->f = ((t & 0x10000) ? FC : 0) | ps_ov_add[lk >> 4] | ((r >> 8) & (F3 | F5 | FS))
         | ps_hc_add[lk & 7] | (r ? 0 : FZ);
    return r;
}
static inline uint16_t SBC16(PsZ80 *z, uint16_t a, uint16_t v)
{
    uint32_t t = (uint32_t)a - v - (z->f & FC);
    uint8_t lk = ((a & 0x8800) >> 11) | ((v & 0x8800) >> 10) | ((t & 0x8800) >> 9);
    uint16_t r = (uint16_t)t;
    z->f = ((t & 0x10000) ? FC : 0) | FN | ps_ov_sub[lk >> 4] | ((r >> 8) & (F3 | F5 | FS))
         | ps_hc_sub[lk & 7] | (r ? 0 : FZ);
    return r;
}
static inline void RLCA(PsZ80 *z) { z->a = (uint8_t)((z->a << 1) | (z->a >> 7)); z->f = (z->f & (FP | FZ | FS)) | (z->a & (FC | F3 | F5)); }
static inline void RRCA(PsZ80 *z) { z->f = (z->f & (FP | FZ | FS)) | (z->a & FC); z->a = (uint8_t)((z->a >> 1) | (z->a << 7)); z->f |= z->a & (F3 | F5); }
static inline void RLA(PsZ80 *z)  { uint8_t t = z->a; z->a = (uint8_t)((z->a << 1) | (z->f & FC)); z->f = (z->f & (FP | FZ | FS)) | (z->a & (F3 | F5)) | (t >> 7); }
static inline void RRA(PsZ80 *z)  { uint8_t t = z->a; z->a = (uint8_t)((z->a >> 1) | (z->f << 7)); z->f = (z->f & (FP | FZ | FS)) | (z->a & (F3 | F5)) | (t & FC); }
static inline void DAA(PsZ80 *z)
{
    uint8_t add = 0, carry = z->f & FC;
    if ((z->f & FH) || ((z->a & 0x0F) > 9)) add = 6;
    if (carry || (z->a > 0x99)) add |= 0x60;
    if (z->a > 0x99) carry = FC;
    if (z->f & FN) SUB8(z, add); else ADD8(z, add);
    z->f = (uint8_t)((z->f & ~(FC | FP)) | carry | ps_z80_parity[z->a]);
}
static inline void CPL(PsZ80 *z) { z->a ^= 0xFF; z->f = (z->f & (FC | FP | FZ | FS)) | (z->a & (F3 | F5)) | FN | FH; }
static inline void SCF(PsZ80 *z) { z->f = (z->f & (FP | FZ | FS)) | (z->a & (F3 | F5)) | FC; }
static inline void CCF(PsZ80 *z) { z->f = (z->f & (FP | FZ | FS)) | ((z->f & FC) ? FH : FC) | (z->a & (F3 | F5)); }
static inline void NEG(PsZ80 *z) { uint8_t t = z->a; z->a = 0; SUB8(z, t); }

static inline uint8_t RLC(PsZ80 *z, uint8_t v) { v = (uint8_t)((v << 1) | (v >> 7)); z->f = (v & FC) | ps_z80_sz53p[v]; return v; }
static inline uint8_t RRC(PsZ80 *z, uint8_t v) { z->f = v & FC; v = (uint8_t)((v >> 1) | (v << 7)); z->f |= ps_z80_sz53p[v]; return v; }
static inline uint8_t RL(PsZ80 *z, uint8_t v)  { uint8_t t = v; v = (uint8_t)((v << 1) | (z->f & FC)); z->f = (t >> 7) | ps_z80_sz53p[v]; return v; }
static inline uint8_t RR(PsZ80 *z, uint8_t v)  { uint8_t t = v; v = (uint8_t)((v >> 1) | (z->f << 7)); z->f = (t & FC) | ps_z80_sz53p[v]; return v; }
static inline uint8_t SLA(PsZ80 *z, uint8_t v) { z->f = v >> 7; v = (uint8_t)(v << 1); z->f |= ps_z80_sz53p[v]; return v; }
static inline uint8_t SRA(PsZ80 *z, uint8_t v) { z->f = v & FC; v = (uint8_t)((v & 0x80) | (v >> 1)); z->f |= ps_z80_sz53p[v]; return v; }
static inline uint8_t SLL(PsZ80 *z, uint8_t v) { z->f = v >> 7; v = (uint8_t)((v << 1) | 1); z->f |= ps_z80_sz53p[v]; return v; }
static inline uint8_t SRL(PsZ80 *z, uint8_t v) { z->f = v & FC; v >>= 1; z->f |= ps_z80_sz53p[v]; return v; }

static inline void BIT(PsZ80 *z, int n, uint8_t v)
{
    z->f = (z->f & FC) | FH | (v & (F3 | F5));
    if (!(v & (1 << n))) z->f |= FP | FZ;
    if (n == 7 && (v & 0x80)) z->f |= FS;
}
static inline void BITM(PsZ80 *z, int n, uint8_t v, uint8_t hi)
{
    z->f = (z->f & FC) | FH | (hi & (F3 | F5));
    if (!(v & (1 << n))) z->f |= FP | FZ;
    if (n == 7 && (v & 0x80)) z->f |= FS;
}
static inline void RLD(PsZ80 *z)
{
    uint16_t hl = GET_HL(z); uint8_t t = RD(hl);
    WR(hl, (uint8_t)((t << 4) | (z->a & 0x0F)));
    z->a = (uint8_t)((z->a & 0xF0) | (t >> 4));
    z->f = (z->f & FC) | ps_z80_sz53p[z->a];
}
static inline void RRD(PsZ80 *z)
{
    uint16_t hl = GET_HL(z); uint8_t t = RD(hl);
    WR(hl, (uint8_t)((z->a << 4) | (t >> 4)));
    z->a = (uint8_t)((z->a & 0xF0) | (t & 0x0F));
    z->f = (z->f & FC) | ps_z80_sz53p[z->a];
}
static inline void LDAIR(PsZ80 *z) { z->f = (z->f & FC) | ps_z80_sz53[z->a] | (z->iff2 ? FV : 0); }
static inline uint8_t INF(PsZ80 *z, uint8_t v) { z->f = (z->f & FC) | ps_z80_sz53p[v]; return v; }

static inline void LDX(PsZ80 *z, int dir)
{
    uint8_t t = RD(GET_HL(z));
    WR(GET_DE(z), t);
    SET_HL(z, GET_HL(z) + dir); SET_DE(z, GET_DE(z) + dir); SET_BC(z, GET_BC(z) - 1);
    t += z->a;
    z->f = (z->f & (FC | FZ | FS)) | (GET_BC(z) ? FV : 0) | (t & F3) | ((t & 0x02) ? F5 : 0);
}
static inline void CPX(PsZ80 *z, int dir)
{
    uint8_t v = RD(GET_HL(z)), r = (uint8_t)(z->a - v);
    uint8_t lk = ((z->a & 0x08) >> 3) | ((v & 0x08) >> 2) | ((r & 0x08) >> 1);
    SET_HL(z, GET_HL(z) + dir); SET_BC(z, GET_BC(z) - 1);
    z->f = (z->f & FC) | (GET_BC(z) ? (FV | FN) : FN) | ps_hc_sub[lk] | (r ? 0 : FZ) | (r & FS);
    if (z->f & FH) r--;
    z->f |= (r & F3) | ((r & 0x02) ? F5 : 0);
}
static inline void INX(PsZ80 *z, int dir)
{
    WR(GET_HL(z), 0xFF); SET_HL(z, GET_HL(z) + dir); z->b--;
    z->f = (z->b ? 0 : FZ) | FN;
}
static inline void OUTX(PsZ80 *z, int dir)
{
    (void)RD(GET_HL(z)); SET_HL(z, GET_HL(z) + dir); z->b--;
    z->f = (z->b ? 0 : FZ) | FN;
}
static inline void EX_AF(PsZ80 *z) { uint8_t t = z->a; z->a = z->a_; z->a_ = t; t = z->f; z->f = z->f_; z->f_ = t; }
static inline void EX_DEHL(PsZ80 *z) { uint8_t t = z->d; z->d = z->h; z->h = t; t = z->e; z->e = z->l; z->l = t; }
static inline void EXX(PsZ80 *z)
{
    uint8_t t;
    t = z->b; z->b = z->b_; z->b_ = t; t = z->c; z->c = z->c_; z->c_ = t;
    t = z->d; z->d = z->d_; z->d_ = t; t = z->e; z->e = z->e_; z->e_ = t;
    t = z->h; z->h = z->h_; z->h_ = t; t = z->l; z->l = z->l_; z->l_ = t;
}

#endif
