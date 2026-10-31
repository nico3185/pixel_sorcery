/*
 * Memory bus — routes recompiled code memory accesses to Genesis Plus GX
 * hardware (VDP, sound, Z80, I/O, cartridge, RAM).
 *
 * Your recompiled code calls bus_read8/bus_write8 with a 24-bit M68K
 * address. We route it through GenPlusGX's memory map system, which
 * dispatches to the correct hardware component (VDP, I/O, Z80, etc.).
 *
 * For ROM/RAM regions with direct base pointers, we read/write directly.
 * For I/O-mapped regions (VDP, sound, etc.), we call the handler functions.
 */

#include "genrecomp/bus.h"
#include "genrecomp/m68k.h"
#include "genrecomp/platform.h"

/* Machine state: native hardware layer, or Genesis Plus GX in oracle builds */
#include "ps_hw.h"
#ifdef PS_NATIVE
/* Mirrors the oracle's CPU clock/PC fields used below; the native layer works on g_cyc. */
static struct { unsigned int cycles; uint32_t pc; } m68k;
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "genrecomp/func_table.h"
/* debug switches read once (getenv on every VDP write cost ~2% of a frame) */
static int s_irq_dbg = -1, s_dma_dbg = -1;
static int dbg_env(int *v, const char *name) { if (*v < 0) *v = getenv(name) != NULL; return *v; }
#include "genrecomp/ps_vdp.h"

/* ================================================================
 * Video backend selection (PS_RENDERER=native|gpgx|compare)
 *
 * native : the VDP ports ($C00000-$C0000F) go to PixelSorcery's own VDP
 *          state (ps_vdp) and the frame is drawn by ps_render.
 * gpgx   : the ports go to Genesis Plus GX's VDP, which also draws.
 * compare: oracle mode. Port writes go to both; reads come from GPGX so the
 *          game runs exactly as with gpgx; both frames are drawn and diffed.
 * The PSG, which lives in the same address block, always goes to GPGX.
 * ================================================================ */

/* ps_audio.c: recompiled Z80 sound driver */
int  ps_audio_z80_recomp(void);
void ps_audio_z80_reset(void);
void ps_audio_z80_run(unsigned int target);
void ps_audio_frame(long frame);
int ps_audio_end_frame(unsigned int cycles, int16_t *out, int max_frames);
void ps_audio_rebase(unsigned int frame_cycles);

static int s_video_mode = -1;

int bus_video_mode(void) {
#ifdef PS_NATIVE
    return BUS_VIDEO_NATIVE;
#endif
    if (s_video_mode < 0) {
        const char *m = getenv("PS_RENDERER");
        if (m && !strcmp(m, "gpgx")) s_video_mode = BUS_VIDEO_GPGX;
        else if (m && !strcmp(m, "compare")) s_video_mode = BUS_VIDEO_COMPARE;
        else s_video_mode = BUS_VIDEO_NATIVE;
    }
    return s_video_mode;
}

/* Side-effect-free 68000 read (no cycle tick), for DMA sources and prefetch. */
uint16_t bus_peek16(uint32_t addr) {
    addr &= 0xFFFFFE;
    if (addr >= 0x800000 && addr < 0xE00000) addr |= 0xFF0000;  /* DMA source: work RAM mirror */
#ifdef PS_NATIVE
    if (addr >= 0xE00000) return *(uint16_t *)(work_ram + (addr & 0xFFFF));
    if (addr < 0x400000) { uint32_t n; const uint8_t *rom = ps_hw_rom(&n);
        uint32_t m = 1; while (m < n) m <<= 1; return *(uint16_t *)(rom + (addr & (m - 1))); }
    return 0xFFFF;
#else
    cpu_memory_map *map = &m68k.memory_map[(addr >> 16) & 0xFF];
    if (map->base) return *(uint16 *)(map->base + (addr & 0xFFFF));
    if (map->read16) return (uint16_t)map->read16(addr);
    return 0xFFFF;
#endif
}

static unsigned s_clock_extra;     /* added to the VDP clock during a status read */
static void ps_clock(int *line, int *cycle, int *lpf) {
    *line = v_counter;
    *cycle = (int)(g_cyc + s_clock_extra - mcycles_vdp);
    if (*cycle < 0) *cycle = 0;
    *lpf = lines_per_frame;
}

void bus_video_init(void) {
    ps_vdp_init(ps_clock, bus_peek16);
#ifndef PS_NATIVE
    { int ps_megacd_dma_delayed(unsigned reg23); ps_vdp_set_dma_delayed(ps_megacd_dma_delayed); }
#endif
}

/* VDP data/control/HV ports, excluding the PSG and test registers. */
static inline bool is_vdp_port(uint32_t addr) {
    unsigned int region = (addr >> 16) & 0xFF;
    return region >= 0xC0 && region <= 0xDF && (addr & 0x1F) < 0x10;
}

/* ================================================================
 * Cycle simulation for I/O-mapped hardware
 *
 * GenPlusGX's VDP, I/O, and other hardware handlers use m68k.cycles
 * to determine timing (e.g. VDP V/H counter reads). Since recompiled
 * code doesn't execute the M68K interpreter, we simulate cycle
 * advancement on each bus access. This keeps hardware counters
 * (especially the VDP HV counter) progressing naturally.
 *
 * MCYCLES_PER_LINE is 3420 master clocks per scanline.
 * A typical M68K memory access takes ~4 cycles = ~28 master clocks.
 *
 * We also advance v_counter when enough cycles have accumulated for
 * a full scanline, since GPGX's vdp_hvc_r() only looks 1 line ahead.
 * ================================================================ */
