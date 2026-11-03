/*
 * genrecomp reference runner: plays a ROM on Genesis Plus GX's own 68K
 * interpreter, with the same flags as a recomp (--headless, --record,
 * --frames, --press, --ram-dump). It is the ground truth a recomp is
 * compared against: record both, or dump RAM from both and diff, and the
 * first frame they disagree on is where the recompiled code went wrong.
 *
 *   genrecomp_ref --headless --frames 1800 --ram-dump ref:60 rom.gen
 *
 * Playing from an explorer's stuck point (tools/explore): GENRECOMP_STATE=stuck_N.state
 * starts there, GENRECOMP_PATCH=ADDR=WORD[,...] applies the run's ROM cheats,
 * GENRECOMP_PLAY_OUT=file records the pads from there ("P1 P2" hex per frame, written
 * on exit), R rewinds to the state (the recording restarts with it), and Backspace steps
 * back one second at a time over the last minute (the recording is cut to match).
 */

#include <genrecomp/genrecomp.h>
#include <genrecomp/platform.h>
#include <genrecomp/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>

#include "shared.h"
#include "../../src/ps_png.h"

static uint8_t *s_state; static size_t s_state_len;
static uint16_t *s_play; static long s_play_n, s_play_cap;
#define REW_N 64                                     /* one state per second, the last minute */
static uint8_t *s_rew[REW_N]; static long s_rew_play[REW_N]; static int s_rew_head, s_rew_count;

void ps_megacd_bios(void); void ps_megacd_brm(bool load_files, bool save);

static void write_play(void) {
    const char *out = getenv("GENRECOMP_PLAY_OUT");
    FILE *f = out ? fopen(out, "w") : NULL;
    if (!f) return;
    for (long i = 0; i < s_play_n; i++) fprintf(f, "%04X 0000\n", s_play[i]);
    fclose(f);
    printf("ref: %ld frames of play -> %s\n", s_play_n, out);
}

#ifdef HOOK_CPU
/* PS_PCRING=1 (builds with HOOK_CPU, e.g. build-trace): the main CPU's last 512 instruction
 * addresses, appended to PS_RAMDUMP_AT's dump (oldest first) - the path into a crash */
#include "debug/cpuhook.h"
static uint32_t s_pcring[512]; static unsigned s_pcpos;
static void pc_hook(hook_type_t t, int w, unsigned int a, unsigned int v) {
    (void)w; (void)v;
    /* stops once a game running from program/word RAM ($020000-$23FFFF) reaches the BIOS
     * ROM below $020000 or its error slots ($FFFD18-$FFFDFF): a jump the relocation missed */
    static int frozen, game;
    if (t != HOOK_M68K_E || frozen) return;
    s_pcring[s_pcpos++ & 511] = a;
    a &= 0xFFFFFF;
    if (a >= 0x020000 && a < 0x240000) game = 1;
    else if (game && (a < 0x020000 || (a >= 0xFFFD18 && a < 0xFFFE00))) frozen = 1;
}
#endif

