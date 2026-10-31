/* ps_synth.c — PixelSorcery's own YM2612 (OPN2) + SN76489 (PSG) synthesis.
 *
 * Written from public hardware documentation (Yamaha OPN/OPN2 manuals, Sega Genesis
 * technical notes, community die-shot research). No emulator source is used.
 *
 * Timing model: the chips run lazily. Every FM data write, status read, reset and frame
 * end first renders FM samples up to that master cycle (one sample per 1008 master clocks,
 * i.e. 7.67 MHz / 144 = 53.267 kHz). The status register (timers, busy flag) is therefore
 * exact at read time, which the sound driver relies on. The batch granularity of timer B
 * deliberately matches the oracle so the driver's write stream stays identical.
 *
 * Output: FM samples + PSG (averaged over each FM sample window) are mixed and resampled
 * to the host rate at frame end.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#ifndef M_PI  /* MSVC's math.h only defines this with _USE_MATH_DEFINES */
#define M_PI 3.14159265358979323846
#endif
#include "ps_synth.h"

#define FM_RATIO   1008          /* master clocks per FM sample (7 * 144) */
#define BUSY_RATIO 42            /* master clocks per YM clock (7 * 6) */
#define PSG_RATIO  240           /* master clocks per PSG tone tick (15 * 16) */
#define MAX_FM     4096          /* FM samples buffered per frame */

/* ------------------------------------------------------------------ tables */
static uint16_t s_logsin[256];   /* -log2(sin) quarter wave, 4.8 fixed point */
static uint16_t s_exp[256];      /* 2^x mantissa, 10 bits */
static int s_psg_vol[16];

/* Detune in phase-increment units, by keycode (0-31) and |DT| (1-3). YM2612 datasheet. */
static const uint8_t s_dt[4][32] = {
    {0},
    {0,0,0,0,1,1,1,1,1,1,1,1,2,2,2,2,2,3,3,3,4,4,4,5,5,6,6,7,8,8,8,8},
    {1,1,1,1,2,2,2,2,2,3,3,3,4,4,4,5,5,6,6,7,8,8,9,10,11,12,13,14,16,16,16,16},
    {2,2,2,2,2,3,3,3,4,4,4,5,5,6,6,7,8,8,9,10,11,12,13,14,16,17,19,20,22,22,22,22},
};
/* Envelope increment patterns: rows by rate & 3, 8 steps of the global EG counter. */
static const uint8_t s_eg_pat[4][8] = {
    {0,1,0,1,0,1,0,1}, {0,1,0,1,1,1,0,1}, {0,1,1,1,0,1,1,1}, {0,1,1,1,1,1,1,1},
};
static const uint8_t s_eg_pat_hi[4][8] = {
    {1,1,1,1,1,1,1,1}, {1,1,1,2,1,1,1,2}, {1,2,1,2,1,2,1,2}, {1,2,2,2,1,2,2,2},
};
/* LFO: samples per step for each frequency setting (3.98 .. 72.2 Hz over 128 steps). */
static const uint8_t s_lfo_len[8] = {108, 77, 71, 67, 62, 44, 8, 5};
/* LFO PM depth in cents per FMS setting. */
static const float s_fms_cents[8] = {0, 3.4f, 6.7f, 10, 14, 20, 40, 80};

static void init_tables(void)
{
    for (int i = 0; i < 256; i++) {
        double s = sin((i + 0.5) * M_PI / 512.0);
        s_logsin[i] = (uint16_t)lround(-log2(s) * 256.0);
        s_exp[i] = (uint16_t)lround((pow(2.0, i / 256.0) - 1.0) * 1024.0);
    }
    for (int i = 0; i < 15; i++) s_psg_vol[i] = (int)lround(2200.0 * pow(10.0, -0.1 * i)); /* 2 dB steps */
    s_psg_vol[15] = 0;
}

/* ------------------------------------------------------------------ YM2612 state */
enum { EG_ATT, EG_DEC, EG_SUS, EG_REL };

typedef struct {
    /* registers */
    int dt, mul, tl, ks, ar, d1r, d2r, d1l, rr, am, ssg;
    /* runtime */
    uint32_t phase, inc;
    int env, state, key, ssg_inv;
    int fnum, block, kc;         /* frequency source (ch3 special mode: per operator) */
} Op;