/* Exact 68000 timing: generated code advances g_cyc per instruction (m68k.h CYC/M68K_PRE). */
static unsigned s_dma_busy_end;   /* VRAM fill/copy in progress: status DMA flag until then */
static bool s_dma_busy;
static bool s_dma_moved;         /* GPGX moved all the DMA's bytes at once (dma_length 0) */
static unsigned s_fifo[4];      /* native VDP FIFO: read-out cycle of each entry */
static int s_fifo_idx;
unsigned int g_cyc_skip;
unsigned int g_refresh_hold;
uint32_t g_m68k_pf;
unsigned int g_m68k_icost;
unsigned int g_cyc, g_cyc_event = MCYCLES_PER_LINE, g_refresh = 128 * 7;

static bus_vblank_callback_t s_vblank_cb = NULL;
static bool s_in_tick = false;  /* reentrancy guard */

void bus_set_vblank_callback(bus_vblank_callback_t cb) {
    s_vblank_cb = cb;
}

static uint32_t s_last_addr;  /* previous access, for the warning below */

static void bus_lines(void);
static long s_audio_frame;
static bool s_vint_done;
static int s_vb_latched;
static bool s_vint_pending;   /* raised at VInt time, cleared when the 68000 takes level 6 */
static int s_irq_delay;       /* IE0/IE1 enabled with an interrupt pending: fire after one more instruction */
static int s_irq_delay_level; /* 6 (VInt) or 4 (HInt) */
static int s_hcounter;        /* HInt line counter (reloaded from register 10) */
static bool s_hint_pending;
void bus_vint_pending(void) { s_vint_pending = true; }
/* Cycles since the start of the hardware frame (VBlank line): the 68000's E-clock phase
 * (interrupt acknowledge time) follows this count. */
unsigned g_irq_phase_at;          /* interrupt phase time when taken later than its cause */
bool g_irq_phase_set;
#define s_irq_phase_at g_irq_phase_at
#define s_irq_phase_set g_irq_phase_set
unsigned bus_frame_cycles(void) {
    int vb = s_vb_latched;
    unsigned t = g_cyc;
    if (s_irq_phase_set) { t = s_irq_phase_at; s_irq_phase_set = false; }
    (void)vb;
    return t;                     /* the frame time base starts at VBlank, as on the oracle */
}
void bus_irq_ack(uint8_t level) {
    if (level == 6) {
        s_vint_pending = false; status &= ~0x80;
        /* GPGX's IRQ level is "VInt ? 6 : HInt ? 4": an HInt left pending behind the VInt (e.g.
         * IE1 set while the VInt was pending, which bus_write8/16 do not raise) is asserted once
         * the VInt is acknowledged, and taken when the handler's RTE lowers the mask. */
        if (s_hint_pending && (g_ps_vdp.reg[0] & 0x10)) recomp_m68k_interrupt(4);
    }
    else if (level == 4) s_hint_pending = false;
}
/* HInt: counter reaches zero on an active line (or at the VBlank line) */
static void bus_next_event(void);
static void bus_hint_fire(void) {
    { extern long ps_frames; static long from = -2;
      if (from == -2) from = getenv("PS_HINT_DBG") ? atol(getenv("PS_HINT_DBG")) : -1;
      if (from >= 0 && ps_frames >= from && ps_frames <= from + 6)
          fprintf(stderr, "hint f%ld line %d cyc %u reg0 %02X mask %d pend %d\n", ps_frames, v_counter, g_cyc,
                  g_ps_vdp.reg[0], g_m68k.int_mask, g_m68k_irq_pending); }
    s_hint_pending = true;
    bus_next_event();                       /* the handler's own instructions keep the schedule */
    if (g_ps_vdp.reg[0] & 0x10) recomp_m68k_interrupt(4);
}
static int bus_vblank_line(void);
static unsigned bus_vint_time(void);

/* Per access: hand the current time to the GPGX handlers (they read m68k.cycles and may
 * add bus latency, taken back by bus_sync_back after the call). */
static void bus_tick_cycles(void) {
    m68k.cycles = g_cyc;
    /* Some GPGX handlers (Z80 BUSACK) read m68k.pc for prefetched bus data. */
    m68k.pc = g_m68k_pf ? g_m68k_pf : g_m68k.pc;
    /* DMA completes instantly here: never leave the busy flag set. */
    status &= ~0x0002;
}
#ifndef PS_NATIVE
void ps_megacd_poll_wait(void);
#endif
static inline void bus_sync_back(void) {
#ifndef PS_NATIVE
    if (m68k.stopped) ps_megacd_poll_wait();     /* Mega CD: main CPU polling the sub CPU */
#endif
    if ((int)(m68k.cycles - g_cyc) > 0) g_cyc = m68k.cycles;
}

/* Instruction boundary reached g_cyc_event: run scanlines up to now. */
/* Next scheduler event (pending delayed IRQ, VInt on the VBlank line, else line end). */
static void bus_next_event(void) {
    g_cyc_event = s_irq_delay ? g_cyc
                : (v_counter == bus_vblank_line() && !s_vint_done) ? bus_vint_time()
                : mcycles_vdp + MCYCLES_PER_LINE;
}

