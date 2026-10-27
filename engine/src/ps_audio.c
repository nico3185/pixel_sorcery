/*
 * ps_audio.c -- native sound runtime.
 *
 * Milestone A: the game's Z80 sound driver runs as statically recompiled C
 * (tools/z80recomp) instead of the GPGX Z80 core. Its I/O still goes through
 * GPGX's Z80 memory handlers (YM2612, PSG, bank register, 68K window), so the
 * sound chips are unchanged and the FM/PSG register-write streams can be
 * compared exactly between PS_Z80=recomp and PS_Z80=gpgx.
 *
 *   PS_Z80=recomp|gpgx   Z80 backend (default recomp when a driver is registered)
 *   PS_FMLOG=path        log every YM2612/PSG write as "frame addr data"
 */
#include "genrecomp/ps_z80rt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ps_hw.h"
#include "ps_synth.h"
#include "ps_portable.h"

static long s_frame;

/* ---- Sound chips: PS_AUDIO=native (default) | gpgx | compare ----
 * native : our YM2612/PSG (ps_synth.c) answer status reads and produce the audio.
 * compare: both run; ours drives status and output, GPGX's output goes to PS_WAV_REF.
 * gpgx   : the GPGX chips only (oracle). PS_WAV=file dumps whatever is played. */
static int s_amode = -1;
#ifndef PS_NATIVE
static void (*g_fm_write)(unsigned int, unsigned int, unsigned int);
static unsigned int (*g_fm_read)(unsigned int, unsigned int);
static void (*g_fm_reset)(unsigned int);
static void syn_fm_write(unsigned int c, unsigned int a, unsigned int v)
{
    ps_synth_fm_write(c, a, v);
    if (s_amode == 2) {
        g_fm_write(c, a, v);
    }
}
static unsigned int syn_fm_read(unsigned int c, unsigned int a)
{
    unsigned int r = ps_synth_fm_read(c, a);
    if (s_amode == 2) {
        unsigned int g = g_fm_read(c, a);
        static int n;
        if (g != r && n++ < 10) fprintf(stderr, "ps_audio: YM status differs at frame %ld cycle %u: native %02X gpgx %02X\n", s_frame, c, r, g);
    }
    return r;
}
static void syn_fm_reset(unsigned int c) { ps_synth_reset(c); if (s_amode == 2) g_fm_reset(c); }
static int syn_psg_write(unsigned int c, unsigned int v) { ps_synth_psg_write(c, v); return s_amode == 1; }

#endif

typedef struct { FILE *f; uint32_t frames; } Wav;
static Wav s_wav, s_wref;
static void wav_open(Wav *w, const char *path)
{
    if (!path || !*path || !(w->f = fopen(path, "wb"))) return;
    static const uint8_t h[44] = {'R','I','F','F',0,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,1,0,2,0,
        0x44,0xAC,0,0, 0x10,0xB1,2,0, 4,0,16,0,'d','a','t','a',0,0,0,0};
    fwrite(h, 1, 44, w->f);
}
static void wav_put(Wav *w, const int16_t *s, int n) { if (w->f && n > 0) { fwrite(s, 4, (size_t)n, w->f); w->frames += (uint32_t)n; } }
static void wav_close(Wav *w)
{
    if (!w->f) return;
    uint32_t d = w->frames * 4, r = d + 36;
    fseek(w->f, 4, SEEK_SET); fwrite(&r, 4, 1, w->f);
    fseek(w->f, 40, SEEK_SET); fwrite(&d, 4, 1, w->f);
    fclose(w->f); w->f = NULL;
}
static void wav_close_all(void) { wav_close(&s_wav); wav_close(&s_wref); }