typedef struct {
    Op op[4];                    /* S1, S2, S3, S4 (register offsets +0, +8, +4, +C) */
    int fnum, block, kc, latch;
    int alg, fb, pan_l, pan_r, ams, fms;
    int fb1, fb2;                /* S1's last two outputs (feedback) */
} Ch;

static struct {
    Ch ch[6];
    int addr;                    /* single address latch; bit 8 = part II (set by the port written) */
    int ch3_fnum[3], ch3_block[3], ch3_latch;
    int mode;                    /* reg 0x27 */
    int ta, tb, tac, tbc, status;
    int lfo_on, lfo_freq, lfo_cnt, lfo_step;
    int dac_on, dac;
    unsigned eg_cnt, eg_div;
    int csm_off;                 /* CSM key-on to release after one sample */
    /* timing (master clocks, relative to the current frame) */
    int64_t pos;                 /* time of the next FM sample */
    int64_t frame_start;         /* time of the first FM sample of this frame */
    int64_t busy_end;
    int n;                       /* FM samples rendered this frame */
    int32_t buf[MAX_FM][2];
} Y;

/* ------------------------------------------------------------------ PSG state */
static struct {
    int period[3], tone_cnt[3], tone_out[3];
    int vol[4];
    int noise_ctl, noise_cnt, noise_ff, lfsr;
    int latch_ch, latch_vol;
    int64_t t;                   /* time of the next tick */
    int64_t ev_t[1024]; uint8_t ev_v[1024]; int nev;
} P;

int ps_synth_dbg;
static int32_t s_psg_acc[MAX_FM]; static uint16_t s_psg_cnt[MAX_FM];

/* ------------------------------------------------------------------ YM helpers */
static int keycode(int fnum, int block)
{
    int f11 = (fnum >> 10) & 1, f10 = (fnum >> 9) & 1, f9 = (fnum >> 8) & 1, f8 = (fnum >> 7) & 1;
    int n3 = (f11 & (f10 | f9 | f8)) | ((f11 ^ 1) & f10 & f9 & f8);
    return (block << 2) | (f11 << 1) | n3;
}

static void op_freq(Op *o, int fnum, int block)
{
    o->fnum = fnum; o->block = block; o->kc = keycode(fnum, block);
}

static void ch_freq(int c)
{
    Ch *ch = &Y.ch[c];
    ch->kc = keycode(ch->fnum, ch->block);
    if (c == 2 && (Y.mode & 0xC0)) {
        op_freq(&ch->op[0], Y.ch3_fnum[1], Y.ch3_block[1]);   /* $A9 -> S1 */
        op_freq(&ch->op[1], Y.ch3_fnum[2], Y.ch3_block[2]);   /* $AA -> S2 */
        op_freq(&ch->op[2], Y.ch3_fnum[0], Y.ch3_block[0]);   /* $A8 -> S3 */
        op_freq(&ch->op[3], ch->fnum, ch->block);
    } else {
        for (int i = 0; i < 4; i++) op_freq(&ch->op[i], ch->fnum, ch->block);
    }
}

static uint32_t phase_inc(const Op *o, int pm_cents)
{
    int fnum = o->fnum;
    if (pm_cents) fnum = (int)(fnum * exp2f(pm_cents / 120000.0f));   /* pm in 1/100 cents */
    int base = ((fnum << o->block) >> 1);
    int d = s_dt[o->dt & 3][o->kc];
    base = (o->dt & 4) ? base - d : base + d;
    base &= 0x1FFFF;
    uint32_t inc = o->mul ? (uint32_t)base * o->mul : (uint32_t)base >> 1;
    return inc & 0xFFFFF;
}

static int eg_rate(const Op *o, int r)
{
    if (r == 0) return 0;
    int rate = r + (o->kc >> (3 - o->ks));
    return rate > 63 ? 63 : rate;
}

