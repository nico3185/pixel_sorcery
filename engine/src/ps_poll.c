/* ps_poll.c — controller-poll events: input record/replay and RAM checkpoints.
 *
 * A "poll" is a TH 0->1 write to port A, which the game does once per input read.
 * Keying inputs and checkpoints by poll index (not video frame) keeps the recompiled
 * build and the original on the oracle aligned even when their lag frames differ.
 * Linked into both the recompiled game and the GPGX tracer (same hook in io_ctrl.c).
 *
 *   PS_POLL_RECORD=file   write "P1 P2" (hex) per poll
 *   PS_POLL_REPLAY=file   feed recorded pads per poll (keyboard ignored)
 *   PS_CHECKPOINTS=file   per poll: index + 256 hashes of 256-byte blocks of 68K RAM
 *   PS_CHECKPOINT_DUMP=N:dir   full 68K RAM (and Z80 RAM) at poll N into dir/ram_N.bin
 *   PS_RESUME=file:N      replay the first N polls of a recorded session at full speed, then
 *                         hand control to the player (resume a run; PS_POLL_RECORD keeps the
 *                         whole session, replayed part included, so it still replays exactly)
 *   PS_POLL_STOP=N        exit after poll N (replay to a divergence and stop)
 *   PS_STATE_DUMP=dir:N   every N polls: dir/state_<poll>.bin = 68K RAM 64K | Z80 RAM 8K | VRAM 64K |
 *                         CRAM 128 | VSRAM 128 | VDP regs 32 (GPGX native byte order)
 *   PS_AUDIO_LOG=file     every YM2612/PSG write: "poll line chip addr data"
 *   PS_CHECKPOINT_MASK=a-b,c-d   68K RAM offsets (hex, end exclusive) zeroed before hashing:
 *                         known-benign differences such as stack scratch
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ps_hw.h"
#include "ps_portable.h"
#ifdef PS_NATIVE
#include "genrecomp/ps_vdp.h"
#endif

static FILE *s_rec, *s_ck;
static uint16_t (*s_play)[2];
static uint16_t s_cur[2];          /* replay: pad value held until the next poll */
static long s_play_n, s_poll, s_dump_at = -1, s_stop = -1;
static char s_dump_dir[512];
static unsigned s_th = 0x40;
static int s_mask[32][2], s_nmask;
static char s_state_dir[512]; static long s_state_every;
static FILE *s_alog;
static int s_live_after;           /* PS_RESUME: live input once the replay is used up */
int g_ps_boot_hidden;   /* booting unseen (a disc's BIOS menu): nothing shown or heard, no pacing */
int ps_poll_fast_forward(void) { return (s_play && s_live_after) || g_ps_boot_hidden; }
#ifndef PS_NATIVE
static void (*s_fm_w)(unsigned int, unsigned int, unsigned int);
static void alog_fm(unsigned int c, unsigned int a, unsigned int v)
{ fprintf(s_alog, "%ld %d FM %u %02X\n", s_poll, v_counter, a, v & 0xFF); s_fm_w(c, a, v); }
static int alog_psg(unsigned int c, unsigned int v)
{ (void)c; fprintf(s_alog, "%ld %d PSG 0 %02X\n", s_poll, v_counter, v & 0xFF); return 0; }
#endif
static uint8_t s_tmp[0x10000];            /* port A TH as last driven (power-on: pulled high) */

long ps_poll_index(void) { return s_poll; }
long ps_frames;                          /* frames (VBlanks) since power-on, both sides */

static uint32_t fnv(const uint8_t *p, size_t n)
{
    uint32_t h = 2166136261u;
    while (n--) { h ^= *p++; h *= 16777619u; }
    return h;
}

