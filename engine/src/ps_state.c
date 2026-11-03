/* ps_state.c -- machine snapshots: save the whole Mega Drive state, restore it in place.
 *
 * The recompiled game keeps no 68000 state of its own on the C stack between instructions:
 * registers are in g_m68k, every return address is in emulated RAM, and the interpreter
 * (m68k_interp.c) hands over to compiled code exactly at any call or return. So a snapshot
 * is the hardware (RAM, VDP, Z80, YM2612/PSG, I/O, save RAM, bus clock and scheduler) plus
 * the CPU at a known instruction boundary, and a restore resumes the 68000 there through
 * func_table_resume(): interpreted until the code returns or jumps into compiled functions.
 *
 * Snapshot point: the VInt moment of a frame, in the scheduler (bus.c), before the frame's
 * VBlank work (input, the VBlank interrupt, presentation), when
 *   - the scheduler was entered at a plain instruction boundary (g_m68k_ipc_kind == 1: the
 *     compiled code's CYCP or the interpreter's loop top), so the PC to resume at is known
 *     and nothing of the next instruction has run, and
 *   - no interrupt or exception handler is in progress (their context is on the C stack).
 * Otherwise the snapshot waits for the next frame. A restore runs what followed that point:
 * the VBlank work, the rest of the scanline loop, the next event, then the 68000 at the PC
 * (or where the VBlank handler redirected it).
 *
 * Format: "PSSTATE\0", u32 version, u32 ROM hash, u32 payload bytes, u32 sections, then per
 * section u32 tag, u32 bytes, data (host byte order: a state is for the same build family).
 *
 *   PS_STATE_SAVE_AT=POLL:FILE  snapshot at the first snapshot point once POLL polls are done
 *   PS_STATE_LOAD=FILE          start from a snapshot instead of power-on (PS_POLL_REPLAY and
 *                               PS_CHECKPOINTS continue at its poll index)
 *   PS_STATE_RELOAD=POLL:FILE   at POLL, restore FILE in place, from inside the running game
 *                               (the pause menu's LOAD STATE path)
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <stdbool.h>
#include "ps_portable.h"

int ps_state_armed;                        /* bus.c calls ps_state_vblank_point() when set */

#ifdef PS_NATIVE
#include "genrecomp/m68k.h"
#include "genrecomp/func_table.h"
#include "ps_hw.h"

#define PS_STATE_VERSION 1

void recomp_m68k_state(void), bus_state(void), ps_hw_state(void), ps_vdp_state(void), ps_audio_state(void),
     ps_synth_state(void), ps_poll_state(void), genrecomp_state(void), ps_render_state(void);
uint32_t ps_hw_rom_hash(void);
bool recomp_m68k_in_exception(void);
void bus_state_continue(void);
void func_table_resume(uint32_t pc);
void func_table_reset_frames(void), recomp_m68k_reset_frames(void);
long ps_poll_index(void);
extern long ps_frames;

static const struct { uint32_t tag; void (*fn)(void); } k_sections[] = {
    { 0x43505530, recomp_m68k_state },    /* CPU0 */
    { 0x42555330, bus_state },            /* BUS0 */
    { 0x48575230, ps_hw_state },          /* HWR0 */
    { 0x56445030, ps_vdp_state },         /* VDP0 */
    { 0x5A383030, ps_audio_state },       /* Z800 */
    { 0x53594E30, ps_synth_state },       /* SYN0 */
    { 0x504F4C30, ps_poll_state },        /* POL0 */
    { 0x46524D30, genrecomp_state },      /* FRM0 */
    { 0x524E4430, ps_render_state },      /* RND0 */
};
#define NSECT (int)(sizeof k_sections / sizeof k_sections[0])

/* serialisation: every module lists its state through ps_state_blob */
enum { IO_NONE, IO_SIZE, IO_SAVE, IO_LOAD };
static int s_io;
static uint8_t *s_buf; static size_t s_len, s_cap, s_pos; static int s_err;

static void put(const void *p, size_t n) {
    if (s_len + n > s_cap) { s_cap = (s_len + n) * 2; s_buf = realloc(s_buf, s_cap); }
    memcpy(s_buf + s_len, p, n); s_len += n;
}
static void put32(uint32_t v) { put(&v, 4); }
static uint32_t get32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

