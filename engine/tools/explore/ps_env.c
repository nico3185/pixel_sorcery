/*
 * ps_env: the original game on the reference interpreter (GPGX) as a library, for learning
 * agents (ps_gym.py: a Gymnasium environment over it with ctypes). The same machine as
 * explore.c: load a ROM, step a frame with a pad, read work RAM and the screen, save and load
 * states. Built with the tracer (-DGENRECOMP_TRACE=ON) it also counts the 68000 addresses the
 * original runs for the first time (the novelty reward). One game per process.
 *
 *   int  ps_env_init(const char *rom)          1: ready (headless, no sound output)
 *   void ps_env_step(unsigned pad, int render)  one frame, player 1's pad (GEN_BTN_* bits)
 *   void ps_env_ram(unsigned char *out)         64 KB of work RAM, $FF0000 first, byte order as read
 *   int  ps_env_screen(unsigned *out)           320x240 ARGB, returns the visible width
 *   int  ps_env_state_size(void)
 *   int  ps_env_save(unsigned char *out)        state bytes, returns their length
 *   void ps_env_load(const unsigned char *in)
 *   long ps_env_code_seen(void)                 addresses run so far (0 without the tracer build)
 *   void ps_env_code_reset(void)
 */
#include <genrecomp/genrecomp.h>
#include <genrecomp/platform.h>
#include <genrecomp/input.h>
#include <stdlib.h>
#include <string.h>
#include "shared.h"

static int16_t s_audio[4096];
static uint8_t s_seen[0x400000 / 16]; static long s_seen_n;
#ifdef HOOK_CPU
#include "cpuhook.h"
static void novelty_hook(hook_type_t type, int width, unsigned int address, unsigned int value) {
    (void)width; (void)value;
    if (type != HOOK_M68K_E || address >= 0x400000) return;
    unsigned i = address >> 1;
    if (!(s_seen[i >> 3] & (1u << (i & 7)))) { s_seen[i >> 3] |= (uint8_t)(1u << (i & 7)); s_seen_n++; }
}
#endif

int ps_env_init(const char *rom) {
    static char *argv[] = { "ps_env", "--headless", NULL };
    platform_parse_args(2, argv);
    if (!genrecomp_init("ps_env", 3)) return 0;
    char path[4096];
    strncpy(path, rom, sizeof path - 1); path[sizeof path - 1] = 0;
    if (!load_rom(path)) return 0;
    audio_init(44100, 0);
    input.system[0] = input.system[1] = SYSTEM_GAMEPAD;
    system_init();
    system_reset();
#ifdef HOOK_CPU
    set_cpu_hook(novelty_hook);
#endif
    return 1;
}

void ps_env_step(unsigned pad, int render) {
    io_set_pad_state(0, (uint16_t)pad);
    io_set_pad_state(1, 0);
    system_frame_gen(!render);
    audio_update(s_audio);
}

void ps_env_ram(unsigned char *out) {
    for (unsigned a = 0; a < 0x10000; a++) out[a] = work_ram[a ^ 1];   /* GPGX keeps it word-swapped */
}

int ps_env_screen(unsigned *out) {
    int w = bitmap.viewport.w < 320 ? bitmap.viewport.w : 320, h = bitmap.viewport.h < 240 ? bitmap.viewport.h : 240;
    memset(out, 0, 320 * 240 * 4);
    for (int y = 0; y < h; y++)
        memcpy(out + y * 320, bitmap.data + (y + bitmap.viewport.y) * bitmap.pitch + bitmap.viewport.x * 4, (size_t)w * 4);
    return w;
}

int ps_env_state_size(void) { return STATE_SIZE; }
int ps_env_save(unsigned char *out) { return state_save(out); }
void ps_env_load(const unsigned char *in) { state_load((unsigned char *)in); }
long ps_env_code_seen(void) { return s_seen_n; }
void ps_env_code_reset(void) { memset(s_seen, 0, sizeof s_seen); s_seen_n = 0; }