static int eg_inc(int rate, unsigned cnt)
{
    if (rate < 2) return 0;
    if (rate < 48) {
        int sh = 11 - (rate >> 2);
        if (cnt & ((1u << sh) - 1)) return 0;
        return s_eg_pat[rate & 3][(cnt >> sh) & 7];
    }
    if (rate >= 60) return 8;
    return s_eg_pat_hi[rate & 3][cnt & 7] << ((rate - 48) >> 2);
}

static int sustain_level(const Op *o) { return o->d1l == 15 ? 0x3E0 : o->d1l << 5; }

static void key_on(Op *o)
{
    if (o->key) return;
    o->key = 1;
    o->phase = 0;
    o->ssg_inv = 0;
    if (eg_rate(o, o->ar * 2) >= 62) { o->env = 0; o->state = o->d1l ? EG_DEC : EG_SUS; }
    else o->state = EG_ATT;
}

static void key_off(Op *o)
{
    if (!o->key) return;
    o->key = 0;
    if ((o->ssg & 8) && (o->ssg_inv ^ ((o->ssg >> 2) & 1)))   /* freeze the inverted level */
        o->env = (0x200 - o->env) & 0x3FF;
    o->ssg_inv = 0;
    o->state = EG_REL;
}

static void eg_ssg(Op *o)
{
    if (!(o->ssg & 8) || o->env < 0x200 || o->state == EG_REL) return;
    if (o->ssg & 1) {                                   /* hold */
        if (o->ssg & 2) o->ssg_inv = 1;
        if (o->state != EG_ATT && !(o->ssg_inv ^ ((o->ssg >> 2) & 1))) o->env = 0x3FF;
    } else {                                            /* repeat */
        if (o->ssg & 2) o->ssg_inv ^= 1; else o->phase = 0;
        if (o->state != EG_ATT) {
            if (eg_rate(o, o->ar * 2) >= 62) { o->env = 0; o->state = EG_DEC; }
            else o->state = EG_ATT;
        }
    }
}

static void eg_step(Op *o)
{
    int r, inc;
    switch (o->state) {
    case EG_ATT:
        r = eg_rate(o, o->ar * 2);
        if (r >= 62) { o->env = 0; }
        else if ((inc = eg_inc(r, Y.eg_cnt))) o->env += ((~o->env) * inc) >> 4;
        if (o->env <= 0) { o->env = 0; o->state = EG_DEC; }
        break;
    case EG_DEC:
        r = eg_rate(o, o->d1r * 2);
        inc = eg_inc(r, Y.eg_cnt);
        if ((o->ssg & 8) && o->env < 0x200) inc <<= 2; else if (o->ssg & 8) inc = 0;
        o->env += inc;
        if (o->env >= sustain_level(o)) o->state = EG_SUS;
        break;
    case EG_SUS:
        r = eg_rate(o, o->d2r * 2);
        inc = eg_inc(r, Y.eg_cnt);
        if ((o->ssg & 8) && o->env < 0x200) inc <<= 2; else if (o->ssg & 8) inc = 0;
        o->env += inc;
        break;
    case EG_REL:
        r = eg_rate(o, o->rr * 4 + 2);
        inc = eg_inc(r, Y.eg_cnt);
        if ((o->ssg & 8) && o->env < 0x200) inc <<= 2;
        o->env += inc;
        break;
    }
    if (o->env > 0x3FF) o->env = 0x3FF;
}

/* One operator: phase (10 bits) + modulation, attenuation (EG + TL + AM) -> 14-bit signed. */
static inline int op_out(Op *o, int mod, int am_att)
{
    int env = o->env;
    if ((o->ssg & 8) && (o->ssg_inv ^ ((o->ssg >> 2) & 1)) && o->state != EG_REL)
        env = (0x200 - env) & 0x3FF;
    int att = env + (o->tl << 3) + (o->am ? am_att : 0);
    if (att > 0x3FF) att = 0x3FF;
    int ph = ((int)(o->phase >> 10) + mod) & 0x3FF;
    int q = ph & 0xFF; if (ph & 0x100) q ^= 0xFF;
    int l = s_logsin[q] + (att << 2);
    int sh = l >> 8;
    int v = sh >= 13 ? 0 : (((s_exp[(l & 0xFF) ^ 0xFF] | 0x400) << 2) >> sh);
    return (ph & 0x200) ? -v : v;
}