static void audio_chips_init(void)
{
#ifdef PS_NATIVE
    s_amode = 1;                       /* our chips only: nothing else is linked */
    wav_open(&s_wav, getenv("PS_WAV"));
    atexit(wav_close_all);
    ps_synth_init(44100);
    fprintf(stderr, "ps_audio: sound chips native\n");
#else
    const char *m = getenv("PS_AUDIO");
    s_amode = (m && !strcmp(m, "gpgx")) ? 0 : (m && !strcmp(m, "compare")) ? 2 : 1;
    wav_open(&s_wav, getenv("PS_WAV"));
    if (s_amode == 2) wav_open(&s_wref, getenv("PS_WAV_REF"));
    const char *mu = getenv("PS_MUTE");
    if (mu && !strcmp(mu, "fm")) config.fm_preamp = 0;
    if (mu && !strcmp(mu, "psg")) { config.psg_preamp = 0; psg_config(0, 0, 0xFF); }
    atexit(wav_close_all);
    if (s_amode) {
        ps_synth_init(44100);
        g_fm_write = fm_write; g_fm_read = fm_read; g_fm_reset = fm_reset;
        fm_write = syn_fm_write; fm_read = syn_fm_read; fm_reset = syn_fm_reset;
        psg_write_hook = syn_psg_write;
    }
    fprintf(stderr, "ps_audio: sound chips %s\n", s_amode == 0 ? "gpgx" : s_amode == 1 ? "native" : "native (compare with gpgx)");
#endif
}

/* Frame end: produce the frame's audio (stereo frames) before cycle counters rebase. */
int ps_audio_end_frame(unsigned int cycles, int16_t *out, int max_frames)
{
    int n;
#ifndef PS_NATIVE
    static int16_t ref[8192];
    if (s_amode == 0) { n = audio_update(out); wav_put(&s_wav, out, n); return n; }
    if (s_amode == 2) { int m = audio_update(ref); wav_put(&s_wref, ref, m); }
#endif
    n = ps_synth_end_frame(cycles, out, max_frames);
    wav_put(&s_wav, out, n);
    return n;
}

uint8_t ps_z80_sz53[256], ps_z80_sz53p[256], ps_z80_parity[256];

void ps_z80rt_init_tables(void)
{
    for (int i = 0; i < 256; i++) {
        int p = 0;
        for (int b = 0; b < 8; b++) p ^= (i >> b) & 1;
        ps_z80_parity[i] = p ? 0 : FP;
        ps_z80_sz53[i] = (uint8_t)((i & (FS | F3 | F5)) | (i ? 0 : FZ));
        ps_z80_sz53p[i] = ps_z80_sz53[i] | ps_z80_parity[i];
    }
}

static PsZ80 s_z;
static void (*s_driver)(PsZ80 *) = NULL;
/* Weak default; a title's generated src/recomp/z80_driver.c defines it (strong) to point at
 * its recompiled driver (tools/z80recomp --name game). NULL = GPGX Z80.
 * MSVC has no __attribute__((weak)); /alternatename falls back to the default below only
 * when no strong ps_z80_game_driver is linked in. */
#if defined(_MSC_VER)
extern void (*ps_z80_game_driver)(PsZ80 *);
void (*ps_z80_game_driver_ps_default)(PsZ80 *) = NULL;
PS_ALTNAME(ps_z80_game_driver, ps_z80_game_driver_ps_default)
#else
__attribute__((weak)) void (*ps_z80_game_driver)(PsZ80 *) = NULL;
#endif
static int s_mode = -1;                 /* 1 = recomp, 0 = gpgx */
static FILE *s_fmlog;
#ifdef HOOK_CPU
static void gpgx_z80_hook(hook_type_t type, int width, unsigned int address, unsigned int value);
#endif
static char s_ring[64][96];      /* last instructions, dumped on a dispatch miss */
static unsigned s_ring_i;
void ps_z80_trace_open(void);
void ps_audio_z80_reset(void);
void ps_audio_z80_run(unsigned int target);
/* Log YM2612 ($4000-$5FFF) and PSG ($7F11) writes, then forward to the chips. */
static void fm_log(unsigned int a, unsigned char v)
{
    if (s_fmlog && ((a >= 0x4000 && a < 0x6000) || (a & 0xFFF9) == 0x7F11))
        fprintf(s_fmlog, "%ld %04X %02X %u\n", s_frame, a & 0x7FFF, v, Z80.cycles);
}
#ifndef PS_NATIVE
static void (*s_gpgx_write)(unsigned int, unsigned char);
static void logged_write(unsigned int a, unsigned char v) { fm_log(a, v); s_gpgx_write(a, v); }
#endif