/* Interrupt handlers run nested inside this call, and their own instructions keep the
 * scanlines (and the Z80) going, as on hardware. */
void bus_cycle_event(void) {
    if (s_irq_delay == 2) { s_irq_phase_at = g_cyc; g_refresh_hold = 1; }   /* end of the enabling instruction */
    if (s_irq_delay && --s_irq_delay == 0) {
        bool ok = s_irq_delay_level == 6 ? (s_vint_pending && ps_vdp_vint_enabled())
                                         : (s_hint_pending && (g_ps_vdp.reg[0] & 0x10));
        if (ok) {
            /* phase of the enabling instruction, only if taken now: still masked by the
             * CPU, it is taken later with the phase of the SR write that unmasks it */
            s_irq_phase_set = s_irq_delay_level > g_m68k.int_mask;
            recomp_m68k_interrupt((uint8_t)s_irq_delay_level);
        }
    }
    if (g_m68k_irq_pending > g_m68k.int_mask)
        recomp_m68k_interrupt(g_m68k_irq_pending);
    bus_lines();
    bus_next_event();
}

/* VBlank line, latched once per frame at VBlank like the hardware's frame timing
 * (the VDP powers on in Mode 4: 192 lines). */
static int s_vb_latched = 192;
static int bus_vblank_line(void) { return s_vb_latched; }
static void bus_latch_vblank(void) {
#ifdef PS_NATIVE
    s_vb_latched = ps_vdp_height();
#else
    s_vb_latched = (bus_video_mode() == BUS_VIDEO_NATIVE) ? ps_vdp_height()
                 : (bitmap.viewport.h > 0 ? bitmap.viewport.h : 224);
#endif
}

/* Power-on timing as on hardware (and the oracle): the 68000 leaves reset 229 lines +
 * 1004 master clocks after the start of VBlank (line 192 in the power-on Mode 4), the
 * reset exception takes 40 cycles, the Z80 clock is aligned to 15 and the bus refresh
 * timer runs from a 128-cycle boundary. */
void bus_power_on(void) {
    unsigned t = ((unsigned)(lines_per_frame - 192 + 159) * MCYCLES_PER_LINE + 1004) / 7 * 7;
    Z80.cycles = t / 15 * 15;
    s_vb_latched = 192;
    v_counter = (192 + t / MCYCLES_PER_LINE) % lines_per_frame;
    mcycles_vdp = t / MCYCLES_PER_LINE * MCYCLES_PER_LINE;   /* frame time base starts at VBlank */
    g_cyc = t + 40 * 7;                                     /* reset exception */
    g_refresh = g_cyc / (128 * 7) * (128 * 7) + 128 * 7;
    m68k.cycles = g_cyc;
    s_vint_done = true;
    g_cyc_event = mcycles_vdp + MCYCLES_PER_LINE;
    status &= ~0x08;
}
/* VInt is raised 788 (H40) / 770 (H32) master clocks into the first VBlank line. */
static unsigned bus_vint_time(void) {
    return mcycles_vdp + ((g_ps_vdp.reg[12] & 1) ? 788 : 770);
}
static void bus_z80_to(unsigned t) {
    ps_audio_z80_recomp();   /* installs z80_run_hook/z80_reset_hook once when recompiled */
    m68k.cycles = g_cyc;
    if (zstate == 1) z80_run(t);
    else Z80.cycles = t;
    /* Z80 reads of the 68K bus (bank window) stall the 68000 */
    if ((int)(m68k.cycles - g_cyc) > 0) g_cyc = m68k.cycles;
}

/* Scanline/interrupt scheduler, mirroring the hardware frame: VBlank flag set at the
 * start of the first blank line, VInt (68K level 6 + Z80 INT) at vint time on it, the
 * Z80 INT released at the end of that line, VBlank flag cleared on the last line. */