static void render_sample(void)
{
    /* LFO */
    int am_raw = 0, pm_tri = 0;
    if (Y.lfo_on) {
        if (++Y.lfo_cnt >= s_lfo_len[Y.lfo_freq]) { Y.lfo_cnt = 0; Y.lfo_step = (Y.lfo_step + 1) & 127; }
        am_raw = (Y.lfo_step < 64 ? Y.lfo_step ^ 63 : Y.lfo_step & 63) << 1;    /* 0..126 */
        int p = Y.lfo_step >> 2;                                                /* 32-step triangle */
        pm_tri = (p < 8) ? p : (p < 24) ? 16 - p : p - 32;                      /* -8..8 */
    }

    /* Envelopes advance every third sample. */
    if (++Y.eg_div >= 3) {
        Y.eg_div = 0;
        Y.eg_cnt = (Y.eg_cnt + 1) & 0xFFF;
        for (int c = 0; c < 6; c++)
            for (int i = 0; i < 4; i++) { eg_ssg(&Y.ch[c].op[i]); eg_step(&Y.ch[c].op[i]); }
    }

    int32_t L = 0, R = 0;
    for (int c = 0; c < 6; c++) {
        Ch *ch = &Y.ch[c];
        int out;
        static const int am_shift[4] = {8, 3, 1, 0};
        int am_att = am_raw >> am_shift[ch->ams];
        if (c == 5 && Y.dac_on) {
            out = (Y.dac - 128) << 6;
        } else {
            Op *o = ch->op;
            int fbm = ch->fb ? (ch->fb1 + ch->fb2) >> (10 - ch->fb) : 0;
            int s1 = op_out(&o[0], fbm, am_att), s2, s3, s4;
            ch->fb2 = ch->fb1; ch->fb1 = s1;
            switch (ch->alg) {
            case 0: s2 = op_out(&o[1], s1 >> 1, am_att); s3 = op_out(&o[2], s2 >> 1, am_att);
                    out = op_out(&o[3], s3 >> 1, am_att); break;
            case 1: s2 = op_out(&o[1], 0, am_att); s3 = op_out(&o[2], (s1 + s2) >> 1, am_att);
                    out = op_out(&o[3], s3 >> 1, am_att); break;
            case 2: s2 = op_out(&o[1], 0, am_att); s3 = op_out(&o[2], s2 >> 1, am_att);
                    out = op_out(&o[3], (s1 + s3) >> 1, am_att); break;
            case 3: s2 = op_out(&o[1], s1 >> 1, am_att); s3 = op_out(&o[2], 0, am_att);
                    out = op_out(&o[3], (s2 + s3) >> 1, am_att); break;
            case 4: s2 = op_out(&o[1], s1 >> 1, am_att); s3 = op_out(&o[2], 0, am_att);
                    out = s2 + op_out(&o[3], s3 >> 1, am_att); break;
            case 5: s2 = op_out(&o[1], s1 >> 1, am_att); s3 = op_out(&o[2], s1 >> 1, am_att);
                    out = s2 + s3 + op_out(&o[3], s1 >> 1, am_att); break;
            case 6: s2 = op_out(&o[1], s1 >> 1, am_att); s3 = op_out(&o[2], 0, am_att);
                    out = s2 + s3 + op_out(&o[3], 0, am_att); break;
            default: s2 = op_out(&o[1], 0, am_att); s3 = op_out(&o[2], 0, am_att);
                    out = s1 + s2 + s3 + op_out(&o[3], 0, am_att); break;
            }
            /* phase generators */
            int pm = (ch->fms && pm_tri) ? (int)(s_fms_cents[ch->fms] * 100 * pm_tri / 8) : 0;
            for (int i = 0; i < 4; i++) {
                if (o[i].inc == 0xFFFFFFFF) o[i].inc = phase_inc(&o[i], 0);
                o[i].phase = (o[i].phase + (pm ? phase_inc(&o[i], pm) : o[i].inc)) & 0xFFFFF;
            }
        }
        if (out > 8191) out = 8191; else if (out < -8192) out = -8192;
        /* 14-bit precision kept (the 9-bit DAC quantisation would only add noise). Discrete
         * YM2612 "ladder effect": +4 on non-negative outputs whether panned or not, -4 on
         * negative ones (-3 when panned), in 9-bit DAC units. */
        if (ch->pan_l) L += out;
        if (ch->pan_r) R += out;
        if (out >= 0) { L += 4 << 5; R += 4 << 5; }
        else { L -= (4 - ch->pan_l) << 5; R -= (4 - ch->pan_r) << 5; }
    }
    if (Y.n < MAX_FM) { Y.buf[Y.n][0] = L; Y.buf[Y.n][1] = R; Y.n++; }

    if (Y.csm_off) {                                  /* CSM key-on lasts one sample */
        Y.csm_off = 0;
        for (int i = 0; i < 4; i++) if (!(Y.ch[2].op[i].key & 2)) key_off(&Y.ch[2].op[i]);
    }
    /* timer A ticks every sample */
    if (Y.mode & 1) {
        if (--Y.tac <= 0) {
            if (Y.mode & 4) Y.status |= 1;
            Y.tac = 1024 - Y.ta;
            if ((Y.mode & 0xC0) == 0x80) {
                for (int i = 0; i < 4; i++) key_on(&Y.ch[2].op[i]);
                Y.csm_off = 1;
            }
        }
    }
}