void ps_audio_register_driver(void (*run)(PsZ80 *))
{
    s_driver = run;
}

int ps_audio_z80_recomp(void)
{
    if (s_mode < 0) {
        audio_chips_init();
        if (!s_driver && ps_z80_game_driver) s_driver = ps_z80_game_driver;
        const char *m = getenv("PS_Z80");
        s_mode = (s_driver && !(m && !strcmp(m, "gpgx"))) ? 1 : 0;
        const char *log = getenv("PS_FMLOG");
        if (log && *log) s_fmlog = fopen(log, "w");
#ifndef PS_NATIVE
        s_gpgx_write = z80_memory_w;
        if (s_fmlog) z80_writemem = logged_write;   /* GPGX core path logs too */
#endif
        if (s_mode) {
            ps_z80rt_init_tables();
            ps_z80_trace_open();
            if (getenv("PS_Z80_WATCH")) ps_z80_watch = (int)strtol(getenv("PS_Z80_WATCH"), NULL, 16) & 0x1FFF;
#ifndef PS_NATIVE
            z80_reset_hook = ps_audio_z80_reset;   /* every /RESET release, even sub-line pulses */
            z80_run_hook = ps_audio_z80_run;       /* GPGX's own catch-up calls run our driver too */
#endif
            memset(&s_z, 0, sizeof s_z);
            s_z.ram = zram;
            /* power-on register values as GPGX sets them, so traces compare */
            s_z.sp = 0; s_z.a = 0; s_z.f = 0x40;
        }
        fprintf(stderr, "ps_audio: Z80 backend %s\n", s_mode ? "recompiled" : "gpgx");
#ifdef HOOK_CPU
        if (!s_mode && getenv("PS_Z80_TRACE")) { ps_z80_trace_open(); set_cpu_hook(gpgx_z80_hook); }
#endif
    }
    return s_mode;
}

/* Frame end: GPGX rebases its cycle counters; keep the driver clock aligned. */
void ps_audio_rebase(unsigned int frame_cycles) { s_z.cyc -= frame_cycles; }

extern int ps_synth_dbg;
void ps_audio_frame(long frame) { s_frame = frame; ps_synth_dbg = getenv("PS_SYNTH_DBG") && frame >= atol(getenv("PS_SYNTH_DBG")) && frame < atol(getenv("PS_SYNTH_DBG")) + 300; if (s_fmlog && frame % 60 == 0) fflush(s_fmlog); }

/* The recompiled driver may only run once its code is actually in Z80 RAM: before the
 * 68K uploads it, a real Z80 executes whatever RAM holds (power-on junk), which has no
 * recompiled equivalent. We start at the first /RESET release whose RAM matches the
 * image the driver was recompiled from (ps_z80_game_image, written by recomp.sh). */
static int s_started;
#if defined(_MSC_VER)
extern const unsigned char *ps_z80_game_image;
const unsigned char *ps_z80_game_image_ps_default = NULL;
PS_ALTNAME(ps_z80_game_image, ps_z80_game_image_ps_default)
extern unsigned int ps_z80_game_image_len;
unsigned int ps_z80_game_image_len_ps_default = 0;
PS_ALTNAME(ps_z80_game_image_len, ps_z80_game_image_len_ps_default)
#else
__attribute__((weak)) const unsigned char *ps_z80_game_image = NULL;
__attribute__((weak)) unsigned int ps_z80_game_image_len = 0;
#endif