static void bus_lines(void) {
    m68k.cycles = g_cyc;
    for (;;) {
        int vb = bus_vblank_line();
        if (v_counter == vb && !s_vint_done && (int)(g_cyc - bus_vint_time()) >= 0) {
            bus_z80_to(bus_vint_time());
            s_vint_done = true;
            status |= 0x80;
            Z80.irq_state = ASSERT_LINE;
            bus_next_event();
            /* ps_state.c: a machine snapshot is taken here, before the frame's VBlank work */
            { extern int ps_state_armed; void ps_state_vblank_point(void); if (ps_state_armed) ps_state_vblank_point(); }
            if (s_vblank_cb) s_vblank_cb();
            continue;
        }
        if ((int)(g_cyc - mcycles_vdp) < MCYCLES_PER_LINE) break;

        bus_z80_to(mcycles_vdp + MCYCLES_PER_LINE);
        if (v_counter == vb) Z80.irq_state = CLEAR_LINE;
        mcycles_vdp += MCYCLES_PER_LINE;
#ifndef PS_NATIVE
        { void ps_megacd_line(unsigned int); ps_megacd_line(mcycles_vdp); }   /* sub CPU + CD hardware, this line */
#endif
        v_counter = (v_counter + 1) % lines_per_frame;
        if (v_counter == lines_per_frame - 1) { status &= ~0x08; s_hcounter = g_ps_vdp.reg[10]; }
        else if (v_counter < bus_vblank_line()) {        /* active line: HInt counter */
#ifdef PS_NATIVE
            { void genrecomp_render_line(int); genrecomp_render_line(v_counter); }   /* before this line's HInt */
#endif
            if (s_hcounter == 0) { s_hcounter = g_ps_vdp.reg[10]; bus_hint_fire(); }
            else s_hcounter--;
        }

        if (v_counter == vb) {
            /* Frame boundary at the start of VBlank: produce audio, rebase every timestamp,
             * latch the display height for the new frame and relabel the line counter. */
            extern long ps_frames; ps_frames++;
            ps_audio_frame(++s_audio_frame);
            static int16_t s_samples[4096];
            int n = ps_audio_end_frame(mcycles_vdp, s_samples, 2048);
            if (n > 0) platform_queue_audio(s_samples, n);
#ifdef PS_NATIVE
            ps_hw_end_frame(mcycles_vdp);
            g_dma68k_end = 0;
#else
            input_end_frame(mcycles_vdp);
            dma_endCycles = 0;
#endif
#ifndef PS_NATIVE
            { void ps_megacd_end_frame(unsigned int); ps_megacd_end_frame(MCYCLES_PER_LINE); }
#endif
            g_cyc -= mcycles_vdp;
            g_refresh -= mcycles_vdp;
            m68k.cycles = g_cyc;
            Z80.cycles -= mcycles_vdp;
            ps_audio_rebase(mcycles_vdp);
            for (int k = 0; k < 4; k++) s_fifo[k] = s_fifo[k] > mcycles_vdp ? s_fifo[k] - mcycles_vdp : 0;
            /* GPGX clears DMA busy when VBlank starts (status &= 0xFEED) and sets it again only
             * while bytes remain: a fill whose bytes all moved at once (s_dma_moved) reads as
             * finished from then on (QS's 64 KB VRAM fill at \$206C, display off) */
            if (s_dma_busy && s_dma_moved) { s_dma_busy = false; s_dma_busy_end = 0; }
            else {
                if (s_dma_busy && (int)(s_dma_busy_end - mcycles_vdp) <= 0) s_dma_busy = false;
                s_dma_busy_end -= mcycles_vdp;
            }
            mcycles_vdp = 0;
#ifndef PS_NATIVE
            dma_endCycles = 0;
            fifo_cycles[0] = fifo_cycles[1] = fifo_cycles[2] = fifo_cycles[3] = 0;
#endif
            { void genrecomp_render_frame(void); genrecomp_render_frame(); }
            extern uint16_t g_ps_render_flags;
            status |= g_ps_render_flags & 0x60;           /* sprite overflow / collision */
            g_ps_render_flags = 0;
            { static FILE *fl; static int init;   /* PS_FRAMELOG: clocks at each frame start */
              if (!init) { init = 1; const char *q = getenv("PS_FRAMELOG"); if (q && *q) fl = fopen(q, "w"); }
              extern long ps_poll_index(void);       /* poll index: a frame without a poll is a lag frame */
              if (fl) fprintf(fl, "%ld %u %u st=%04X poll=%ld\n", ps_frames, g_cyc, Z80.cycles, status, ps_poll_index()); }
            bus_latch_vblank();
            v_counter = s_vb_latched;
            status |= 0x08;
            s_vint_done = false;
            if (s_hcounter == 0) bus_hint_fire();           /* counter expired on the last active line */
        }
    }
}


/* ================================================================
 * Instant VDP DMA
 *
 * GPGX spreads a DMA over scanlines: vdp_dma_update() does one line's
 * worth and system_frame_gen() does the rest. Recompiled code never runs
 * system_frame_gen, so a large DMA would stay pending forever, and while
 * a 68K-bus DMA is pending GPGX latches (and here, loses) further control
 * port writes, wedging the VDP. So after any VDP write we run the DMA to
 * completion, as if it happened between two instructions. Timing-exact
 * DMA (mid-frame raster effects fed by DMA) is out of scope.
 * ================================================================ */
#ifndef PS_NATIVE
static void bus_finish_dma(void) {
    if (!dma_length) return;
    unsigned int saved_cycles = m68k.cycles;
    uint16 saved_vblank = status & 0x08;
    status |= 0x08;  /* blanking rate: largest chunk per call */
    for (int guard = 0; dma_length && guard < 64; guard++)
        vdp_dma_update(0);
    status = (status & ~0x0A) | saved_vblank;
    m68k.cycles = saved_cycles;  /* 68K-bus DMA stalls the CPU; we don't */
}
#endif

/* ================================================================
 * Native VDP access timing (68000 stalls)
 *
 * Active display: data-port writes go through a 4-entry FIFO drained at fixed
 * access slots per line (VRAM writes take two slots: the VRAM bus is byte-wide);
 * the CPU waits when all four are pending. 68K->VDP DMA halts the CPU for the
 * transfer: per line, 18 (H40) / 16 (H32) bytes in active display, 204 / 166 in
 * blanking (198 / 161 words for CRAM/VSRAM when the display is off).
 * ================================================================ */
static const int s_slots_h40[] = { 352, 820, 948, 1076, 1332, 1460, 1588, 1844, 1972, 2100, 2356, 2484, 2612, 2868,
    2996, 3124, 3364, 3380, 3420 + 352, 3420 + 820, 3420 + 948, 3420 + 1076, 3420 + 1332, 3420 + 1460, 3420 + 1588,
    3420 + 1844, 3420 + 1972, 3420 + 2100, 3420 + 2356, 3420 + 2484 };