void ps_state_blob(void *p, unsigned long n) {
    switch (s_io) {
    case IO_SIZE: s_len += n; break;
    case IO_SAVE: put(p, n); break;
    case IO_LOAD:
        if (s_pos + n > s_len) { s_err = 1; return; }
        memcpy(p, s_buf + s_pos, n); s_pos += n; break;
    default: break;
    }
}

static size_t section_size(int i) {
    int io = s_io; size_t len = s_len;
    s_io = IO_SIZE; s_len = 0; k_sections[i].fn(); size_t n = s_len;
    s_io = io; s_len = len;
    return n;
}

/* the snapshot as bytes (malloc'd; *n its length) */
static uint8_t *capture(size_t *n) {
    s_buf = NULL; s_len = s_cap = 0;
    put("PSSTATE", 8); put32(PS_STATE_VERSION); put32(ps_hw_rom_hash()); put32(0); put32(NSECT);
    for (int i = 0; i < NSECT; i++) {
        size_t sz = section_size(i);
        put32(k_sections[i].tag); put32((uint32_t)sz);
        s_io = IO_SAVE; k_sections[i].fn(); s_io = IO_NONE;
    }
    uint32_t payload = (uint32_t)(s_len - 24); memcpy(s_buf + 16, &payload, 4);
    *n = s_len;
    uint8_t *b = s_buf; s_buf = NULL; s_len = s_cap = 0;
    return b;
}

/* same version, same ROM, every section the size this build lists: 0, else why not */
const char *ps_state_check(const uint8_t *b, size_t n) {
    if (n < 24 || memcmp(b, "PSSTATE", 8)) return "not a state file";
    if (get32(b + 8) != PS_STATE_VERSION) return "other state version";
    if (get32(b + 12) != ps_hw_rom_hash()) return "other ROM";
    if (get32(b + 16) != n - 24 || get32(b + 20) != (uint32_t)NSECT) return "truncated or other layout";
    size_t p = 24;
    for (int i = 0; i < NSECT; i++) {
        if (p + 8 > n || get32(b + p) != k_sections[i].tag || get32(b + p + 4) != section_size(i)) return "other layout";
        p += 8 + get32(b + p + 4);
    }
    return p == n ? NULL : "other layout";
}

static void apply(const uint8_t *b, size_t n) {
    s_buf = (uint8_t *)b; s_len = n; s_pos = 24; s_err = 0;
    for (int i = 0; i < NSECT; i++) { s_pos += 8; s_io = IO_LOAD; k_sections[i].fn(); s_io = IO_NONE; }
    s_buf = NULL; s_len = s_pos = 0;
}

/* ------------------------------------------------------------------ taking one */
static long s_save_at = -1;                /* PS_STATE_SAVE_AT poll */
static char s_save_path[1024];
static void (*s_save_cb)(const uint8_t *, size_t, long);   /* a requested snapshot (menu) */
static long s_reload_at = -1;
static char s_reload_path[1024];
/* rewind: the last RW_SLOTS snapshots in memory, one every RW_EVERY frames, while a session is played
 * (PS_POLL_RECORD, set by play.sh and the app; PS_REWIND=1 forces it, PS_NO_REWIND=1 turns it off);
 * the menu steps back through them (ps_menu.c) */
enum { RW_SLOTS = 30, RW_EVERY = 60, RW_TW = 160, RW_TH = 112 };
static struct { uint8_t *b; size_t n; long poll, frame; uint32_t thumb[RW_TW * RW_TH]; } s_rw[RW_SLOTS];
static int s_rw_head, s_rw_count, s_rw_on; static long s_rw_last = -1000000;
static void rearm(void) { ps_state_armed = s_save_at >= 0 || s_save_cb || s_reload_at >= 0 || s_rw_on; }
static void rewind_tick(long poll) {
    if (!s_rw_on || ps_frames - s_rw_last < RW_EVERY) return;
    s_rw_last = ps_frames;
    free(s_rw[s_rw_head].b);
    s_rw[s_rw_head].b = capture(&s_rw[s_rw_head].n); s_rw[s_rw_head].poll = poll; s_rw[s_rw_head].frame = ps_frames;
    {   /* its picture for the timeline: the frame on screen, half size */
        const uint8_t *genrecomp_get_framebuffer(void); int genrecomp_present_width(void);
        const uint32_t *f = (const uint32_t *)genrecomp_get_framebuffer(); int w = genrecomp_present_width();
        for (int y = 0; y < RW_TH; y++)
            for (int x = 0; x < RW_TW; x++) s_rw[s_rw_head].thumb[y * RW_TW + x] = f[(y * 2) * w + x * w / RW_TW];
    }
    s_rw_head = (s_rw_head + 1) % RW_SLOTS;
    if (s_rw_count < RW_SLOTS) s_rw_count++;
}
/* the timeline (ps_menu.c): how many snapshots are held, and the picture (RW_TW x RW_TH) and age in
 * frames of the one STEPS back (1: the newest) */