int main(int argc, char *argv[]) {
    argc = platform_parse_args(argc, argv);
#ifdef HOOK_CPU
    if (getenv("PS_PCRING")) set_cpu_hook(pc_hook);
#endif
    if (argc < 2) {
        fprintf(stderr, "usage: genrecomp_ref [--headless] [--record out.mp4] "
                        "[--frames N] [--press F:BTN[:LEN]] [--ram-dump DIR:EVERY] rom.gen\n");
        return 1;
    }
    if (!genrecomp_init("genrecomp reference", 3)) return 1;

    /* Not genrecomp_load_rom: that stubs Z80 bus control for recompiled
     * code, and the reference must run the untouched hardware. */
    ps_megacd_bios();                                  /* Mega CD discs: .cue/.iso/.chd */
    char path[MAXPATHLEN];
    strncpy(path, argv[1], sizeof(path) - 1);
    path[sizeof(path) - 1] = 0;
    { void ps_60hz(const char *); ps_60hz(path); }   /* discs: their region's BIOS, 60 Hz (as genrecomp) */
    if (!load_rom(path)) { fprintf(stderr, "ref: cannot load %s\n", argv[1]); return 1; }
    audio_init(44100, 0);
    input.system[0] = SYSTEM_GAMEPAD;  /* before system_init: io_init binds ports */
    input.system[1] = SYSTEM_GAMEPAD;
    system_init();
    system_reset();
    ps_megacd_brm(!platform_is_headless(), false);
    const char *patch = getenv("GENRECOMP_PATCH");
    for (const char *q = patch; q && *q; ) {                /* GPGX keeps the ROM word-swapped */
        unsigned a, v;
        if (sscanf(q, "%x=%x", &a, &v) == 2) { cart.rom[a ^ 1] = (uint8)(v >> 8); cart.rom[(a + 1) ^ 1] = (uint8)v; }
        q = strchr(q, ',');
        if (q) q++;
    }
    const char *st = getenv("GENRECOMP_STATE");
    if (st && *st) {
        FILE *f = fopen(st, "rb");
        if (!f) { fprintf(stderr, "ref: cannot open %s\n", st); return 1; }
        s_state = malloc(STATE_SIZE);
        s_state_len = fread(s_state, 1, STATE_SIZE, f);
        fclose(f);
        state_load(s_state);
        printf("ref: from %s (R rewinds to it)\n", st);
    }

    static uint8_t present[GEN_RENDER_WIDTH * GEN_RENDER_HEIGHT * 4];
    static int16_t audio[4096];
    for (;;) {
        if (!platform_poll_events()) break;
        if (s_state) {   /* R: back to the saved state, recording restarts */
            static int prev;
            int r = SDL_GetKeyboardState(NULL)[SDL_SCANCODE_R];
            if (r && !prev) { state_load(s_state); s_play_n = 0; s_rew_count = 0; printf("ref: rewound\n"); fflush(stdout); }
            prev = r;
            static int prevb;
            int b = SDL_GetKeyboardState(NULL)[SDL_SCANCODE_BACKSPACE];
            if (b && !prevb && s_rew_count > 1) {      /* the newest is now: go to the one before */
                s_rew_count--;
                s_rew_head = (s_rew_head + REW_N - 1) % REW_N;
                int i = (s_rew_head + REW_N - 1) % REW_N;
                state_load(s_rew[i]);
                s_play_n = s_rew_play[i];
                printf("ref: back 1 s\n"); fflush(stdout);
            }
            prevb = b;
            static long n;
            if (n++ % 60 == 0) {                         /* a rewind point every second */
                if (!s_rew[s_rew_head]) s_rew[s_rew_head] = malloc(STATE_SIZE);
                state_save(s_rew[s_rew_head]);
                s_rew_play[s_rew_head] = s_play_n;
                s_rew_head = (s_rew_head + 1) % REW_N;
                if (s_rew_count < REW_N) s_rew_count++;
            }
        }
        recomp_input_update();
        if (s_state) {
            if (s_play_n == s_play_cap) s_play = realloc(s_play, (size_t)(s_play_cap = s_play_cap ? s_play_cap * 2 : 4096) * sizeof *s_play);
            s_play[s_play_n++] = recomp_input_read_pad(0);
        }
        if (system_hw == SYSTEM_MCD) system_frame_scd(0); else system_frame_gen(0);
        {   /* PS_WAV_OUT=FILE: the sound (44.1 kHz stereo, 16-bit) for recordings */
            int ns = audio_update(audio);
            static FILE *wf; static int init; static long bytes;
            if (!init) { init = 1; const char *w = getenv("PS_WAV_OUT"); if (w && *w && (wf = fopen(w, "wb"))) fwrite("RIFF\0\0\0\0WAVEfmt \x10\0\0\0\1\0\2\0\x44\xAC\0\0\x10\xB1\2\0\4\0\x10\0data\0\0\0\0", 1, 44, wf); }
            if (wf && ns > 0) {
                fwrite(audio, 4, (size_t)ns, wf); bytes += ns * 4;
                long pos = ftell(wf); uint32_t r = (uint32_t)(bytes + 36), d = (uint32_t)bytes;
                fseek(wf, 4, SEEK_SET); fwrite(&r, 4, 1, wf); fseek(wf, 40, SEEK_SET); fwrite(&d, 4, 1, wf); fseek(wf, pos, SEEK_SET);
                fflush(wf);
            }
        }

        memset(present, 0, sizeof(present));
        int w = bitmap.viewport.w < GEN_RENDER_WIDTH ? bitmap.viewport.w : GEN_RENDER_WIDTH;
        int h = bitmap.viewport.h < GEN_RENDER_HEIGHT ? bitmap.viewport.h : GEN_RENDER_HEIGHT;
        const uint8_t *src = bitmap.data + bitmap.viewport.y * bitmap.pitch + bitmap.viewport.x * 4;
        for (int y = 0; y < h; y++)
            memcpy(present + y * GEN_RENDER_WIDTH * 4, src + y * bitmap.pitch, (size_t)w * 4);
        {   /* PS_FRAMES_DIR=DIR [PS_FRAMES_EVERY=N]: PNGs of the frames, for headless checks */
            static const char *fd; static long every, fn; static int init;
            if (!init) { init = 1; fd = getenv("PS_FRAMES_DIR"); every = getenv("PS_FRAMES_EVERY") ? atol(getenv("PS_FRAMES_EVERY")) : 60; if (every < 1) every = 1; }
            if (fd && *fd && fn % every == 0) { char png[1024]; snprintf(png, sizeof png, "%s/f%06ld.png", fd, fn); ps_png_write(png, present, GEN_RENDER_WIDTH, h); }
            fn++;
        }
        {   /* PS_RAMDUMP_AT=FRAME:FILE: work RAM (64 KB, as the 68000 sees it), CRAM (128 bytes, host
             * order) and the 68000's D0-D7, A0-A7, PC (4 bytes each, host order) at that frame, to compare a disc build with its cartridge */
            static long at = -2, n; static const char *file;
            if (at == -2) { const char *e = getenv("PS_RAMDUMP_AT"); at = -1;
                            if (e && strchr(e, ':')) { at = atol(e); file = strchr(e, ':') + 1; } }
            if (n++ == at) {
                FILE *f = fopen(file, "wb");
                if (f) { uint8_t r[0x10000];
                         for (int i = 0; i < 0x10000; i += 2) { r[i] = work_ram[i + 1]; r[i + 1] = work_ram[i]; }  /* GPGX keeps words byte-swapped */
                         fwrite(r, 1, sizeof r, f); fwrite(cram, 1, sizeof cram, f);
                         for (int k = M68K_REG_D0; k <= M68K_REG_PC; k++) { uint32_t v = m68k_get_reg((m68k_register_t)k); fwrite(&v, 4, 1, f); }
#ifdef HOOK_CPU
                         for (int k = 0; k < 512; k++) fwrite(&s_pcring[(s_pcpos + k) & 511], 4, 1, f);
#endif
                         fclose(f); }
            }
        }
        platform_present_frame(present);
        platform_frame_sync();
    }
    write_play();
    if (!platform_is_headless()) ps_megacd_brm(false, true);
    genrecomp_shutdown();
    return 0;
}