static const int s_slots_h32[] = { 230, 510, 810, 970, 1130, 1450, 1610, 1770, 2090, 2250, 2410, 2730, 2890, 3050,
    3350, 3370, 3420 + 230, 3420 + 510, 3420 + 810, 3420 + 970, 3420 + 1130, 3420 + 1450, 3420 + 1610, 3420 + 1770,
    3420 + 2090, 3420 + 2250, 3420 + 2410, 3420 + 2730 };

static bool vdp_blank_line(int line) {
    return (line >= ps_vdp_height() && line < lines_per_frame - 1) || !(g_ps_vdp.reg[1] & 0x40);
}

static void vdp_fifo_wait(void) {
    if (vdp_blank_line(v_counter)) return;
    const int *slots = (g_ps_vdp.reg[12] & 1) ? s_slots_h40 : s_slots_h32;
    int byte_access = (g_ps_vdp.code & 0x0F) < 0x03;
    unsigned c = g_cyc;
    if ((int)(c - s_fifo[(s_fifo_idx + 3) & 3]) < 0) {
        if ((int)(c - s_fifo[s_fifo_idx]) < 0) g_cyc = ((s_fifo[s_fifo_idx] + 6) / 7) * 7;   /* FIFO full */
        c = s_fifo[(s_fifo_idx + 3) & 3];
    }
    int rel = (int)(c - mcycles_vdp), slot = 0;
    while (rel >= slots[slot]) slot++;
    s_fifo[s_fifo_idx] = mcycles_vdp + slots[slot + byte_access];
    s_fifo_idx = (s_fifo_idx + 1) & 3;
}

/* End time of a DMA moving `units` (words for 68K->CRAM/VSRAM, bytes otherwise) from t,
 * at the per-line rates of its kind: 0 = 68K->CRAM/VSRAM, 1 = 68K->VRAM, 2 = fill, 3 = copy. */
static unsigned vdp_dma_end(unsigned t, uint32_t units, int type) {
    bool h40 = g_ps_vdp.reg[12] & 1;
    unsigned line_start = mcycles_vdp;
    int line = v_counter;
    while (units) {
        int vb = bus_vblank_line();
        bool in_vblank = line >= vb && line < lines_per_frame - 1;
        bool blank = in_vblank || !(g_ps_vdp.reg[1] & 0x40);
        unsigned rate = blank ? (h40 ? 204 : 166) : (h40 ? 18 : 16);
        rate >>= (type & 1);                            /* VRAM word / copy read+write: 2 accesses */
        if (type == 0 && blank) rate = h40 ? 198 : 161; /* refresh slots when the display is off */
        int lines = 1;
        if (in_vblank) lines = (lines_per_frame - 1 - line + lines_per_frame) % lines_per_frame;
        if (lines < 1) lines = 1;
        unsigned seg_end = line_start + (unsigned)lines * MCYCLES_PER_LINE;
        uint32_t can = (uint32_t)((uint64_t)(seg_end - t) * rate / MCYCLES_PER_LINE);
        if (units <= can) { t += (unsigned)((uint64_t)units * MCYCLES_PER_LINE / rate); units = 0; }
        else { units -= can; t = seg_end; line = (line + lines) % lines_per_frame; line_start = seg_end; }
    }
    return t;
}

/* True when the whole DMA fits in the VBlank segment it started in (one update call). */
static bool vdp_dma_in_vblank_segment(unsigned t) {
    int vb = bus_vblank_line();
    bool in_vblank = v_counter >= vb && v_counter < lines_per_frame - 1;
    if (!in_vblank) return false;
    unsigned seg_end = mcycles_vdp + (unsigned)(lines_per_frame - 1 - v_counter) * MCYCLES_PER_LINE;
    return t <= seg_end;
}

static void vdp_dma_stall(void) {
    uint32_t n = g_ps_vdp_dma68k_words;
    g_ps_vdp_dma68k_words = 0;
    unsigned t = vdp_dma_end(g_cyc, n, g_ps_vdp_dma68k_vram ? 1 : 0);
    /* Across a line boundary the CPU is re-halted to the DMA end, absorbing its own cost */
    if (t > mcycles_vdp + MCYCLES_PER_LINE && !vdp_dma_in_vblank_segment(t)) g_cyc_skip = 1;
    if ((int)(t - g_cyc) > 0) g_cyc = t;           /* the 68000 is halted during 68K-bus DMA */
#ifdef PS_NATIVE
    g_dma68k_end = t;                              /* a Z80 needing the 68K bus waits until then */
#else
    dma_endCycles = t; dma_type = 0;               /* GPGX's Z80 bus access waits on these; its own */
#endif                                             /* VDP (which sets them) is not the one running */
}

static void vdp_dma_busy(void) {
    unsigned t = g_cyc;
    int type = g_ps_vdp_slow_type;
    if (type == 2) {                                 /* fill starts after the data-port write */
        bool h40 = g_ps_vdp.reg[12] & 1, blank = vdp_blank_line(v_counter);
        unsigned rate = blank ? (h40 ? 204 : 166) : (h40 ? 18 : 16);
        t += 2 * (MCYCLES_PER_LINE / rate);
    }
    s_dma_busy_end = vdp_dma_end(t, g_ps_vdp_slow_len, type);
    /* GPGX quirk: a fill whose start (data-port write + 2 slots) falls past the end of the
     * current line gets a negative byte budget, which its unsigned length test reads as
     * "everything": all bytes move at once, only the busy time remains */
    s_dma_moved = type == 2 && !(vdp_blank_line(v_counter) && v_counter >= ps_vdp_height()) &&
                  (int)(t - (mcycles_vdp + MCYCLES_PER_LINE)) > 0;
    extern long ps_frames;
    if (dbg_env(&s_dma_dbg, "PS_DMA_DBG")) fprintf(stderr, "dma f%ld type %d len %u start %u end %u line %d reg1 %02X\n", ps_frames,
                                      type, g_ps_vdp_slow_len, g_cyc, s_dma_busy_end, v_counter, g_ps_vdp.reg[1]);
    s_dma_busy = true;
    g_ps_vdp_slow_len = 0;
}