static void on_poll(void)
{
    long n = s_poll++;
#ifndef PS_NATIVE
    if (s_alog && !s_fm_w && fm_write) { s_fm_w = fm_write; fm_write = alog_fm; psg_write_hook = alog_psg; }
#endif
    if (s_state_every > 0 && n % s_state_every == 0) {
        char path[600];
        snprintf(path, sizeof path, "%s/state_%07ld.bin", s_state_dir, n);
        FILE *f = fopen(path, "wb");
        if (f) {
            fwrite(work_ram, 1, 0x10000, f); fwrite(zram, 1, 0x2000, f);
#ifdef PS_NATIVE
            fwrite(g_ps_vdp.vram, 1, 0x10000, f); fwrite(g_ps_vdp.cram, 1, 0x80, f);
            fwrite(g_ps_vdp.vsram, 1, 0x50, f); { static const uint8_t z[0x30]; fwrite(z, 1, 0x30, f); }
            fwrite(g_ps_vdp.reg, 1, 0x20, f); fclose(f);
#else
            fwrite(vram, 1, 0x10000, f);
            fwrite(cram, 1, 0x80, f); fwrite(vsram, 1, 0x80, f); fwrite(reg, 1, 0x20, f); fclose(f);
#endif
        }
    }
    if (s_play && s_live_after && n >= s_play_n) {
        free(s_play); s_play = NULL;
        fprintf(stderr, "ps_poll: resumed at poll %ld, live input\n", n);
    }
    if (s_play) {
        if (n < s_play_n) { s_cur[0] = s_play[n][0]; s_cur[1] = s_play[n][1]; }
        else { s_cur[0] = s_cur[1] = 0; }
        input.pad[0] = s_cur[0]; input.pad[PS_PAD_INDEX(1)] = s_cur[1];
    }
    if (s_rec) fprintf(s_rec, "%04X %04X\n", input.pad[0], input.pad[PS_PAD_INDEX(1)]);
    if (s_ck) {
        memcpy(s_tmp, work_ram, sizeof s_tmp);
        for (int m = 0; m < s_nmask; m++) memset(s_tmp + s_mask[m][0], 0, s_mask[m][1] - s_mask[m][0]);
        fprintf(s_ck, "%ld f=%ld", n, ps_frames);
        for (int b = 0; b < 256; b++) fprintf(s_ck, " %08X", fnv(s_tmp + b * 0x100, 0x100));
        fputc('\n', s_ck);
    }
    if (n == s_dump_at) {
        char path[600];
        snprintf(path, sizeof path, "%s/ram_%ld.bin", s_dump_dir, n);
        FILE *f = fopen(path, "wb");
        if (f) { fwrite(work_ram, 1, 0x10000, f); fwrite(zram, 1, 0x2000, f); fclose(f); }
    }
    if (n == s_stop) {
        if (s_rec) fflush(s_rec);
        if (s_ck) fflush(s_ck);
        fprintf(stderr, "ps_poll: reached poll %ld, exiting\n", n);
        exit(0);
    }
}

static void pad_write_hook(int port, unsigned int data, unsigned int ctrl)
{
    if (s_play) { input.pad[0] = s_cur[0]; input.pad[PS_PAD_INDEX(1)] = s_cur[1]; }   /* frame-start input updates must not override the recording */
    if (port != 0) return;
    unsigned th = (ctrl & 0x40) ? (data & 0x40) : 0x40;
    /* The game may read the TH-low half (Start/A) before the TH 0->1 edge that counts as
     * the poll; live, that read saw the same frame's pad, i.e. the upcoming poll's value. */
    if (!th && s_th && s_play) {
        long n = s_poll < s_play_n ? s_poll : s_play_n - 1;
        if (n >= 0) { s_cur[0] = s_play[n][0]; s_cur[1] = s_play[n][1]; input.pad[0] = s_cur[0]; input.pad[PS_PAD_INDEX(1)] = s_cur[1]; }
    }
    if (th && !s_th) on_poll();
    s_th = th;
}

