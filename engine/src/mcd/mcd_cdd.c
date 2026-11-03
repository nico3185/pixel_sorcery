/*
 * mcd_cdd.c -- the native Mega CD's CD drive, behaving as the reference player's (Genesis Plus GX,
 * which the 74 stage starts are checked against): the status and command registers as nibbles
 * ($FF8038-$FF804B), a command run when the BIOS writes $FF804A, the reported status updated by
 * "get status" only once the drive is ready, Play/Seek taking effect one 75 Hz tick later with the
 * reference's latency (2 ticks, cd_latency 0: no seek time), and the 75 Hz tick that feeds the CD
 * controller and raises level 4 while the host clock runs. The port's own code, written to match
 * that behaviour (the reference's code is not copied); the register map is the hardware's.
 * The disc is the player's cue/bin image (mcd_disc.c); CD-DA is not played yet (silent).
 */
#include "mcd.h"
#include "mcd_disc.h"
bool mcd_disc_read(int lba, uint8_t raw[2352]);
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void mcd_cdc_decode(int lba);                      /* a data sector under the head (mcd_cdc.c) */
void mcd_cdc_decode_idle(void);                    /* the decoder runs on while nothing is read */

enum { ST_STOP = 0x0, ST_PLAY = 0x1, ST_SEEK = 0x2, ST_SCAN = 0x3, ST_PAUSE = 0x4, ST_OPEN = 0x5,
       ST_TOC = 0x9, ST_NO_DISC = 0xB, ST_END = 0xC };
#define SCAN_SPEED 30

static struct {
    uint8_t st[10], cmd[10];                       /* RS0-RS9, TC0-TC9 */
    uint8_t audio_off;                             /* $FF8036 high byte: 1 = no audio track playing */
    bool host_clock;
    uint8_t status, pending;                       /* drive state; end state of a pending seek, 0 = none */
    int latency, index, scan;                      /* index: 0-based track */
    long lba;
    long acc;                                      /* 75 Hz: 3 per sub clock against 500000 */
} d;

/* CD-DA: each audio sector played (588 stereo samples at 44.1 kHz, the output's rate) goes into a
 * ring the mixer drains every frame (ps_audio.c); volume from the fader ($FF8034, 1024 = full) */
static int16_t s_cdda[16384][2];
static unsigned s_cdda_w, s_cdda_r, s_fader = 1024;
static void cdda_sector(long lba) {
    uint8_t raw[2352];
    if (!mcd_disc_read((int)lba, raw)) return;
    for (int i = 0; i < 588; i++) {
        int16_t *o = s_cdda[s_cdda_w++ & 16383];
        o[0] = (int16_t)(raw[4 * i] | raw[4 * i + 1] << 8); o[1] = (int16_t)(raw[4 * i + 2] | raw[4 * i + 3] << 8);
    }
    if (s_cdda_w - s_cdda_r > 16384) s_cdda_r = s_cdda_w - 16384;
}
/* the mixer's call: add up to N stereo frames of CD audio to OUT */
void mcd_cdda_mix(int16_t *out, int n) {
    for (int i = 0; i < n && s_cdda_r != s_cdda_w; i++, s_cdda_r++) {
        const int16_t *c = s_cdda[s_cdda_r & 16383];
        for (int k = 0; k < 2; k++) {
            int v = out[2 * i + k] + (int)c[k] * (int)s_fader / 1024;
            out[2 * i + k] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        }
    }
}

static int ntracks(void) { const McdDisc *m = mcd_disc(); return m->loaded ? m->last - m->first + 1 : 0; }
static long track_start(int i) { const McdDisc *m = mcd_disc(); return m->track[m->first + i].lba; }
static long track_end(int i) { const McdDisc *m = mcd_disc(); return i + 1 < ntracks() ? m->track[m->first + i + 1].lba : m->lead_out; }
static bool track_data(int i) { const McdDisc *m = mcd_disc(); return i >= 0 && i < ntracks() && m->track[m->first + i].data; }

static void put_bcd(int at, int v) { d.st[at] = (uint8_t)(v / 10); d.st[at + 1] = (uint8_t)(v % 10); }
static void put_time(long frames) { put_bcd(2, (int)(frames / 75 / 60)); put_bcd(4, (int)(frames / 75 % 60)); put_bcd(6, (int)(frames % 75)); }
static void put_flags(void) { d.st[8] = track_data(d.index) ? 0x4 : 0x0; }
static void checksum(void) { unsigned s = 0; for (int i = 0; i < 9; i++) s += d.st[i]; d.st[9] = (uint8_t)(~s & 0xF); }
/* a status that hides the track infos (RS1 = F) until the drive is ready */
static void not_ready(uint8_t state) {
    memset(d.st, 0, sizeof d.st); d.st[0] = state; d.st[1] = 0xF;
    d.st[9] = (uint8_t)(~(state + 0xF) & 0xF);
}
static int cmd_bcd(int at) { return d.cmd[at] * 10 + d.cmd[at + 1]; }

