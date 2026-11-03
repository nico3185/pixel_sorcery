/*
 * mcd_pcm.c -- the native Mega CD's PCM chip (Ricoh RF5C164): 8 channels stepping through 64 KB of
 * wave RAM (loop points, envelope, pan), clocked every 384 sub-CPU clocks (~32.55 kHz) and mixed into
 * the output at 44.1 kHz (ps_audio.c). Ported from ares (ares/md/mcd/pcm.cpp at commit dc4f4659d496;
 * the ISC notice is in mcd.h).
 */
#include "mcd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
    uint8_t ram[0x10000];
    struct { bool enable; uint8_t bank, channel; } io;
    struct { bool enable; uint8_t envelope, pan, start; uint16_t step, loop; uint32_t address; } ch[8];
} p;

static int16_t s_out[8192][2];                     /* samples at 32.55 kHz, drained by the mixer */
static unsigned s_w, s_r;
static double s_phase;

uint8_t mcd_pcm_read(uint32_t o) {
    o &= 0x1FFF;
    if (o >= 0x1000) return p.ram[(uint32_t)p.io.bank << 12 | (o & 0xFFF)];
    if (o >= 0x10 && o <= 0x1F) { uint16_t off = (uint16_t)(p.ch[(o >> 1) & 7].address >> 11); return (uint8_t)((o & 1) ? off >> 8 : off); }
    return 0xFF;
}
unsigned long g_mcd_pcm_writes, g_mcd_pcm_reg_writes;
void mcd_pcm_write(uint32_t o, uint8_t v) {
    o &= 0x1FFF;
    g_mcd_pcm_writes++; if (o < 0x1000) g_mcd_pcm_reg_writes++;
    static FILE *log; static int init;                  /* PS_PCM_LOG=file: register writes, as the reference logs them */
    if (!init) { init = 1; const char *f = getenv("PS_PCM_LOG"); if (f && *f) log = fopen(f, "w"); }
    if (log && o < 0x1000) { extern long ps_frames; fprintf(log, "%ld %02X %02X\n", ps_frames, (unsigned)(o & 0xFF), v); }
    if (o >= 0x1000) { p.ram[(uint32_t)p.io.bank << 12 | (o & 0xFFF)] = v; return; }
    __typeof__(p.ch[0]) *c = &p.ch[p.io.channel];
    switch (o) {
    case 0x0: c->envelope = v; break;
    case 0x1: c->pan = v; break;
    case 0x2: c->step = (uint16_t)((c->step & 0xFF00) | v); break;
    case 0x3: c->step = (uint16_t)((c->step & 0x00FF) | v << 8); break;
    case 0x4: c->loop = (uint16_t)((c->loop & 0xFF00) | v); break;
    case 0x5: c->loop = (uint16_t)((c->loop & 0x00FF) | v << 8); break;
    case 0x6: c->start = v; if (!c->enable) c->address = (uint32_t)c->start << 8 << 11; break;
    case 0x7: if (v & 0x40) p.io.channel = v & 7; else p.io.bank = v & 0xF; p.io.enable = v >> 7 & 1; break;
    case 0x8:
        for (int i = 0; i < 8; i++) {
            p.ch[i].enable = !(v >> i & 1);
            if (!p.ch[i].enable) p.ch[i].address = (uint32_t)p.ch[i].start << 8 << 11;
        }
        break;
    }
}

/* every 384 sub-CPU clocks (mcd.c) */
void mcd_pcm_clock(void) {
    int32_t l = 0, r = 0;
    if (p.io.enable)
        for (int i = 0; i < 8; i++) {
            __typeof__(p.ch[0]) *c = &p.ch[i];
            if (!c->enable) continue;
            int32_t d = p.ram[(c->address >> 11) & 0xFFFF];
            if (d == 0xFF) {                                    /* loop marker */
                c->address = (uint32_t)c->loop << 11;
                d = p.ram[c->loop];
                if (d == 0xFF) continue;
            } else c->address = (c->address + c->step) & 0x7FFFFFF;
            d = (d & 0x80) ? (d & 0x7F) : -(d & 0x7F);
            l += d * c->envelope * (c->pan & 0xF) >> 5;
            r += d * c->envelope * (c->pan >> 4) >> 5;
        }
    l = (l > 32767 ? 32767 : l < -32768 ? -32768 : l) >> 2 & ~15;   /* the 10-bit DAC */
    r = (r > 32767 ? 32767 : r < -32768 ? -32768 : r) >> 2 & ~15;
    int16_t *o = s_out[s_w++ & 8191]; o[0] = (int16_t)l; o[1] = (int16_t)r;
    { extern unsigned long g_mcd_pcm_loud; if (l || r) g_mcd_pcm_loud++; }
    if (s_w - s_r > 8192) s_r = s_w - 8192;
}

/* the mixer's call: N stereo frames at 44.1 kHz, the chip's 32.55 kHz samples held between */
void mcd_pcm_mix(int16_t *out, int n) {
    const double step = 12500000.0 / 384.0 / 44100.0;
    for (int i = 0; i < n && s_r != s_w; i++) {
        const int16_t *c = s_out[s_r & 8191];
        for (int k = 0; k < 2; k++) {
            int v = out[2 * i + k] + c[k];
            out[2 * i + k] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        }
        s_phase += step;
        while (s_phase >= 1.0 && s_r != s_w) { s_phase -= 1.0; s_r++; }
    }
}

unsigned long g_mcd_pcm_loud;                      /* non-silent samples (PS_MCD_TRACE) */
unsigned mcd_pcm_channels_on(void) { unsigned m = 0; for (int i = 0; i < 8; i++) m |= (unsigned)p.ch[i].enable << i; return p.io.enable ? m : 0; }
void mcd_pcm_power(void) {
    memset(&p, 0, sizeof p);
    for (int i = 0; i < 8; i++) p.ch[i].pan = 0xFF;
}

/* ps_state.c: the PCM chip, its wave RAM and output ring */
void ps_state_blob(void *p, unsigned long n);
#define B(v) ps_state_blob(&(v), sizeof (v))
void mcd_pcm_state(void) { B(p); B(s_out); B(s_w); B(s_r); B(s_phase); }
#undef B