/* VDP status as the hardware composes it, evaluated at the end of the reading instruction:
 * VInt/VBlank flags kept by the scheduler, FIFO empty/full from the FIFO model, DMA busy,
 * HBlank by position in the line, display-off forces VBlank; bits 15-10 = prefetch. */
static uint16_t bus_vdp_status(uint16_t native) {
    unsigned c = g_cyc + g_m68k_icost;
    uint16_t t = status & 0xE8;
    status &= ~0x60;                                 /* sprite flags clear on read */
    if (s_dma_busy) { if ((int)(c - s_dma_busy_end) < 0) t |= 0x02; else s_dma_busy = false; }
    if ((int)(c - s_fifo[(s_fifo_idx + 3) & 3]) >= 0) t |= 0x200;
    else if ((int)(c - s_fifo[s_fifo_idx]) < 0) t |= 0x100;
    if (!(g_ps_vdp.reg[1] & 0x40)) t |= 0x08;
    int rel = (int)(c - mcycles_vdp);
    if (v_counter == bus_vblank_line() && rel >= (int)((g_ps_vdp.reg[12] & 1) ? 788 : 770)
        && Z80.irq_state != ASSERT_LINE) t |= 0x80;
    bool h40 = g_ps_vdp.reg[12] & 1;
    if (rel >= (h40 ? 228 : 280) && rel < (h40 ? 872 : 860)) t |= 0x04;
    return t | (native & 0xFC00);
}
/* ================================================================
 * Write watchpoint: GENRECOMP_WATCH=FFFEDE (hex, 24-bit) logs every write
 * that touches that byte, with the shadow call stack. For "who overwrote
 * this" questions, e.g. a return address found zeroed at RTS.
 * ================================================================ */
static void bus_watch(uint32_t addr, int size, uint32_t val) {
    static int s_init;
    static uint32_t s_watch;
    if (!s_init) {
        const char *w = getenv("GENRECOMP_WATCH");
        s_watch = w ? (uint32_t)strtoul(w, NULL, 16) & 0xFFFFFF : 0xFFFFFFFF;
        s_init = 1;
    }
    if (s_watch - addr < (uint32_t)size) {
        fprintf(stderr, "watch: write%d $%06X = $%X, ", size * 8, addr, val);
        func_table_dump_stack(stderr);
    }
}

/* ================================================================
 * Flat 24-bit address space reads
 *
 * GenPlusGX's memory_map[256] divides the 24-bit space into 256
 * x 64KB regions. Each region has either a base pointer (for ROM/RAM)
 * or read8/read16 handler functions (for I/O-mapped hardware).
 * ================================================================ */

uint8_t bus_read8(uint32_t addr) {
    addr &= 0xFFFFFF;
    bus_tick_cycles();
    s_last_addr = addr;
    unsigned int region = (addr >> 16) & 0xFF;
#ifndef PS_NATIVE
    cpu_memory_map *map = &m68k.memory_map[region];
#endif
    (void)region;

    if (is_vdp_port(addr) && bus_video_mode() != BUS_VIDEO_GPGX) {
        if ((addr & 0x1C) == 4) s_clock_extra = g_m68k_icost;
        uint8_t v = ps_vdp_read8(addr & 0x1F, g_m68k_pf ? g_m68k_pf : g_m68k.pc);
        s_clock_extra = 0;
        if ((addr & 0x1C) == 4) {
            uint16_t st = bus_vdp_status((uint16_t)(v << 8));
            v = (addr & 1) ? (uint8_t)st : (uint8_t)((st >> 8) & 0x03) | (v & 0xFC);
        }
        if (bus_video_mode() == BUS_VIDEO_NATIVE) return v;
    }

#ifdef PS_NATIVE
    return ps_hw_read8(addr);
#else
    if (map->read8) {
        { uint8_t _v = (uint8_t)map->read8(addr); bus_sync_back(); return _v; }
    }
    if (map->base) {
        return READ_BYTE(map->base, addr & 0xFFFF);
    }
    return 0xFF;
#endif
}

uint16_t bus_read16(uint32_t addr) {
    addr &= 0xFFFFFF;
    bus_tick_cycles();
    s_last_addr = addr;
    unsigned int region = (addr >> 16) & 0xFF;
#ifndef PS_NATIVE
    cpu_memory_map *map = &m68k.memory_map[region];
#endif
    (void)region;

    if (is_vdp_port(addr) && bus_video_mode() != BUS_VIDEO_GPGX) {
        if ((addr & 0x1C) == 4) s_clock_extra = g_m68k_icost;
        uint16_t v = ps_vdp_read16(addr & 0x1F, g_m68k_pf ? g_m68k_pf : g_m68k.pc);
        s_clock_extra = 0;
        if ((addr & 0x1C) == 4) v = bus_vdp_status(v);
        if (bus_video_mode() == BUS_VIDEO_NATIVE) return v;
    }

#ifdef PS_NATIVE
    return ps_hw_read16(addr);
#else
    if (map->read16) {
        { uint16_t _v = (uint16_t)map->read16(addr); bus_sync_back(); return _v; }
    }
    if (map->base) {
        return *(uint16 *)(map->base + (addr & 0xFFFF));
    }
    return 0xFFFF;
#endif
}