static void irq4(void) {
    McdIrq *q = &g_mcd.cdd_irq;
    if (!q->pending) { q->pending = true; mcd_irq_sync(); }
}

/* ------------------------------------------------------------------ the 75 Hz tick */
static void update(void) {
    if (d.latency > 0) d.latency--;
    else if (d.status == ST_PLAY) {
        if (d.index >= ntracks()) { d.status = ST_END; return; }
        if (track_data(d.index)) mcd_cdc_decode((int)d.lba);
        else {
            if (d.lba >= track_start(d.index)) { d.audio_off = 0; cdda_sector(d.lba); }
            mcd_cdc_decode_idle();
        }
        d.lba++;
        if (d.lba >= track_end(d.index)) { d.index++; d.audio_off = 1; }
    } else {
        mcd_cdc_decode_idle();
        if (d.status == ST_SCAN) {
            int i = d.index;
            d.lba += d.scan;
            if (d.lba >= track_end(i)) {
                if (++i < ntracks()) d.lba = track_start(i);
                else { d.lba = mcd_disc()->lead_out; d.index = ntracks(); d.status = ST_END; d.audio_off = 1; return; }
            } else if (d.lba < track_start(i)) {
                if (i > 0) d.lba = track_end(--i); else d.lba = 0;
            }
            d.audio_off = track_data(i) ? 1 : 0;
            d.index = i;
        }
    }
    if (d.pending) {                                       /* a Play or Seek sent last tick starts now */
        long lba = ((long)cmd_bcd(2) * 60 + cmd_bcd(4)) * 75 + cmd_bcd(6) - 150;
        if (!d.latency) d.latency = 2;
        d.lba = lba;
        int i = 0;
        while (track_end(i) <= lba && i < ntracks()) i++;
        d.index = i;
        d.audio_off = 1;
        d.status = d.pending;
        d.pending = 0;
    }
}

/* every sub-CPU clock batch (mcd.c): 75 ticks a second, each then level 4 while the host clock runs */
void mcd_cdd_clocks(unsigned clocks) {
    d.acc += (long)clocks * 3;
    while (d.acc >= 500000) {
        d.acc -= 500000;
        {   static FILE *t; static int init; if (!init) { init = 1; const char *f = getenv("PS_CDD_TICKS"); if (f) t = fopen(f, "w"); }
            if (t) { extern long ps_frames; extern uint16_t v_counter; fprintf(t, "%ld %u\n", ps_frames, v_counter); } }
        update();
        if (d.host_clock) irq4();
    }
}
void mcd_cdd_clock(void) {}                              /* the gate array's 384-clock tick: unused here */
void mcd_cdd_sample(void) {}                             /* CD-DA: not yet */