/* Render FM samples whose start time is before `cycles`; timer B counts per batch. */
static void advance(int64_t cycles)
{
    int n = 0;
    while (Y.pos < cycles) { render_sample(); Y.pos += FM_RATIO; n++; }
    if (n && (Y.mode & 2)) {
        Y.tbc -= n;
        if (Y.tbc <= 0) {
            if (Y.mode & 8) Y.status |= 2;
            if (ps_synth_dbg) fprintf(stderr, "  ym TB flag pos=%lld batch=%d\n", (long long)Y.pos, n);
            do Y.tbc += (256 - Y.tb) << 4; while (Y.tbc <= 0);
        }
    }
}

static void write_op(int c, int slot, int reg, int v)
{
    static const int map[4] = {0, 2, 1, 3};
    Op *o = &Y.ch[c].op[map[slot]];
    switch (reg & 0xF0) {
    case 0x30: o->dt = (v >> 4) & 7; o->mul = v & 15; o->inc = 0xFFFFFFFF; break;
    case 0x40: o->tl = v & 0x7F; break;
    case 0x50: o->ks = v >> 6; o->ar = v & 31; break;
    case 0x60: o->am = v >> 7; o->d1r = v & 31; break;
    case 0x70: o->d2r = v & 31; break;
    case 0x80: o->d1l = v >> 4; o->rr = v & 15; break;
    case 0x90: o->ssg = v & 15; break;
    }
}

static void invalidate(int c) { for (int i = 0; i < 4; i++) Y.ch[c].op[i].inc = 0xFFFFFFFF; }