uint32_t bus_read32(uint32_t addr) {
    uint16_t hi = bus_read16(addr);
    uint16_t lo = bus_read16(addr + 2);
    return ((uint32_t)hi << 16) | lo;
}

/* ================================================================
 * Flat 24-bit address space writes
 * ================================================================ */

uint32_t g_ps_mcd_mode_writes;   /* main-CPU writes to the Mega CD memory mode register */
uint32_t g_ps_wram_code_writes;  /* main-CPU writes over a compiled work RAM program (overlays) */
void bus_write8(uint32_t addr, uint8_t val) {
    addr &= 0xFFFFFF;
#ifndef PS_NATIVE
    if ((addr & 0xFFFFFE) == 0xA12002) g_ps_mcd_mode_writes++;   /* Word RAM handed over: overlays */
    else if (addr >= 0xFF0000 && (g_ps_overlay_wram_code[(addr & 0xFFFF) >> 4] >> ((addr >> 1) & 7) & 1))
        g_ps_wram_code_writes++;                                  /* over a compiled work RAM program */
#endif
    bus_watch(addr, 1, val);
    bus_tick_cycles();
    s_last_addr = addr;
    unsigned int region = (addr >> 16) & 0xFF;
#ifndef PS_NATIVE
    cpu_memory_map *map = &m68k.memory_map[region];
#endif
    (void)region;

    if (is_vdp_port(addr) && bus_video_mode() != BUS_VIDEO_GPGX) {
        if ((addr & 0x1C) == 0) vdp_fifo_wait();      /* data port */
        bool _ie0 = ps_vdp_vint_enabled(), _ie1 = g_ps_vdp.reg[0] & 0x10;
        ps_vdp_write8(addr & 0x1F, val);
        { extern void recomp_m68k_irq_drop(uint8_t);      /* enable cleared: request withdrawn */
          if (_ie0 && !ps_vdp_vint_enabled()) recomp_m68k_irq_drop(6);
          if (_ie1 && !(g_ps_vdp.reg[0] & 0x10)) recomp_m68k_irq_drop(4); }
        if (!_ie0 && ps_vdp_vint_enabled() && s_vint_pending) { s_irq_delay = 2; s_irq_delay_level = 6; g_cyc_event = g_cyc; }
        else if (!_ie1 && (g_ps_vdp.reg[0] & 0x10) && s_hint_pending && !(s_vint_pending && ps_vdp_vint_enabled()))
            { s_irq_delay = 2; s_irq_delay_level = 4; g_cyc_event = g_cyc; }
        if (dbg_env(&s_irq_dbg, "PS_IRQ_DBG") && _ie0 != ps_vdp_vint_enabled()) fprintf(stderr, "IE0 %d->%d pending=%d cyc=%u line=%d\n", _ie0, ps_vdp_vint_enabled(), s_vint_pending, g_cyc, v_counter);
        if (g_ps_vdp_dma68k_words) vdp_dma_stall();
        if (g_ps_vdp_slow_len) vdp_dma_busy();
        if (bus_video_mode() == BUS_VIDEO_NATIVE) return;
    }

#ifdef PS_NATIVE
    ps_hw_write8(addr, val); return;
#else
    if (map->write8) {
        map->write8(addr, val);
        if (region >= 0xC0 && region <= 0xDF) bus_finish_dma();
        bus_sync_back();
        return;
    }
    if (map->base) {
        WRITE_BYTE(map->base, addr & 0xFFFF, val);
    }
#endif
}

void bus_write16(uint32_t addr, uint16_t val) {
    addr &= 0xFFFFFF;
#ifndef PS_NATIVE
    if ((addr & 0xFFFFFE) == 0xA12002) g_ps_mcd_mode_writes++;   /* Word RAM handed over: overlays */
    else if (addr >= 0xFF0000 && (g_ps_overlay_wram_code[(addr & 0xFFFF) >> 4] >> ((addr >> 1) & 7) & 1))
        g_ps_wram_code_writes++;                                  /* over a compiled work RAM program */
#endif
    bus_watch(addr, 2, val);
    bus_tick_cycles();
    s_last_addr = addr;
    unsigned int region = (addr >> 16) & 0xFF;
#ifndef PS_NATIVE
    cpu_memory_map *map = &m68k.memory_map[region];
#endif
    (void)region;

    if (is_vdp_port(addr) && bus_video_mode() != BUS_VIDEO_GPGX) {
        if ((addr & 0x1C) == 0) vdp_fifo_wait();      /* data port */
        bool _ie0 = ps_vdp_vint_enabled(), _ie1 = g_ps_vdp.reg[0] & 0x10;
        ps_vdp_write16(addr & 0x1F, val);
        { extern void recomp_m68k_irq_drop(uint8_t);      /* enable cleared: request withdrawn */
          if (_ie0 && !ps_vdp_vint_enabled()) recomp_m68k_irq_drop(6);
          if (_ie1 && !(g_ps_vdp.reg[0] & 0x10)) recomp_m68k_irq_drop(4); }
        if (!_ie0 && ps_vdp_vint_enabled() && s_vint_pending) { s_irq_delay = 2; s_irq_delay_level = 6; g_cyc_event = g_cyc; }
        else if (!_ie1 && (g_ps_vdp.reg[0] & 0x10) && s_hint_pending && !(s_vint_pending && ps_vdp_vint_enabled()))
            { s_irq_delay = 2; s_irq_delay_level = 4; g_cyc_event = g_cyc; }
        if (dbg_env(&s_irq_dbg, "PS_IRQ_DBG") && _ie0 != ps_vdp_vint_enabled()) fprintf(stderr, "IE0 %d->%d pending=%d cyc=%u line=%d\n", _ie0, ps_vdp_vint_enabled(), s_vint_pending, g_cyc, v_counter);
        if (g_ps_vdp_dma68k_words) vdp_dma_stall();
        if (g_ps_vdp_slow_len) vdp_dma_busy();
        if (bus_video_mode() == BUS_VIDEO_NATIVE) return;
    }

#ifdef PS_NATIVE
    ps_hw_write16(addr, val); return;
#else
    if (map->write16) {
        map->write16(addr, val);
        if (region >= 0xC0 && region <= 0xDF) bus_finish_dma();
        bus_sync_back();
        return;
    }
    if (map->base) {
        *(uint16 *)(map->base + (addr & 0xFFFF)) = val;
    }
#endif
}