int ps_state_rewind_count(void) { return s_rw_count; }
const uint32_t *ps_state_rewind_thumb(int steps, int *w, int *h, long *age) {
    if (steps < 1 || steps > s_rw_count) return NULL;
    int i = (s_rw_head + RW_SLOTS - steps) % RW_SLOTS;
    *w = RW_TW; *h = RW_TH; *age = ps_frames - s_rw[i].frame;
    return s_rw[i].thumb;
}
/* the snapshot STEPS back (1: the newest; fewer if the ring holds fewer), the newer ones dropped, its
 * poll in *poll; the caller owns it. NULL when there is none */
uint8_t *ps_state_rewind_take(int steps, size_t *n, long *poll) {
    if (!s_rw_count) return NULL;
    if (steps > s_rw_count) steps = s_rw_count;
    for (int k = 1; k < steps; k++) { s_rw_head = (s_rw_head + RW_SLOTS - 1) % RW_SLOTS; free(s_rw[s_rw_head].b); s_rw[s_rw_head].b = NULL; s_rw_count--; }
    s_rw_head = (s_rw_head + RW_SLOTS - 1) % RW_SLOTS; s_rw_count--;
    uint8_t *b = s_rw[s_rw_head].b; *n = s_rw[s_rw_head].n; *poll = s_rw[s_rw_head].poll; s_rw[s_rw_head].b = NULL;
    s_rw_last = ps_frames;                 /* the next one a full step after the restore */
    return b;
}

void ps_state_request(void (*done)(const uint8_t *, size_t, long)) { s_save_cb = done; rearm(); }

static bool write_file(const char *path, const uint8_t *b, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(b, 1, n, f) == n;
    return fclose(f) == 0 && ok;
}

/* PS_STATE_RELOAD=POLL:FILE: at POLL, load FILE in place (the menu's LOAD STATE path) */
const char *ps_state_load_now(uint8_t *b, size_t n);
uint8_t *ps_state_read_file(const char *path, size_t *n);
void ps_state_vblank_point(void) {
    if (s_reload_at >= 0 && ps_poll_index() >= s_reload_at) {
        s_reload_at = -1; rearm();
        size_t n; uint8_t *b = ps_state_read_file(s_reload_path, &n);
        fprintf(stderr, "ps_state: reloading %s at poll %ld\n", s_reload_path, ps_poll_index());
        const char *why = b ? ps_state_load_now(b, n) : "cannot read";
        fprintf(stderr, "ps_state: reload refused: %s\n", why);
    }
    if (g_m68k_ipc_kind != 1 || !g_m68k_ipc || g_m68k_redirect || recomp_m68k_in_exception()) return;
    long poll = ps_poll_index();
    if (s_save_at >= 0 && poll >= s_save_at) {
        size_t n; uint8_t *b = capture(&n);
        bool ok = write_file(s_save_path, b, n);
        fprintf(stderr, "ps_state: %s %s at poll %ld, frame %ld, PC $%06X (%zu bytes)\n", ok ? "saved" : "cannot write",
                s_save_path, poll, ps_frames, g_m68k_ipc, n);
        free(b); s_save_at = -1;
    }
    rewind_tick(poll);
    {   /* PS_REWIND_AT=FRAME:STEPS: a rewind at that frame, as the key does (tests) */
        static long at = -2; static int steps;
        if (at == -2) { const char *e = getenv("PS_REWIND_AT"); at = e ? atol(e) : -1; steps = e && strchr(e, ':') ? atoi(strchr(e, ':') + 1) : 1; }
        if (at >= 0 && ps_frames >= at) { at = -1; void ps_menu_rewind(int); ps_menu_rewind(steps); }
    }
    if (s_save_cb) {
        size_t n; uint8_t *b = capture(&n);
        void (*cb)(const uint8_t *, size_t, long) = s_save_cb; s_save_cb = NULL;
        cb(b, n, poll);
        free(b);
    }
    rearm();
}

/* ------------------------------------------------------------------ restoring one */
static uint8_t *s_load; static size_t s_load_n;
static jmp_buf s_root; static bool s_root_set;