/* Hardware /RESET released: Z80 restarts at 0 with interrupts off. */
void ps_audio_z80_reset(void)
{
    if (ps_z80_game_image && ps_z80_game_image_len)
        s_started = memcmp(zram, ps_z80_game_image, ps_z80_game_image_len) == 0;
    else
        s_started = 1;
    if (getenv("PS_Z80_DEBUG")) {
        unsigned n = 0; while (n < ps_z80_game_image_len && zram[n] == ps_z80_game_image[n]) n++;
        fprintf(stderr, "ps_audio: /RESET released, RAM matches image for %u/%u bytes\n", n, ps_z80_game_image_len);
        FILE *d = fopen("/tmp/zram_at_reset.bin", "wb"); if (d) { fwrite(zram, 1, 0x2000, d); fclose(d); }
    }
    if (s_started) fprintf(stderr, "ps_audio: driver uploaded, recompiled Z80 started\n");
#ifndef PS_NATIVE
    /* GPGX's z80_reset() just ran: take its post-reset register state */
    s_z.a = Z80.af.b.h; s_z.f = Z80.af.b.l; s_z.b = Z80.bc.b.h; s_z.c = Z80.bc.b.l;
    s_z.d = Z80.de.b.h; s_z.e = Z80.de.b.l; s_z.h = Z80.hl.b.h; s_z.l = Z80.hl.b.l;
    s_z.ixh = Z80.ix.b.h; s_z.ixl = Z80.ix.b.l; s_z.iyh = Z80.iy.b.h; s_z.iyl = Z80.iy.b.l;
    s_z.sp = Z80.sp.w.l;
#endif
    s_z.pc = 0; s_z.iff1 = s_z.iff2 = 0; s_z.im = 0; s_z.halted = 0;
    s_z.ei_block = 0; s_z.i = s_z.r = 0;
    s_z.cyc = Z80.cycles;
}

/* Run the recompiled driver up to the given master-clock time. */
void ps_audio_z80_run(unsigned int target)
{
    if (!s_started || !s_driver) { Z80.cycles = target; s_z.cyc = target; return; }   /* idle until upload (or no driver) */
    static int s_last = -1;                    /* debug: Z80 RAM changed while the driver was not running? */
    if (ps_z80_watch >= 0) {
        int now = zram[ps_z80_watch] | zram[(ps_z80_watch + 1) & 0x1FFF] << 8;
        if (s_last >= 0 && now != s_last)
            fprintf(stderr, "ps_audio: $%04X changed OUTSIDE the driver: %04X -> %04X (zstate=%d, 68K pc ~ frame %ld)\n",
                    ps_z80_watch, s_last, now, zstate, s_frame);
    }
    s_z.irq_line = (Z80.irq_state == ASSERT_LINE);
    s_z.cyc = Z80.cycles;   /* GPGX resyncs the Z80 clock on bus request/release and reset: authoritative */
    s_z.cyc_end = target;
    while (s_z.cyc < s_z.cyc_end) {
        int64_t before = s_z.cyc;
        s_driver(&s_z);
        if (s_z.cyc == before) break;   /* halted with no interrupt: idle to target */
    }
    if (s_z.cyc < (int64_t)target) s_z.cyc = target;
    Z80.cycles = (unsigned int)s_z.cyc;
    if (ps_z80_watch >= 0) s_last = zram[ps_z80_watch] | zram[(ps_z80_watch + 1) & 0x1FFF] << 8;
}

/* I/O above $4000: GPGX's handlers, with the Z80 clock synced for chip timing
 * and bus-contention cycles fed back. */
uint8_t ps_z80_rd_io(PsZ80 *z, uint16_t a)
{
    Z80.cycles = (unsigned int)z->cyc;
#ifdef PS_NATIVE
    uint8_t v = ps_hw_z80_read(a);
#else
    uint8_t v = z80_memory_r(a);
#endif
    z->cyc = Z80.cycles;
    return v;
}

void ps_z80_wr_io(PsZ80 *z, uint16_t a, uint8_t v)
{
    Z80.cycles = (unsigned int)z->cyc;
#ifdef PS_NATIVE
    fm_log(a, v);
    ps_hw_z80_write(a, v);
#else
    if (s_fmlog) logged_write(a, v); else z80_memory_w(a, v);
#endif
    z->cyc = Z80.cycles;
}