static void write_reg(int part, int r, int v)
{
    if (r < 0x30) {
        if (part) return;
        switch (r) {
        case 0x22: Y.lfo_on = (v >> 3) & 1; Y.lfo_freq = v & 7; if (!Y.lfo_on) { Y.lfo_step = 0; Y.lfo_cnt = 0; } break;
        case 0x24: Y.ta = (Y.ta & 3) | (v << 2); break;
        case 0x25: Y.ta = (Y.ta & 0x3FC) | (v & 3); break;
        case 0x26: Y.tb = v; break;
        case 0x27: {
            int old = Y.mode;
            if ((v & 1) && !(old & 1)) Y.tac = 1024 - Y.ta;
            if ((v & 2) && !(old & 2)) Y.tbc = (256 - Y.tb) << 4;
            Y.status &= (~v >> 4);
            Y.mode = v;
            if ((old ^ v) & 0xC0) { ch_freq(2); invalidate(2); }
            break;
        }
        case 0x28: {
            int c = v & 3; if (c == 3) break;
            if (v & 4) c += 3;
            for (int i = 0; i < 4; i++) {
                Op *o = &Y.ch[c].op[i];
                if (v & (0x10 << i)) { key_on(o); o->key |= 2; }
                else { if (o->key) { o->key = 1; key_off(o); } }
            }
            break;
        }
        case 0x2A: Y.dac = v; break;
        case 0x2B: Y.dac_on = v >> 7; break;
        }
        return;
    }
    int c = r & 3; if (c == 3) return;
    c += part * 3;
    if (r < 0xA0) { write_op(c, (r >> 2) & 3, r, v); return; }
    Ch *ch = &Y.ch[c];
    switch (r & 0xFC) {
    case 0xA0: ch->fnum = ((ch->latch & 7) << 8) | v; ch->block = (ch->latch >> 3) & 7; ch_freq(c); invalidate(c); break;
    case 0xA4: ch->latch = v & 0x3F; break;
    case 0xA8: if (part) break;
        Y.ch3_fnum[c] = ((Y.ch3_latch & 7) << 8) | v; Y.ch3_block[c] = (Y.ch3_latch >> 3) & 7;
        ch_freq(2); invalidate(2); break;
    case 0xAC: if (!part) Y.ch3_latch = v & 0x3F; break;
    case 0xB0: ch->fb = (v >> 3) & 7; ch->alg = v & 7; break;
    case 0xB4: ch->pan_l = v >> 7; ch->pan_r = (v >> 6) & 1; ch->ams = (v >> 4) & 3; ch->fms = v & 7; break;
    }
}

/* ------------------------------------------------------------------ PSG */
static void psg_apply(int v)
{
    if (v & 0x80) {
        P.latch_ch = (v >> 5) & 3; P.latch_vol = (v >> 4) & 1;
        if (P.latch_vol) P.vol[P.latch_ch] = v & 15;
        else if (P.latch_ch == 3) { P.noise_ctl = v & 7; P.lfsr = 0x8000; }
        else P.period[P.latch_ch] = (P.period[P.latch_ch] & 0x3F0) | (v & 15);
    } else {
        if (P.latch_vol) P.vol[P.latch_ch] = v & 15;
        else if (P.latch_ch == 3) { P.noise_ctl = v & 7; P.lfsr = 0x8000; }
        else P.period[P.latch_ch] = (P.period[P.latch_ch] & 15) | ((v & 0x3F) << 4);
    }
}

static int psg_tick(void)
{
    for (int i = 0; i < 3; i++) {
        if (--P.tone_cnt[i] <= 0) {
            P.tone_cnt[i] = P.period[i] ? P.period[i] : 0x400;
            P.tone_out[i] ^= 1;
        }
    }
    if (--P.noise_cnt <= 0) {
        int n = P.noise_ctl & 3;
        P.noise_cnt = (n == 3) ? (P.period[2] ? P.period[2] : 0x400) : (0x10 << n);
        P.noise_ff ^= 1;
        if (P.noise_ff) {                              /* shift on the rising edge */
            int fb = (P.noise_ctl & 4) ? ((P.lfsr ^ (P.lfsr >> 3)) & 1) : (P.lfsr & 1);
            P.lfsr = (P.lfsr >> 1) | (fb << 15);
        }
    }
    int s = 0;
    for (int i = 0; i < 3; i++) {
        int on = (P.period[i] <= 1) ? 1 : P.tone_out[i];   /* period 0/1: constant level */
        s += on ? s_psg_vol[P.vol[i]] : -s_psg_vol[P.vol[i]];
    }
    s += (P.lfsr & 1) ? s_psg_vol[P.vol[3]] : -s_psg_vol[P.vol[3]];
    return s;
}

/* Run the PSG over this frame, binning ticks into FM sample windows. */
static void psg_frame(int64_t cycles)
{
    int e = 0;
    memset(s_psg_acc, 0, sizeof(int32_t) * (Y.n ? Y.n : 1));
    memset(s_psg_cnt, 0, sizeof(uint16_t) * (Y.n ? Y.n : 1));
    while (P.t < cycles) {
        while (e < P.nev && P.ev_t[e] <= P.t) psg_apply(P.ev_v[e++]);
        int s = psg_tick();
        int64_t k = (P.t - Y.frame_start) / FM_RATIO;
        if (k < 0) k = 0;
        if (Y.n) { if (k >= Y.n) k = Y.n - 1; s_psg_acc[k] += s; s_psg_cnt[k]++; }
        P.t += PSG_RATIO;
    }
    while (e < P.nev) psg_apply(P.ev_v[e++]);
    P.nev = 0;
    P.t -= cycles;
}