/* ------------------------------------------------------------------ commands */
unsigned long g_mcd_cdd_commands, g_mcd_cdd_ticks;
static void process(void) {
    g_mcd_cdd_commands++;
    switch (d.cmd[0]) {
    case 0x0:                                              /* get drive status */
        if (d.latency <= 3) {
            d.st[0] = d.status;
            if (d.status == ST_STOP || d.status > ST_PAUSE) break;
            if (d.st[1] == 0xF || d.st[1] == 0x0) { d.st[1] = 0; put_time(d.lba + 150); put_flags(); }
            else if (d.st[1] == 0x1) { put_time(labs(d.lba - track_start(d.index))); put_flags(); }
            else if (d.st[1] == 0x2) { if (d.index < ntracks()) put_bcd(2, d.index + 1); else d.st[2] = d.st[3] = 0xA; }
        }
        break;
    case 0x1:                                              /* stop */
        d.status = mcd_disc()->loaded ? ST_TOC : ST_NO_DISC;
        d.audio_off = 1;
        not_ready(ST_STOP);
        d.index = 0; d.lba = 0;
        goto logged;
    case 0x2:                                              /* report */
        switch (d.cmd[3]) {
        case 0x0: memset(d.st + 1, 0, 8); put_time(d.lba + 150); put_flags(); break;
        case 0x1: memset(d.st + 1, 0, 8); d.st[1] = 1; put_time(labs(d.lba - track_start(d.index))); put_flags(); break;
        case 0x2: memset(d.st + 1, 0, 8); d.st[1] = 2; if (d.index < ntracks()) put_bcd(2, d.index + 1); else d.st[2] = d.st[3] = 0xA; break;
        case 0x3: memset(d.st + 1, 0, 8); d.st[1] = 3; put_time(mcd_disc()->lead_out + 150); break;
        case 0x4: memset(d.st + 1, 0, 8); d.st[1] = 4; d.st[3] = 1; put_bcd(4, ntracks()); break;
        case 0x5: {
            int t = cmd_bcd(4);
            memset(d.st + 1, 0, 8); d.st[1] = 5;
            if (t >= 1 && t <= ntracks()) { put_time(track_start(t - 1) + 150); if (track_data(t - 1)) d.st[6] |= 0x8; }
            d.st[8] = (uint8_t)(t % 10);
        } break;
        case 0x6: memset(d.st + 1, 0, 8); d.st[1] = 6; break;
        default: goto logged;
        }
        d.st[0] = d.status;
        break;
    case 0x3: not_ready(ST_SEEK); d.pending = ST_PLAY; goto logged;   /* play: seek first */
    case 0x4: not_ready(ST_SEEK); d.pending = ST_PAUSE; goto logged;  /* seek */
    case 0x6: d.audio_off = 1; d.status = d.st[0] = ST_PAUSE; break;
    case 0x7: d.status = d.st[0] = ST_PLAY; break;
    case 0x8: d.scan = SCAN_SPEED; d.status = d.st[0] = ST_SCAN; break;
    case 0x9: d.scan = -SCAN_SPEED; d.status = d.st[0] = ST_SCAN; break;
    case 0xA: d.audio_off = 1; d.status = d.st[0] = ST_PAUSE; break;
    case 0xC:                                              /* close tray */
        d.audio_off = 1; d.status = mcd_disc()->loaded ? ST_TOC : ST_NO_DISC;
        not_ready(ST_STOP); d.index = 0; d.lba = 0;
        goto logged;
    case 0xD:                                              /* open tray */
        d.audio_off = 1; d.status = ST_OPEN;
        not_ready(ST_OPEN); d.index = 0; d.lba = 0;
        goto logged;
    default: d.st[0] = d.status; break;
    }
    checksum();
logged:
    {   static FILE *log; static int init;                  /* PS_CDD_LOG=file: every command and its status */
        if (!init) { init = 1; const char *f = getenv("PS_CDD_LOG"); if (f) log = fopen(f, "w"); }
        if (log) { extern long ps_frames; fprintf(log, "%ld ", ps_frames);
                   for (int i = 0; i < 10; i++) fprintf(log, "%X", d.cmd[i]);
                   fprintf(log, " "); for (int i = 0; i < 10; i++) fprintf(log, "%X", d.st[i]); fprintf(log, "\n"); fflush(log); } }
}

/* ------------------------------------------------------------------ registers */
uint16_t mcd_cdd_read(uint32_t a) {
    if (a == 0xFF8034) return 0;
    if (a == 0xFF8036) return (uint16_t)(d.audio_off << 8 | d.host_clock << 2);
    if (a >= 0xFF8038 && a <= 0xFF8041) { unsigned i = (a - 0xFF8038) & ~1u; return (uint16_t)(d.st[i] << 8 | d.st[i + 1]); }
    if (a >= 0xFF8042 && a <= 0xFF804B) { unsigned i = (a - 0xFF8042) & ~1u; return (uint16_t)(d.cmd[i] << 8 | d.cmd[i + 1]); }
    return 0;
}
void mcd_cdd_write(uint32_t a, bool upper, bool lower, uint16_t v) {
    if (a == 0xFF8036) { if (lower) d.host_clock = v >> 2 & 1; return; }
    if (a == 0xFF8034) { unsigned f = (v >> 4) & 0x7FF; s_fader = f > 1024 ? 1024 : f; return; }   /* CD fader volume */
    if (a >= 0xFF8042 && a <= 0xFF804B) {
        unsigned i = (a - 0xFF8042) & ~1u;
        if (a == 0xFF804A) { d.cmd[8] = d.cmd[9] = 0; process(); return; }   /* the last word sends the command */
        if (upper) d.cmd[i] = v >> 8 & 0xF;
        if (lower) d.cmd[i + 1] = v & 0xF;
    }
}

/* power and peripheral reset: the drive's state; the registers to the gate array's reset values */
/* the gate array's reset by the sub CPU: the drive's 75 Hz clock restarts from this line's start */
void mcd_cdd_rephase(long clocks_into_line) { d.acc = -clocks_into_line * 3; }
void mcd_cdd_power(void) {
    long acc = d.acc;
    memset(&d, 0, sizeof d);
    d.acc = acc;
    d.status = mcd_disc()->loaded ? ST_TOC : ST_NO_DISC;
    d.audio_off = 1;
    d.st[9] = 0xF;
    memset(d.cmd, 0xF, sizeof d.cmd);
}
uint8_t mcd_cdd_status(void) { return (uint8_t)(d.status | d.host_clock << 4); }
void mcd_cdd_debug(char *out, int n) {
    int k = snprintf(out, n, "cmd"); for (int i = 0; i < 10; i++) k += snprintf(out + k, n - k, "%X", d.cmd[i]);
    k += snprintf(out + k, n - k, " st"); for (int i = 0; i < 10; i++) k += snprintf(out + k, n - k, "%X", d.st[i]);
}