void ps_z80_miss(PsZ80 *z)
{
    if (z->misses++ < 20) {
        fprintf(stderr, "ps_audio: Z80 dispatch miss at $%04X\n", z->pc);
        if (ps_z80_trace_on && z->misses == 1)
            for (unsigned k = 0; k < 64; k++) fprintf(stderr, "  %s\n", s_ring[(s_ring_i + k) & 63]);
    }
}

/* Instruction trace for comparison against the GPGX Z80 (tools/trace_runner PS_Z80_TRACE). */
int ps_z80_trace_on;
static FILE *s_ztrace;
static long s_ztrace_n;
static long s_trace_from;
void ps_z80_trace(PsZ80 *z, uint16_t pc)
{
    snprintf(s_ring[s_ring_i++ & 63], sizeof s_ring[0], "%04X A%02X F%02X BC%02X%02X DE%02X%02X HL%02X%02X SP%04X [SP]%02X%02X",
             pc, z->a, z->f, z->b, z->c, z->d, z->e, z->h, z->l, z->sp,
             z->ram[(z->sp + 1) & 0x1FFF], z->ram[z->sp & 0x1FFF]);
    if (!s_ztrace || s_frame < s_trace_from || s_ztrace_n++ > 200000) return;
    fprintf(s_ztrace, "%ld %04X A%02X F%02X B%02X C%02X D%02X E%02X H%02X L%02X SP%04X CY%lld\n", s_frame,
            pc, z->a, z->f & 0xD7, z->b, z->c, z->d, z->e, z->h, z->l, z->sp, (long long)z->cyc);
}
void ps_z80_trace_open(void)
{
    const char *p = getenv("PS_Z80_TRACE");
    if (p && *p) { s_ztrace = fopen(p, "w"); ps_z80_trace_on = 1; }
    if (getenv("PS_Z80_TRACE_FROM")) s_trace_from = atol(getenv("PS_Z80_TRACE_FROM"));
}

int ps_z80_watch = -1;
void ps_z80_watch_hit(PsZ80 *z, uint16_t a, uint8_t v)
{
    static int n;
    if ((v == 0xFF || v == 0xA5) && n++ < 40)
        fprintf(stderr, "ps_audio: write $%04X <- %02X at PC~$%04X SP=%04X IX=%02X%02X IY=%02X%02X HL=%02X%02X\n",
                a, v, z->pc, z->sp, z->ixh, z->ixl, z->iyh, z->iyl, z->h, z->l);
}

#ifdef HOOK_CPU
/* Same-harness reference: with PS_Z80=gpgx and PS_Z80_TRACE, log the GPGX Z80 in the
 * recompiled driver's trace format (only in builds with GPGX's CPU hook). */
static void gpgx_z80_hook(hook_type_t type, int width, unsigned int address, unsigned int value)
{
    (void)width; (void)value;
    if (type != HOOK_Z80_E || !s_ztrace || s_frame < s_trace_from || s_ztrace_n++ > 200000) return;
    fprintf(s_ztrace, "%ld %04X A%02X F%02X B%02X C%02X D%02X E%02X H%02X L%02X SP%04X CY%u\n", s_frame,
            address & 0xFFFF, Z80.af.b.h, Z80.af.b.l & 0xD7, Z80.bc.b.h, Z80.bc.b.l,
            Z80.de.b.h, Z80.de.b.l, Z80.hl.b.h, Z80.hl.b.l, Z80.sp.w.l, Z80.cycles);
}
#endif

/* ps_state.c: the recompiled Z80's registers and clock (its code runs from z.pc on) */
void ps_state_blob(void *p, unsigned long n);
void ps_audio_state(void) {
    ps_audio_z80_recomp();                     /* chips and driver set up before loading */
    uint8_t *ram = s_z.ram; void *user = s_z.user;
#define B(v) ps_state_blob(&(v), sizeof (v))
    B(s_z); B(s_started); B(s_frame);
#undef B
    s_z.ram = ram; s_z.user = user;
}