/* ------------------------------------------------------------------ public API */
static int s_rate = 44100;
static double s_rs_step;
#define TAPS   16                    /* half-length of the sinc kernel (FM samples) */
#define PHASES 256                   /* fractional positions */
#define HMASK  8191
static float s_kern[PHASES][2 * TAPS];
static float s_hist[HMASK + 1][2];
static long long s_hn;               /* FM samples pushed into the history */
static double s_rs_abs = TAPS;
static float s_dc_x[2], s_dc_y[2];       /* next output time, absolute FM samples */

static void build_kernel(double step)
{
    double fc = 0.45 / step;         /* cutoff relative to the FM rate: ~19.8 kHz at 44.1 kHz */
    for (int p = 0; p < PHASES; p++) {
        double frac = (double)p / PHASES, sum = 0;
        for (int j = -TAPS + 1; j <= TAPS; j++) {
            double x = j - frac;
            double sinc = x == 0 ? 2 * fc : sin(2 * M_PI * fc * x) / (M_PI * x);
            double win = 0.42 + 0.5 * cos(M_PI * x / TAPS) + 0.08 * cos(2 * M_PI * x / TAPS);   /* Blackman */
            if (fabs(x) >= TAPS) win = 0;
            s_kern[p][j + TAPS - 1] = (float)(sinc * win);
            sum += sinc * win;
        }
        for (int j = 0; j < 2 * TAPS; j++) s_kern[p][j] /= (float)sum;
    }
}
static int s_fm_gain = 29;         /* 9-bit channel sum -> 16-bit range */
static int s_psg_gain = 1;

void ps_synth_init(int out_rate)
{
    init_tables();
    s_rate = out_rate;
    s_rs_step = (53693175.0 / FM_RATIO) / out_rate;
    build_kernel(s_rs_step);
    ps_synth_reset(0);
    memset(&P, 0, sizeof P);
    P.lfsr = 0x8000;
    for (int i = 0; i < 4; i++) P.vol[i] = 15;
    const char *g = getenv("PS_FM_GAIN"); if (g) s_fm_gain = atoi(g);
    const char *mu = getenv("PS_MUTE");          /* fm | psg: compare one chip at a time */
    if (mu && !strcmp(mu, "fm")) s_fm_gain = 0;
    if (mu && !strcmp(mu, "psg")) s_psg_gain = 0;
}

void ps_synth_reset(unsigned int cycles)
{
    if (ps_synth_dbg) fprintf(stderr, "  ym RESET at %u\n", cycles);
    advance(cycles);
    int64_t pos = Y.pos, fs = Y.frame_start; int n = Y.n;
    int32_t keep[MAX_FM][2]; memcpy(keep, Y.buf, sizeof(int32_t) * 2 * n);
    memset(&Y, 0, sizeof Y);
    Y.pos = pos; Y.frame_start = fs; Y.n = n; memcpy(Y.buf, keep, sizeof(int32_t) * 2 * n);
    for (int c = 0; c < 6; c++) {
        Y.ch[c].pan_l = Y.ch[c].pan_r = 1;
        for (int i = 0; i < 4; i++) { Y.ch[c].op[i].env = 0x3FF; Y.ch[c].op[i].state = EG_REL; Y.ch[c].op[i].inc = 0xFFFFFFFF; }
    }
    Y.tac = 0; Y.tbc = 0; Y.mode = 0x30;   /* reset: timers stopped, flags cleared */
}

void ps_synth_fm_write(unsigned int cycles, unsigned int a, unsigned int v)
{
    if (a & 1) {
        advance(cycles);
        Y.busy_end = (((int64_t)cycles + BUSY_RATIO - 1) / BUSY_RATIO + 32) * BUSY_RATIO;
        write_reg(Y.addr >> 8, Y.addr & 0xFF, v & 0xFF);   /* either data port uses the latch */
    } else {
        Y.addr = (v & 0xFF) | ((a & 2) << 7);
    }
}