void bus_write32(uint32_t addr, uint32_t val) {
    bus_write16(addr, (uint16_t)(val >> 16));
    bus_write16(addr + 2, (uint16_t)(val & 0xFFFF));
}

/* ================================================================
 * Direct RAM access (bypass memory map for speed)
 *
 * GenPlusGX stores M68K work RAM in the global `work_ram[0x10000]`.
 * These functions access it directly for stack/local operations.
 * ================================================================ */

uint8_t bus_ram_read8(uint16_t offset) {
    return READ_BYTE(work_ram, offset);
}

void bus_ram_write8(uint16_t offset, uint8_t val) {
    WRITE_BYTE(work_ram, offset, val);
}

uint16_t bus_ram_read16(uint16_t offset) {
    return *(uint16 *)(work_ram + offset);
}

void bus_ram_write16(uint16_t offset, uint16_t val) {
    *(uint16 *)(work_ram + offset) = val;
}

uint32_t bus_ram_read32(uint16_t offset) {
    return ((uint32_t)bus_ram_read16(offset) << 16) | bus_ram_read16(offset + 2);
}

void bus_ram_write32(uint16_t offset, uint32_t val) {
    bus_ram_write16(offset, (uint16_t)(val >> 16));
    bus_ram_write16(offset + 2, (uint16_t)(val & 0xFFFF));
}

void bus_dump_state(FILE *f) {
    /* Layout: work RAM 64K | VDP regs 32 | CRAM 128 | VSRAM 128 | VRAM 64K.
     * RAM is in host 16-bit word order. */
    fwrite(work_ram, 1, 0x10000, f);
#ifdef PS_NATIVE
    fwrite(g_ps_vdp.reg, 1, 0x20, f);
    fwrite(g_ps_vdp.cram, 1, 0x80, f);
    fwrite(g_ps_vdp.vsram, 1, 0x50, f); { static const uint8_t z[0x30]; fwrite(z, 1, 0x30, f); }
    fwrite(g_ps_vdp.vram, 1, 0x10000, f);
#else
    fwrite(reg, 1, 0x20, f);
    fwrite(cram, 1, 0x80, f);
    fwrite(vsram, 1, 0x80, f);
    fwrite(vram, 1, 0x10000, f);
#endif
}

uint8_t *bus_get_ram(void) {
    return work_ram;
}

const uint8_t *bus_get_rom(uint32_t *size_out) {
#ifdef PS_NATIVE
    return ps_hw_rom(size_out);
#else
    /* ROM is mapped starting at region 0x00 of the memory map */
    if (size_out) {
        *size_out = rominfo.romend + 1;
    }
    if (m68k.memory_map[0].base) {
        return m68k.memory_map[0].base;
    }
    return NULL;
#endif
}

/* ps_state.c: the bus clock, scheduler and VDP access timing */
void ps_state_blob(void *p, unsigned long n);
void bus_state(void) {
#define B(v) ps_state_blob(&(v), sizeof (v))
#ifdef PS_NATIVE
    B(m68k);
#endif
    B(s_clock_extra); B(s_dma_busy_end); B(s_dma_busy); B(s_dma_moved); B(s_fifo); B(s_fifo_idx);
    B(g_cyc_skip); B(g_refresh_hold); B(g_m68k_pf); B(g_m68k_icost); B(g_cyc); B(g_cyc_event); B(g_refresh);
    B(s_last_addr); B(s_audio_frame); B(s_vint_done); B(s_vb_latched); B(s_vint_pending); B(s_irq_delay);
    B(s_irq_delay_level); B(s_hcounter); B(s_hint_pending); B(g_irq_phase_at); B(g_irq_phase_set);
#undef B
}
/* A restored snapshot (taken at ps_state_vblank_point) carries on as the bus clock would
 * have: the frame's VBlank work (input, VInt, present), the remaining scanlines, the next event. */
void bus_state_continue(void) {
    if (s_vblank_cb) s_vblank_cb();
    bus_lines();
    bus_next_event();
}