static void load_replay(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "ps_poll: cannot open %s\n", path); return; }
    long cap = 1 << 16;
    s_play = malloc(sizeof *s_play * cap);
    unsigned a, b;
    while (fscanf(f, "%x %x", &a, &b) == 2) {
        if (s_play_n == cap) { cap *= 2; s_play = realloc(s_play, sizeof *s_play * cap); }
        s_play[s_play_n][0] = (uint16_t)a; s_play[s_play_n][1] = (uint16_t)b; s_play_n++;
    }
    fclose(f);
    fprintf(stderr, "ps_poll: replaying %ld polls from %s\n", s_play_n, path);
}

void ps_poll_flush(void) { if (s_rec) fflush(s_rec); }
/* a restored snapshot (menu LOAD STATE): the recording is rewritten to the slot's inputs */
void ps_poll_record_close(void) { if (s_rec) { fclose(s_rec); s_rec = NULL; } }
void ps_poll_record_reopen(void) { const char *e = getenv("PS_POLL_RECORD"); if (e && *e) s_rec = fopen(e, "a"); }
static void flush_all(void) { if (s_rec) fclose(s_rec); if (s_ck) fclose(s_ck); if (s_alog) fclose(s_alog); }

PS_INITIALIZER(ps_poll_init)
{
    const char *e;
    if ((e = getenv("PS_POLL_REPLAY")) && *e) load_replay(e);
    if ((e = getenv("PS_RESUME")) && *e) {
        char path[512]; const char *c = strrchr(e, ':');
        snprintf(path, sizeof path, "%.*s", c ? (int)(c - e) : (int)strlen(e), e);
        load_replay(path);
        if (s_play && c && atol(c + 1) < s_play_n) s_play_n = atol(c + 1);
        s_live_after = 1;
    }
    if ((e = getenv("PS_POLL_RECORD")) && *e) s_rec = fopen(e, "w");
    if ((e = getenv("PS_CHECKPOINTS")) && *e) s_ck = fopen(e, "w");
    if ((e = getenv("PS_CHECKPOINT_DUMP")) && *e) {
        s_dump_at = atol(e);
        const char *c = strchr(e, ':');
        snprintf(s_dump_dir, sizeof s_dump_dir, "%s", c ? c + 1 : ".");
    }
    if ((e = getenv("PS_POLL_STOP")) && *e) s_stop = atol(e);
    if ((e = getenv("PS_STATE_DUMP")) && *e) {
        const char *c = strrchr(e, ':');                /* last ':' (Windows paths have C:) */
        if (c && (c[1] < '0' || c[1] > '9')) c = NULL;  /* "C:\dir" alone: no interval */
        snprintf(s_state_dir, sizeof s_state_dir, "%.*s", c ? (int)(c - e) : (int)strlen(e), e);
        s_state_every = c ? atol(c + 1) : 60;
    }
    if ((e = getenv("PS_AUDIO_LOG")) && *e) s_alog = fopen(e, "w");
    if ((e = getenv("PS_CHECKPOINT_MASK")) && *e) {
        for (const char *p = e; *p && s_nmask < 32; ) {
            char *q; long a = strtol(p, &q, 16), b = (*q == '-') ? strtol(q + 1, &q, 16) : a + 2;
            if (a >= 0 && b <= 0x10000 && a < b) { s_mask[s_nmask][0] = (int)a; s_mask[s_nmask][1] = (int)b; s_nmask++; }
            p = (*q == ',') ? q + 1 : q; if (!*q) break;
        }
    }
    if (s_play || s_rec || s_ck || s_dump_at >= 0 || s_stop >= 0 || s_state_every || s_alog) {
        io_pad_write_hook = pad_write_hook;
        atexit(flush_all);
    }
}

/* ps_state.c: poll counter, TH line, the pad value held, frames since power-on */
void ps_state_blob(void *p, unsigned long n);
void ps_poll_state(void) {
#define B(v) ps_state_blob(&(v), sizeof (v))
    B(s_poll); B(s_th); B(s_cur); B(ps_frames);
#undef B
}