unsigned int ps_synth_fm_read(unsigned int cycles, unsigned int a)
{
    if (a != 0) return 0;               /* discrete YM2612: status only at A1/A0 = 0 */
    advance(cycles);
    return (unsigned)Y.status | ((int64_t)cycles < Y.busy_end ? 0x80 : 0);
}

void ps_synth_psg_write(unsigned int cycles, unsigned int v)
{
    if (P.nev < 1024) { P.ev_t[P.nev] = cycles; P.ev_v[P.nev] = (uint8_t)v; P.nev++; }
    else psg_apply(v);
}

int ps_synth_end_frame(unsigned int cycles, int16_t *out, int max_frames)
{
    advance(cycles);
    /* Samples rendered past the frame end (a read that overshot it) are dropped and the
     * next frame starts at the first sample boundary after `cycles`: the chip state keeps
     * the extra ticks. This mirrors the oracle so the driver sees identical timer timing. */
    {
        int64_t k = (cycles - Y.frame_start + FM_RATIO - 1) / FM_RATIO;
        if (k < 0) k = 0;
        if (k < Y.n) Y.n = (int)k;
        Y.pos = Y.frame_start + k * FM_RATIO;
    }
    psg_frame(cycles);
    int n = Y.n, o = 0;
    for (int k = 0; k < n; k++) {
        int p = s_psg_cnt[k] ? s_psg_acc[k] / s_psg_cnt[k] * s_psg_gain : 0;
        Y.buf[k][0] = Y.buf[k][0] * s_fm_gain / 32 + p;
        Y.buf[k][1] = Y.buf[k][1] * s_fm_gain / 32 + p;
    }
    /* Band-limited resample (windowed sinc, cutoff just under the output Nyquist) over a
     * history ring of FM-rate samples, so nothing above 20 kHz folds back as aliasing. */
    for (int k = 0; k < n; k++) {
        s_hist[s_hn & HMASK][0] = (float)Y.buf[k][0];
        s_hist[s_hn & HMASK][1] = (float)Y.buf[k][1];
        s_hn++;
    }
    /* output t (in FM samples, absolute) is s_rs_abs; need samples up to t + TAPS */
    while (o < max_frames && s_rs_abs + TAPS < (double)s_hn) {
        int base = (int)floor(s_rs_abs);
        double frac = s_rs_abs - base;
        int ph = (int)(frac * PHASES + 0.5); if (ph >= PHASES) { ph -= PHASES; base++; }
        const float *w = s_kern[ph];
        float l = 0, r = 0;
        for (int j = -TAPS + 1; j <= TAPS; j++) {
            const float *h = s_hist[(base + j) & HMASK];
            float c = w[j + TAPS - 1];
            l += h[0] * c; r += h[1] * c;
        }
        /* DC blocker (~20 Hz one-pole high-pass): the DAC ladder offset is pure DC */
        float hl = l - s_dc_x[0] + 0.99715f * s_dc_y[0], hr = r - s_dc_x[1] + 0.99715f * s_dc_y[1];
        s_dc_x[0] = l; s_dc_x[1] = r; s_dc_y[0] = hl; s_dc_y[1] = hr;
        l = hl; r = hr;
        out[o * 2] = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
        out[o * 2 + 1] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
        o++;
        s_rs_abs += s_rs_step;
    }
    /* rebase to the next frame */
    Y.pos -= cycles;
    Y.frame_start = Y.pos;
    Y.busy_end = Y.busy_end > cycles ? Y.busy_end - cycles : 0;
    Y.n = 0;
    return o;
}

void ps_synth_dbg_timers(int *tac, int *tbc, int *mode, int *status) { *tac = Y.tac; *tbc = Y.tbc; *mode = Y.mode; *status = Y.status; }

/* ps_state.c: YM2612 and PSG (timers, envelopes, pending PSG writes), output resampler */
void ps_state_blob(void *p, unsigned long n);
void ps_synth_state(void) {
#define B(v) ps_state_blob(&(v), sizeof (v))
    B(Y); B(P); B(s_hist); B(s_hn); B(s_rs_abs); B(s_dc_x); B(s_dc_y);
#undef B
}