/* the restore itself, from the root of the C stack (nothing of the old run left on it) */
static void resume_loaded(void) {
    uint8_t *b = s_load; size_t n = s_load_n; s_load = NULL;
    func_table_reset_frames(); recomp_m68k_reset_frames();
    apply(b, n); free(b);
    fprintf(stderr, "ps_state: restored at poll %ld, frame %ld, PC $%06X\n", ps_poll_index(), ps_frames, g_m68k_ipc);
    uint32_t pc = g_m68k_ipc;
    bus_state_continue();                  /* VBlank work, scanlines, next event (as bus.c would) */
    uint32_t to = g_m68k_redirect ? g_m68k_redirect : pc;
    g_m68k_redirect = 0; g_m68k_ipc = 0; g_m68k_ipc_kind = 0;
    func_table_resume(to);
}

/* func_table_call at depth 0 (main's power-on call) sets the root a load unwinds to: */
jmp_buf *ps_state_jmpbuf(void) { s_root_set = true; return &s_root; }
bool ps_state_pending(void) { return s_load != NULL; }
void ps_state_resume_loaded(void) { resume_loaded(); }

/* Load b (taken over, malloc'd) now: unwinds the running game to the root. Returns only if
 * refused (then nothing changed), with the reason. */
const char *ps_state_load_now(uint8_t *b, size_t n) {
    const char *why = ps_state_check(b, n);
    if (why) { free(b); return why; }
    if (!s_root_set) { free(b); return "no root"; }
    s_load = b; s_load_n = n;
    longjmp(s_root, 1);
}

uint8_t *ps_state_read_file(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = len > 0 ? malloc((size_t)len) : NULL;
    if (b && fread(b, 1, (size_t)len, f) != (size_t)len) { free(b); b = NULL; }
    fclose(f);
    *n = b ? (size_t)len : 0;
    return b;
}

PS_INITIALIZER(ps_state_init)
{
    const char *e;
    if ((e = getenv("PS_STATE_SAVE_AT")) && *e) {
        const char *c = strchr(e, ':');
        if (c) { s_save_at = atol(e); snprintf(s_save_path, sizeof s_save_path, "%s", c + 1); }
    }
    s_rw_on = !getenv("PS_NO_REWIND") && ((e = getenv("PS_POLL_RECORD")) && *e || getenv("PS_REWIND"));
    if ((e = getenv("PS_STATE_RELOAD")) && *e) {
        const char *c = strchr(e, ':');
        if (c) { s_reload_at = atol(e); snprintf(s_reload_path, sizeof s_reload_path, "%s", c + 1); }
    }
    rearm();
}

/* PS_STATE_LOAD: checked once the ROM is loaded (the hash), at the first root call */
bool ps_state_boot_load(void) {
    static int done;
    if (done++) return false;
    const char *e = getenv("PS_STATE_LOAD");
    if (!e || !*e) return false;
    size_t n; uint8_t *b = ps_state_read_file(e, &n);
    const char *why = b ? ps_state_check(b, n) : "cannot read";
    if (why) { fprintf(stderr, "ps_state: %s: %s, powering on instead\n", e, why); free(b); return false; }
    s_load = b; s_load_n = n;
    return true;
}

#else   /* oracle build (Genesis Plus GX machine): no snapshots */
void ps_state_blob(void *p, unsigned long n) { (void)p; (void)n; }
void ps_state_vblank_point(void) {}
static jmp_buf s_root;
jmp_buf *ps_state_jmpbuf(void) { return &s_root; }
bool ps_state_pending(void) { return false; }
void ps_state_resume_loaded(void) {}
bool ps_state_boot_load(void) { return false; }
void ps_state_request(void (*done)(const uint8_t *, size_t, long)) { (void)done; }
const char *ps_state_check(const uint8_t *b, size_t n) { (void)b; (void)n; return "no snapshots in this build"; }
const char *ps_state_load_now(uint8_t *b, size_t n) { (void)n; free(b); return "no snapshots in this build"; }
uint8_t *ps_state_read_file(const char *path, size_t *n) { (void)path; *n = 0; return NULL; }
uint8_t *ps_state_rewind_take(int steps, size_t *n, long *poll) { (void)steps; *n = 0; *poll = 0; return NULL; }
int ps_state_rewind_count(void) { return 0; }
const uint32_t *ps_state_rewind_thumb(int steps, int *w, int *h, long *age) { (void)steps; *w = *h = 0; *age = 0; return NULL; }
#endif
