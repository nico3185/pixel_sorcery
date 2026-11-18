/* ps_launcher -- a game shelf for a family of recompiled titles, in the look of a 16-bit console:
 * ribbed black plastic, a red stripe, cartridges in a carousel. Each cartridge's label is the
 * title's own title screen, captured once from the player's ROM by running the game headless
 * (nothing ROM-derived ships with the launcher).
 *
 *   ps_launcher CONFIG GAMESDIR [--roms DIR] [--fullscreen]
 *
 * CONFIG (written by tools/make_launcher.sh):
 *   name NAME          window title
 *   header TEXT        the big title at the top
 *   accent RRGGBB      the family's colour (selection glow, PRESS START)
 *   game ID|NAME|YEAR|PLAYERS|LABEL FRAME|DEMO FRAME|ROM FILE NAME[|PORT[|SETTINGS]]   one per cartridge
 *   stack ID ID ...    optional: versions of one game in one column (the first on top, e.g. a game,
 *                      then its lock-on versions); the shelf is then a grid - left/right the games, up/down
 *                      the versions, stacked behind the cartridge. Games in no stack: a column each.
 *   (PORT: the games/ title whose executable make_launcher.sh bundles as ID, default ID;
 *   SETTINGS: the folder of the title's settings.ini - its ps_menu title - default PORT)
 * Shelf settings (Tab, or Y / Back on a pad): aspect ratio, widescreen enemies, modern water,
 * full screen, low latency, volume, renderer, controller setup - kept in PREF/<name>.ini and
 * written into each title's settings.ini (and the shared display.ini, input.ini) as it starts;
 * the title's own menu still changes them.
 * The selected cartridge plays a few seconds of the title's attract mode on its label (after
 * the live previews of the Mickey & Donald Collection's launcher, MIT, github.com/Darhkfox/
 * MickeyAndDonaldCollection: here a clip captured once, demo.bmp, as the label).
 * GAMESDIR/ID is the title's native executable. Per title, PREF/ID/ (SDL_GetPrefPath:
 * ~/Library/Application Support/PixelSorcery/ID on macOS, as make_app.sh's apps) keeps the ROM
 * (asked for once), label.bmp, demo.bmp and the play sessions (sessions/DATE.{polls,log,crash}).
 * --roms DIR: look there for ROM FILE NAME first (development). PS_LAUNCHER_HOME=dir replaces
 * PREF (tests). Keys: left/right, up/down (versions), Return/Z/Space play, Tab settings, F or F11 fullscreen, Esc
 * quits; gamepads: d-pad or stick, A/Start play, Y/Back settings; mouse: click a cartridge, wheel. */
#include <SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifndef _WIN32
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <dirent.h>
extern char **environ;
#else
#include <process.h>
#include <direct.h>
#define mkdir(p, m) _mkdir(p)
#endif

#define WMAX 960                                  /* logical screen: 270 high; the shelf 16:9 (480, x4 = 1080p), */
static int s_W = 480;                             /* the disc look as wide as the aspect ratio chosen (360-960) */
#define W s_W
#define H 270
#define CW 150                                    /* cartridge */
#define CH 150
#define LW 128                                    /* label (a 320x224 frame, scaled 0.4) */
#define LH 90
#define MAXG 16
#define DEMO_N 90                                 /* demo clip: 90 frames, one every 4th (15 fps, 6 s) */
#define DEMO_STEP 4

typedef struct {
    char id[64], name[96], year[16], players[16], romname[128], settings[64];
    long frame, demo;
    char rom[1024], store[1024];
    int have_rom;
    volatile int label_state;                     /* 0 none, 1 captured (load it), 2 loaded, 3 failed */
    volatile int demo_state;                      /* 0 none, 1 captured (load it), 2 loaded, 3 failed */
    SDL_Texture *cart, *clip;
    char resume[1200];                            /* PS_RESUME for the next start (continue, a slot) */
    char port[64];                                /* the title run (--game PORT in one executable) */
    int soon;                                     /* 1: a COMING SOON card; 2: another program (app[]) */
    char app[1024];
    char audio[256], audio_path[1100];            /* the disc whose audio tracks it plays (PS_CD_AUDIO) */
    SDL_Texture *labeltex;                        /* the disc look: the label alone */
} Game;

static Game G[MAXG];
static int NG;
/* the grid: columns of versions (stack lines), the version shown in each column */
static int s_ncol, s_nrow[MAXG], s_cell[MAXG][MAXG], s_cur[MAXG], s_colof[MAXG], s_rowof[MAXG];
static float s_vpos[MAXG];                          /* each column's version, animated */
static char s_stack[MAXG][256]; static int s_nstack;
static char s_name[128] = "PixelSorcery", s_header[64] = "COLLECTION", s_gamesdir[1024], s_romdir[1024];
static uint32_t s_accent = 0xE0201C;
static int s_disc;                                /* "look disc": the Mega CD compilation's game-select screen */
static char s_bgfile[1100], s_self[1100];          /* its picture (BMP); "@self" games dir: this executable */
static SDL_Renderer *R;
static SDL_Window *Win;
static SDL_AudioDeviceID s_audio;
static int s_noflip;                              /* tests: leave the frame in the back buffer */

/* ------------------------------------------------------------------ 5x7 font (own design, as ps_menu.c) */
static const char k_chars[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789:./-<>%()&'!,";
static const uint8_t k_font[][7] = {
    {0,0,0,0,0,0,0},
    {14,17,17,31,17,17,17},{30,17,17,30,17,17,30},{14,17,16,16,16,17,14},{30,17,17,17,17,17,30},
    {31,16,16,30,16,16,31},{31,16,16,30,16,16,16},{14,17,16,23,17,17,15},{17,17,17,31,17,17,17},
    {14,4,4,4,4,4,14},{7,2,2,2,2,18,12},{17,18,20,24,20,18,17},{16,16,16,16,16,16,31},
    {17,27,21,21,17,17,17},{17,25,21,19,17,17,17},{14,17,17,17,17,17,14},{30,17,17,30,16,16,16},
    {14,17,17,17,21,18,13},{30,17,17,30,20,18,17},{15,16,16,14,1,1,30},{31,4,4,4,4,4,4},
    {17,17,17,17,17,17,14},{17,17,17,17,17,10,4},{17,17,17,21,21,21,10},{17,17,10,4,10,17,17},
    {17,17,10,4,4,4,4},{31,1,2,4,8,16,31},
    {14,17,19,21,25,17,14},{4,12,4,4,4,4,14},{14,17,1,2,4,8,31},{30,1,1,14,1,1,30},
    {2,6,10,18,31,2,2},{31,16,30,1,1,17,14},{6,8,16,30,17,17,14},{31,1,2,4,8,8,8},
    {14,17,17,14,17,17,14},{14,17,17,15,1,2,12},
    {0,4,4,0,4,4,0},{0,0,0,0,0,12,12},{1,1,2,4,8,16,16},{0,0,0,31,0,0,0},
    {2,4,8,16,8,4,2},{8,4,2,1,2,4,8},{25,26,2,4,8,11,19},{2,4,8,8,8,4,2},{8,4,2,2,2,4,8},
    {12,18,20,8,21,18,13},{4,4,8,0,0,0,0},{4,4,4,4,4,0,4},{0,0,0,0,12,4,8},
};

static int glyph(char c) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    const char *p = c ? strchr(k_chars, c) : NULL;
    return p ? (int)(p - k_chars) : 0;
}
static int text_w(const char *s, int sc) { return (int)strlen(s) * 6 * sc - sc; }

/* ------------------------------------------------------------------ drawing into a pixel buffer */
typedef struct { uint32_t *p; int w, h; } Buf;

static uint32_t rgb(int r, int g, int b) {
    r = r < 0 ? 0 : r > 255 ? 255 : r; g = g < 0 ? 0 : g > 255 ? 255 : g; b = b < 0 ? 0 : b > 255 ? 255 : b;
    return 0xFF000000u | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
}
static uint32_t shade(uint32_t c, float k) {
    return rgb((int)((c >> 16 & 255) * k), (int)((c >> 8 & 255) * k), (int)((c & 255) * k));
}
static void px(Buf *b, int x, int y, uint32_t c) { if (x >= 0 && y >= 0 && x < b->w && y < b->h) b->p[y * b->w + x] = c; }
static void blend(Buf *b, int x, int y, uint32_t c, int a) {      /* a: 0..256 */
    if (x < 0 || y < 0 || x >= b->w || y >= b->h) return;
    uint32_t d = b->p[y * b->w + x];
    int da = d >> 24;
    if (!da) { b->p[y * b->w + x] = (c & 0xFFFFFF) | (uint32_t)(a > 255 ? 255 : a) << 24; return; }
    int r = ((c >> 16 & 255) * a + (d >> 16 & 255) * (256 - a)) >> 8;
    int g = ((c >> 8 & 255) * a + (d >> 8 & 255) * (256 - a)) >> 8;
    int bl = ((c & 255) * a + (d & 255) * (256 - a)) >> 8;
    b->p[y * b->w + x] = (uint32_t)da << 24 | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)bl;
}
static void rect(Buf *b, int x, int y, int w, int h, uint32_t c) {
    for (int j = y; j < y + h; j++) for (int i = x; i < x + w; i++) px(b, i, j, c);
}
static void rect_a(Buf *b, int x, int y, int w, int h, uint32_t c, int a) {
    for (int j = y; j < y + h; j++) for (int i = x; i < x + w; i++) blend(b, i, j, c, a);
}

static int s_italic;                               /* text(): lean the glyphs right (the header) */
/* text at scale sc; colour from `top` to `bot` down the glyph (chrome), a 1-px drop shadow */
static void text(Buf *b, int x, int y, const char *s, int sc, uint32_t top, uint32_t bot, int shadow) {
    for (int pass = shadow ? 0 : 1; pass < 2; pass++)
        for (const char *q = s; *q; q++) {
            int g = glyph(*q), x0 = x + (int)(q - s) * 6 * sc;
            for (int r = 0; r < 7; r++)
                for (int k = 0; k < 5; k++) {
                    if (!(k_font[g][r] & (16 >> k))) continue;
                    for (int j = 0; j < sc; j++) {
                        float t = (r * sc + j) / (float)(7 * sc - 1);
                        uint32_t c = pass ? rgb((int)((top >> 16 & 255) * (1 - t) + (bot >> 16 & 255) * t),
                                                (int)((top >> 8 & 255) * (1 - t) + (bot >> 8 & 255) * t),
                                                (int)((top & 255) * (1 - t) + (bot & 255) * t)) : 0xFF000000u;
                        int lean = s_italic ? (7 * sc - 1 - (r * sc + j)) / 3 : 0;
                        for (int i = 0; i < sc; i++)
                            px(b, x0 + lean + k * sc + i + (pass ? 0 : sc > 1 ? sc / 2 + 1 : 1),
                               y + r * sc + j + (pass ? 0 : sc > 1 ? sc / 2 + 1 : 1), c);
                    }
                }
        }
}
static void text_c(Buf *b, int cx, int y, const char *s, int sc, uint32_t top, uint32_t bot, int shadow) {
    text(b, cx - text_w(s, sc) / 2, y, s, sc, top, bot, shadow);
}

/* ------------------------------------------------------------------ sound: shelf music + blips */
/* Our own tune, synthesized live in a 16-bit FM style (2-operator FM bass and lead, a soft pad,
 * noise hats and a kick), 8 bars looping; the blips mix on top. 44.1 kHz mono. */
#define SR 44100
static int s_music_on = 1, s_music_vol = 100;      /* shelf settings */
static volatile int s_music_paused;
static int16_t *s_sfx; static int s_sfx_len, s_sfx_pos;
static double note_hz(int m) { return 440.0 * pow(2.0, (m - 69) / 12.0); }
/* 8 bars of 16 steps: chord roots Am F C G Am F G E; melody (MIDI note, 0 rest), bass by chord */
static const int k_roots[8] = { 57, 53, 48, 55, 57, 53, 55, 52 };
static const int k_third[8] = { 3, 4, 4, 4, 3, 4, 4, 4 };
static const unsigned char k_melody[128] = {
    76,0,0,74, 72,0,74,0, 76,0,79,0, 76,0,74,0,   72,0,0,0, 69,0,72,0, 74,0,72,0, 69,0,0,0,
    67,0,0,72, 76,0,79,0, 81,0,79,0, 76,0,72,0,   74,0,0,0, 71,0,74,0, 79,0,0,0, 0,0,0,0,
    76,0,0,74, 72,0,74,0, 76,0,79,0, 81,0,83,0,   84,0,0,0, 81,0,77,0, 76,0,74,0, 72,0,0,0,
    74,0,0,76, 79,0,76,0, 74,0,71,0, 67,0,71,0,   68,0,71,0, 76,0,0,0, 75,0,0,0, 76,0,0,0 };
static void SDLCALL audio_cb(void *u, Uint8 *stream, int len) {
    (void)u;
    int16_t *out = (int16_t *)stream; int n = len / 2;
    static double t, ph_l, ph_lm, ph_b, ph_bm, ph_p[3], kick_ph; static unsigned noise = 1;
    const double step = 60.0 / 128.0 / 4.0;          /* 128 bpm, 16th notes */
    for (int i = 0; i < n; i++) {
        double v = 0;
        if (s_music_on && !s_music_paused) {
            int st = (int)(t / step) % 128, bar = st / 16; double in = fmod(t, step);
            /* lead: the last note held until the next one */
            int k = st; while (k > 0 && !k_melody[k]) k--;
            double since = (st - k) * step + in, fl = note_hz(k_melody[k]);
            if (k_melody[k]) {
                double env = exp(-since * 3.2) * (since < 0.005 ? since / 0.005 : 1);
                ph_lm += 2 * M_PI * fl * 2 / SR; ph_l += 2 * M_PI * fl / SR;
                v += 0.16 * env * sin(ph_l + (1.2 + 1.5 * exp(-since * 8)) * sin(ph_lm));
            }
            /* bass: root and octave in eighths */
            double fb = note_hz(k_roots[bar] - 12 + ((st / 2) % 4 == 2 ? 12 : 0));
            double bi = fmod(t, step * 2), benv = exp(-bi * 6);
            ph_bm += 2 * M_PI * fb / SR; ph_b += 2 * M_PI * fb / SR;
            v += 0.20 * benv * sin(ph_b + 1.8 * benv * sin(ph_bm));
            /* pad: the chord, soft and detuned */
            int ch[3] = { k_roots[bar] + 12, k_roots[bar] + 12 + k_third[bar], k_roots[bar] + 19 };
            for (int c = 0; c < 3; c++) { ph_p[c] += 2 * M_PI * note_hz(ch[c]) * (1 + 0.002 * c) / SR; v += 0.035 * sin(ph_p[c]); }
            /* drums: kick on beats, hats on off-beats */
            double bt = fmod(t, step * 4);
            if (bt < 0.15) { kick_ph += 2 * M_PI * (50 + 120 * exp(-bt * 30)) / SR; v += 0.35 * exp(-bt * 18) * sin(kick_ph); } else kick_ph = 0;
            double ht = fmod(t + step * 2, step * 4);
            noise = noise * 1103515245u + 12345u;
            if (ht < 0.04) v += 0.05 * exp(-ht * 120) * (((noise >> 16) & 1) ? 1 : -1);
            v *= s_music_vol / 100.0 * 0.8;
            t += 1.0 / SR;
            if ((i & 4095) == 0) {                     /* keep the phases small (precision) */
                ph_l = fmod(ph_l, 2 * M_PI); ph_lm = fmod(ph_lm, 2 * M_PI); ph_b = fmod(ph_b, 2 * M_PI); ph_bm = fmod(ph_bm, 2 * M_PI);
                for (int c = 0; c < 3; c++) ph_p[c] = fmod(ph_p[c], 2 * M_PI);
            }
        }
        if (s_sfx && s_sfx_pos < s_sfx_len) v += s_sfx[s_sfx_pos++] / 32768.0;
        if (v > 1) v = 1; if (v < -1) v = -1;
        out[i] = (int16_t)(v * 30000);
    }
}
static void blip(const int *notes, int n, int ms) {          /* notes in Hz, each ms long */
    if (!s_audio) return;
    int len = SR * ms / 1000;
    int16_t *buf = malloc(sizeof(int16_t) * (size_t)len * (size_t)n);
    if (!buf) return;
    for (int k = 0; k < n; k++)
        for (int i = 0; i < len; i++) {
            double t = i / (double)SR, env = exp(-t * 18.0);
            double ph = fmod(t * notes[k], 1.0);
            double v = (ph < 0.5 ? 1 : -1) * 0.6 + (fmod(t * notes[k] * 2, 1.0) < 0.25 ? 0.25 : -0.25);  /* two pulses: a PSG tone */
            buf[k * len + i] = (int16_t)(v * env * 2600 * s_music_vol / 100);
        }
    SDL_LockAudioDevice(s_audio);
    free(s_sfx); s_sfx = buf; s_sfx_len = len * n; s_sfx_pos = 0;
    SDL_UnlockAudioDevice(s_audio);
}
static void snd_move(void) { int n[] = { 1320, 1760 }; blip(n, 2, 30); }
static void snd_start(void) { int n[] = { 523, 659, 784, 1047, 1319 }; blip(n, 5, 55); }
static void snd_nope(void) { int n[] = { 220, 165 }; blip(n, 2, 70); }

/* ------------------------------------------------------------------ files */
static int exists(const char *p) { struct stat st; return stat(p, &st) == 0; }
static void mkdirs(const char *path) {
    char t[1024]; if (snprintf(t, sizeof t, "%s", path) >= (int)sizeof t) return;
    for (char *q = t + 1; *q; q++) if (*q == '/') { *q = 0; mkdir(t, 0755); *q = '/'; }
    mkdir(t, 0755);
}
static int copy_file(const char *from, const char *to) {
    FILE *a = fopen(from, "rb"), *b = a ? fopen(to, "wb") : NULL;
    if (!a || !b) { if (a) fclose(a); return 0; }
    char buf[65536]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, a)) > 0) fwrite(buf, 1, n, b);
    fclose(a); fclose(b);
    return 1;
}
static void trim(char *s) {
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\n' || e[-1] == '\r' || e[-1] == '\t')) *--e = 0;
    char *b = s; while (*b == ' ' || *b == '\t') b++;
    memmove(s, b, strlen(b) + 1);
}

static int load_config(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        trim(line);
        if (!*line || *line == '#') continue;
        if (!strncmp(line, "name ", 5)) { snprintf(s_name, sizeof s_name, "%.*s", (int)sizeof s_name - 1, line + 5);   /* a display title: cut to fit */ trim(s_name); }
        else if (!strncmp(line, "header ", 7)) { snprintf(s_header, sizeof s_header, "%.*s", (int)sizeof s_header - 1, line + 7); trim(s_header); }
        else if (!strncmp(line, "accent ", 7)) s_accent = (uint32_t)strtoul(line + 7, NULL, 16);
        else if (!strcmp(line, "look disc")) s_disc = 1;
        else if (!strncmp(line, "background ", 11)) {          /* beside the config unless absolute */
            const char *b = line + 11, *sl = strrchr(path, '/');
            if (*b == '/' || !sl) snprintf(s_bgfile, sizeof s_bgfile, "%s", b);
            else snprintf(s_bgfile, sizeof s_bgfile, "%.*s/%s", (int)(sl - path), path, b);
        }
        else if ((!strncmp(line, "soon ", 5) || !strncmp(line, "app ", 4)) && NG < MAXG) {
            /* soon ID|NAME|YEAR: shown, not startable; app ID|NAME|YEAR|PATH;PATH...: another program,
             * started when one of the paths exists (~ = home), else COMING SOON */
            Game *g = &G[NG]; char *f[4] = { 0 }; int n = 0, app = *line == 'a';
            for (char *tok = strtok(line + (app ? 4 : 5), "|"); tok && n < 4; tok = strtok(NULL, "|")) { trim(tok); f[n++] = tok; }
            if (n < 3) continue;
            memset(g, 0, sizeof *g);
            snprintf(g->id, sizeof g->id, "%s", f[0]); snprintf(g->name, sizeof g->name, "%s", f[1]);
            snprintf(g->year, sizeof g->year, "%s", f[2]); snprintf(g->players, sizeof g->players, "1");
            snprintf(g->port, sizeof g->port, "%s", f[0]); snprintf(g->settings, sizeof g->settings, "%s", f[0]);
            g->soon = 1;
            for (char *c = app && n > 3 ? strtok(f[3], ";") : NULL; c; c = strtok(NULL, ";")) {
                char p2[1024]; trim(c);
                if (*c == '~' && getenv("HOME")) snprintf(p2, sizeof p2, "%s%s", getenv("HOME"), c + 1); else snprintf(p2, sizeof p2, "%s", c);
                if (exists(p2)) { snprintf(g->app, sizeof g->app, "%s", p2); g->soon = 2; g->have_rom = 1; break; }
            }
            NG++;
        }
        else if (!strncmp(line, "stack ", 6) && s_nstack < MAXG) {   /* an overlong line is skipped */
            if (snprintf(s_stack[s_nstack], sizeof s_stack[0], "%s", line + 6) < (int)sizeof s_stack[0]) s_nstack++;
        }
        else if (!strncmp(line, "game ", 5) && NG < MAXG) {
            Game *g = &G[NG]; char *f[10] = { 0 }; int n = 0;
            for (char *tok = strtok(line + 5, "|"); tok && n < 10; tok = strtok(NULL, "|")) { trim(tok); f[n++] = tok; }
            if (n < 7) continue;
            snprintf(g->settings, sizeof g->settings, "%s", n >= 9 && *f[8] ? f[8] : n >= 8 && *f[7] ? f[7] : f[0]);
            snprintf(g->id, sizeof g->id, "%s", f[0]); snprintf(g->name, sizeof g->name, "%s", f[1]);
            snprintf(g->year, sizeof g->year, "%s", f[2]); snprintf(g->players, sizeof g->players, "%s", f[3]);
            g->frame = atol(f[4]); g->demo = atol(f[5]);
            snprintf(g->romname, sizeof g->romname, "%s", f[6]);
            snprintf(g->port, sizeof g->port, "%s", n >= 8 && *f[7] ? f[7] : f[0]);
            snprintf(g->audio, sizeof g->audio, "%s", n >= 10 ? f[9] : "");   /* AUDIO: another disc's soundtrack */
            NG++;
        }
    }
    fclose(f);
    for (int i = 0; i < NG; i++) s_colof[i] = -1;
    for (int i = 0; i < NG; i++) {                  /* columns in the order of their first game */
        if (s_colof[i] >= 0) continue;
        int c = s_ncol++;
        s_cell[c][0] = i; s_nrow[c] = 1; s_colof[i] = c; s_rowof[i] = 0;
        for (int k = 0; k < s_nstack; k++) {
            char buf[sizeof s_stack[0]]; memcpy(buf, s_stack[k], sizeof buf);
            int ids[MAXG], n = 0, mine = 0;
            for (char *tok = strtok(buf, " \t"); tok && n < MAXG; tok = strtok(NULL, " \t"))
                for (int j = 0; j < NG; j++) if (!strcmp(G[j].id, tok)) { ids[n++] = j; mine |= j == i; }
            if (!mine) continue;
            s_nrow[c] = 0;
            for (int r = 0; r < n; r++)
                if (s_colof[ids[r]] < 0 || ids[r] == i) { s_cell[c][s_nrow[c]] = ids[r]; s_colof[ids[r]] = c; s_rowof[ids[r]] = s_nrow[c]++; }
            break;
        }
    }
    return NG > 0;
}

static void find_rom(Game *g) {
    if (g->soon) return;
    *g->audio_path = 0;
    if (*g->audio) {                                /* the soundtrack's disc: kept beside, or in --roms */
        char p[1100];
        if (snprintf(p, sizeof p, "%s/%s", g->store, g->audio) < (int)sizeof p && exists(p)) snprintf(g->audio_path, sizeof g->audio_path, "%s", p);
        else if (*s_romdir && snprintf(p, sizeof p, "%s/%s", s_romdir, g->audio) < (int)sizeof p && exists(p)) snprintf(g->audio_path, sizeof g->audio_path, "%s", p);
    }
    snprintf(g->rom, sizeof g->rom, "%s/%s", g->store, g->romname);
    g->have_rom = exists(g->rom);
    if (!g->have_rom && *s_romdir) {
        char p[1024];
        if (snprintf(p, sizeof p, "%s/%s", s_romdir, g->romname) < (int)sizeof p && exists(p)) { snprintf(g->rom, sizeof g->rom, "%s", p); g->have_rom = 1; }
    }
}

/* ------------------------------------------------------------------ shelf settings */
static const char *k_aspects[] = { "4:3", "16:10", "16:9", "21:9", "32:9" };    /* as ps_menu.c's */
static struct { int aspect, objects, water, fullscreen, latency, volume, metal, music, style; } SET = { 2, 1, 1, 0, 1, 100, 0, 1, 0 };
static const char *k_styles[] = { "PIXEL", "MODERN" };   /* the disc look's home screen: the Mega CD's, or a modern landing */
static char s_pref[1024], s_setpath[1100];
static void settings_load(void) {
    FILE *f = fopen(s_setpath, "r"); if (!f) return;
    char l[128];
    while (fgets(l, sizeof l, f)) {
        int v;
        if (sscanf(l, "aspect=%d", &v) == 1 && v >= 0 && v < 5) SET.aspect = v;
        else if (sscanf(l, "wide_objects=%d", &v) == 1) SET.objects = !!v;
        else if (sscanf(l, "water=%d", &v) == 1) SET.water = !!v;
        else if (sscanf(l, "fullscreen=%d", &v) == 1) SET.fullscreen = !!v;
        else if (sscanf(l, "low_latency=%d", &v) == 1) SET.latency = !!v;
        else if (sscanf(l, "volume=%d", &v) == 1 && v >= 0 && v <= 100) SET.volume = v;
        else if (sscanf(l, "metal=%d", &v) == 1) SET.metal = !!v;
        else if (sscanf(l, "music=%d", &v) == 1) SET.music = !!v;
        else if (sscanf(l, "style=%d", &v) == 1 && v >= 0 && v < 2) SET.style = v;
    }
    fclose(f);
}
static void settings_save(void) {
    FILE *f = fopen(s_setpath, "w"); if (!f) return;
    fprintf(f, "aspect=%d\nwide_objects=%d\nwater=%d\nfullscreen=%d\nlow_latency=%d\nvolume=%d\nmetal=%d\nmusic=%d\nstyle=%d\n",
            SET.aspect, SET.objects, SET.water, SET.fullscreen, SET.latency, SET.volume, SET.metal, SET.music, SET.style);
    s_music_on = SET.music; s_music_vol = SET.volume;
    fclose(f);
}
/* the shelf's choices into the title's own settings.ini (its other keys kept) */
static void settings_apply(Game *g) {
    char dir[1100], path[1200], tmp[1300]; snprintf(dir, sizeof dir, "%s%s", s_pref, g->settings); mkdirs(dir);
    snprintf(path, sizeof path, "%s/settings.ini", dir); snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *in = fopen(path, "r"), *out = fopen(tmp, "w");
    if (!out) { if (in) fclose(in); return; }
    char l[256];
    while (in && fgets(l, sizeof l, in))
        if (strncmp(l, "aspect=", 7) && strncmp(l, "wide_objects=", 13) && strncmp(l, "water=", 6) && strncmp(l, "fullscreen=", 11)
            && strncmp(l, "low_latency=", 12) && strncmp(l, "volume=", 7)) fputs(l, out);
    fprintf(out, "aspect=%d\nwide_objects=%d\nwater=%d\nfullscreen=%d\nlow_latency=%d\nvolume=%d\n",
            SET.aspect, SET.objects, SET.water, SET.fullscreen, SET.latency, SET.volume);
    if (in) fclose(in);
    fclose(out); rename(tmp, path);
    char disp[1100]; snprintf(disp, sizeof disp, "%sdisplay.ini", s_pref);      /* the renderer, shared */
    FILE *d = fopen(disp, "w"); if (d) { fprintf(d, "renderer=%s\n", SET.metal ? "metal" : "opengl"); fclose(d); }
}

/* Controller setup (as the titles' CONTROLLER SETUP): the raw control for each of A B C START X Y Z
 * MODE - a button (0..99) or an axis pushed past half way (100 + axis) - per controller GUID in
 * PREF/input.ini, which every title reads */
static const char *k_pad_names[8] = { "A", "B", "C", "START", "X", "Y", "Z", "MODE" };
static int s_cap = -1, s_cap_map[8];                  /* capture step (-1: off), the controls so far */
static SDL_GameController *s_pad;
static void pad_save(void) {
    if (!s_pad) return;
    char guid[64]; SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(SDL_GameControllerGetJoystick(s_pad)), guid, sizeof guid);
    char path[1100]; snprintf(path, sizeof path, "%sinput.ini", s_pref);
    char keep[64][256]; int nk = 0;
    FILE *f = fopen(path, "r");
    if (f) { char line[256], g[64]; while (nk < 64 && fgets(line, sizeof line, f)) if (sscanf(line, "%63s", g) == 1 && strcmp(g, guid)) snprintf(keep[nk++], 256, "%s", line); fclose(f); }
    if (!(f = fopen(path, "w"))) return;
    for (int i = 0; i < nk; i++) fputs(keep[i], f);
    int *m = s_cap_map;
    fprintf(f, "%s %d %d %d %d %d %d %d %d  # %s: A B C START X Y Z MODE\n", guid, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], SDL_GameControllerName(s_pad));
    fclose(f);
}

/* ------------------------------------------------------------------ running a title */
/* runs GAMESDIR/ID with argv/env additions; waits; stderr to `log` when given */
static int run_game(Game *g, const char **args, const char **env, const char *log) {
    char exe[1200]; snprintf(exe, sizeof exe, "%s/%s", s_gamesdir, g->id);
    const char *argv[20]; int n = 0;
    if (g->soon == 2) {                               /* another program (SHM): the player's own copy */
#ifdef __APPLE__
        snprintf(exe, sizeof exe, "/usr/bin/open");
        argv[n++] = exe; argv[n++] = "-W"; argv[n++] = "-a"; argv[n++] = g->app; argv[n] = NULL;
#else
        snprintf(exe, sizeof exe, "%s", g->app);
        argv[n++] = exe; argv[n] = NULL;
#endif
        args = NULL;
    } else if (!strcmp(s_gamesdir, "@self")) {        /* one executable: PROGRAM --game PORT [options] ROM */
        snprintf(exe, sizeof exe, "%s", s_self);
        argv[n++] = exe; argv[n++] = "--game"; argv[n++] = g->port;
    } else argv[n++] = exe;
    if (g->soon != 2) {
        for (; args && *args && n < 17; args++) argv[n++] = *args;
        argv[n++] = g->rom; argv[n] = NULL;
    }
#ifndef _WIN32
    int ne = 0; while (environ[ne]) ne++;
    const char **envp = calloc((size_t)ne + 16, sizeof *envp);
    int k = 0;
    for (const char **e = env; e && *e; e++) envp[k++] = *e;
    for (int i = 0; i < ne; i++) {                    /* ours first; drop the inherited copies */
        int dup = 0;
        for (const char **e = env; e && *e && !dup; e++) {
            size_t l = strcspn(*e, "=");
            dup = !strncmp(environ[i], *e, l) && environ[i][l] == '=';
        }
        if (!dup) envp[k++] = environ[i];
    }
    posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, log ? log : "/dev/null", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    pid_t pid; int st = -1;
    if (posix_spawn(&pid, exe, &fa, NULL, (char *const *)argv, (char *const *)envp) == 0) waitpid(pid, &st, 0);
    posix_spawn_file_actions_destroy(&fa);
    free(envp);
    return st == 0;
#else
    for (const char **e = env; e && *e; e++) _putenv(*e);
    (void)log;
    return _spawnv(_P_WAIT, exe, argv) == 0;
#endif
}

/* frames `from`, from + every, ... `to` of a headless 4:3 run, as BMPs in `dir` (f000000.bmp ...) */
static void capture(Game *g, const char *dir, long from, long to, long every) {
    char fr[32], t[32], ev[32], frames[32], d[1100];
    snprintf(fr, sizeof fr, "PS_FRAMES_FROM=%ld", from); snprintf(t, sizeof t, "PS_FRAMES_TO=%ld", to);
    snprintf(ev, sizeof ev, "PS_FRAMES_EVERY=%ld", every); snprintf(frames, sizeof frames, "%ld", to + 2);
    snprintf(d, sizeof d, "PS_FRAMES_DIR=%s", dir);
    const char *env[] = { d, fr, t, ev, "PS_FRAMES_BMP=1", "PS_WIDE=4:3", "PS_RENDERER=native", "PS_Z80=recomp",
                          "PS_ART_SCOUT=1", NULL };
    const char *args[] = { "--headless", "--frames", frames, NULL };
    run_game(g, args, env, NULL);
}

/* the label: the title screen at frame g->frame */
static int capture_label(Game *g) {
    char label[1100], dump[1100];
    capture(g, g->store, g->frame, g->frame, 1);
    snprintf(dump, sizeof dump, "%s/f000000.bmp", g->store);
    snprintf(label, sizeof label, "%s/label.bmp", g->store);
    return exists(dump) && rename(dump, label) == 0;
}

static void scale_into(uint32_t *dst, int dw, int dh, int dpitch, SDL_Surface *s);
/* the demo clip: DEMO_N frames of the attract mode from g->demo, scaled to the label, stacked */
static int capture_demo(Game *g) {
    char dir[1100], path[1200], out[1100];
    snprintf(dir, sizeof dir, "%s/demo-frames", g->store); mkdirs(dir);
    capture(g, dir, g->demo, g->demo + (long)(DEMO_N - 1) * DEMO_STEP, DEMO_STEP);
    uint32_t *strip = calloc((size_t)LW * LH * DEMO_N, 4);
    int n = 0;
    for (int i = 0; i < DEMO_N; i++) {
        snprintf(path, sizeof path, "%s/f%06d.bmp", dir, i * DEMO_STEP);
        SDL_Surface *s = SDL_LoadBMP(path);
        if (s) { scale_into(strip + (size_t)i * LW * LH, LW, LH, LW, s); SDL_FreeSurface(s); n++; }
        remove(path);
    }
    remove(dir);
    int ok = 0;
    if (n == DEMO_N) {
        SDL_Surface *o = SDL_CreateRGBSurfaceWithFormatFrom(strip, LW, LH * DEMO_N, 32, LW * 4, SDL_PIXELFORMAT_ARGB8888);
        snprintf(out, sizeof out, "%s/demo.bmp", g->store);
        ok = o && SDL_SaveBMP(o, out) == 0;
        if (o) SDL_FreeSurface(o);
    }
    free(strip);
    return ok;
}
static int SDLCALL label_thread(void *u) {
    (void)u;
    for (int i = 0; i < NG; i++) {
        Game *g = &G[i];
        if (g->label_state != 0 || !g->have_rom || g->soon) continue;
        char label[1100]; snprintf(label, sizeof label, "%s/label.bmp", g->store);
        g->label_state = exists(label) || capture_label(g) ? 1 : 3;
    }
    for (int i = 0; i < NG; i++) {                    /* then the demo clips */
        Game *g = &G[i];
        if (g->demo_state != 0 || !g->have_rom || g->demo <= 0 || g->soon) continue;
        char demo[1100]; snprintf(demo, sizeof demo, "%s/demo.bmp", g->store);
        g->demo_state = exists(demo) || capture_demo(g) ? 1 : 3;
    }
    return 0;
}
static SDL_Thread *s_labels;
static void start_labels(void) {
    if (s_labels) { SDL_WaitThread(s_labels, NULL); s_labels = NULL; }
    s_labels = SDL_CreateThread(label_thread, "labels", NULL);
}

static volatile int s_playing;
static Game *s_play_game;
static int s_fullscreen;
static int SDLCALL play_thread(void *u) {
    Game *g = u;
    char sess[1100], log[1200], rec[1200], crash[1200], stamp[32];
    time_t t = time(NULL); strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", localtime(&t));
    snprintf(sess, sizeof sess, "%s/sessions", g->store); mkdirs(sess);
    snprintf(log, sizeof log, "%s/%s.log", sess, stamp);
    snprintf(rec, sizeof rec, "PS_POLL_RECORD=%s/%s.polls", sess, stamp);
    snprintf(crash, sizeof crash, "PS_CRASH_REPORT=%s/%s.crash", sess, stamp);
    char crash_path[1200]; snprintf(crash_path, sizeof crash_path, "%s/%s.crash", sess, stamp);
    FILE *crash_file = fopen(crash_path, "w"); if (crash_file) fclose(crash_file);
    char res[1300]; snprintf(res, sizeof res, "PS_RESUME=%s", g->resume);
    char aud[1200]; snprintf(aud, sizeof aud, "PS_CD_AUDIO=%s", g->audio_path);
    const char *env[] = { rec, crash, "PS_RENDERER=native", "PS_Z80=recomp", "PS_LAUNCHER=1", "PS_HOME=1",
                          *g->audio_path ? aud : "PS_CD_AUDIO=", *g->resume ? res : NULL, NULL };
    const char *full[] = { "--fullscreen", NULL };
    settings_apply(g);
    run_game(g, SET.fullscreen ? full : NULL, env, log);
    s_playing = 0;
    SDL_RaiseWindow(Win);                                /* the game gone: the shelf in front, keys to it */
    return 0;
}

#ifdef __APPLE__
static int ask_rom(Game *g) {                       /* the system file picker, as make_app.sh's apps */
    char cmd[512];
    int disc = strlen(g->romname) > 4 && !strcmp(g->romname + strlen(g->romname) - 4, ".cue");
    snprintf(cmd, sizeof cmd, "osascript -e 'POSIX path of (choose file with prompt \"Choose your %s %s\")' 2>/dev/null",
             g->name, disc ? "disc (.cue)" : "ROM (.md / .bin / .gen)");
    FILE *p = popen(cmd, "r");
    if (!p) return 0;
    char path[1024] = ""; if (!fgets(path, sizeof path, p)) *path = 0;
    pclose(p); trim(path);
    if (!*path) return 0;
    mkdirs(g->store);
    char dst[1100]; snprintf(dst, sizeof dst, "%s/%s", g->store, g->romname);
    if (disc) {   /* a disc is its .cue and the track files beside it: a folder of links to the
                     player's files, the .cue also under the name ROM FILE NAME expects */
        char *s = strrchr(path, '/'); if (!s) return 0; *s = 0;
        char dir[1100]; snprintf(dir, sizeof dir, "%s", dst); *strrchr(dir, '/') = 0;
        mkdirs(dir);
        DIR *dp = opendir(path); if (!dp) return 0;
        for (struct dirent *e; (e = readdir(dp)); ) {
            if (*e->d_name == '.') continue;
            char from[1100], to[1100];
            snprintf(from, sizeof from, "%s/%s", path, e->d_name); snprintf(to, sizeof to, "%s/%s", dir, e->d_name);
            unlink(to); symlink(from, to);
        }
        closedir(dp);
        char cue[1100]; snprintf(cue, sizeof cue, "%s/%s", path, s + 1);
        if (!exists(dst)) symlink(cue, dst);
    } else if (!copy_file(path, dst)) return 0;
    find_rom(g);
    return g->have_rom;
}
#else
static int ask_rom(Game *g) { (void)g; return 0; }
#endif

/* ------------------------------------------------------------------ continue / save slots */
/* START on a cartridge with a last session or saves opens a chooser: NEW GAME, CONTINUE (the
 * last session, replayed to its end), SLOT 1-4 (the title's save states, PREF/<settings>/saves) */
typedef struct { char label[128]; char resume[1200]; int game; } Choice;
static Choice s_ch[8]; static int s_nch, s_choose = -1;
static int s_vchoose;                               /* the disc look: the chooser lists a card's versions */
static long count_lines(const char *p) {
    FILE *f = fopen(p, "r"); if (!f) return -1;
    long n = 0; int c; while ((c = fgetc(f)) != EOF) n += c == '\n';
    fclose(f); return n;
}
static int build_choices(Game *g) {
    s_nch = 0;
    snprintf(s_ch[s_nch].label, sizeof s_ch[0].label, "NEW GAME"); *s_ch[s_nch++].resume = 0;
    char dir[1100]; snprintf(dir, sizeof dir, "%s/sessions", g->store);
    char newest[1200] = ""; time_t nt = 0;
    {   /* the newest session long enough to matter (over 10 seconds) */
        char cmd[1300]; snprintf(cmd, sizeof cmd, "ls -t \"%s\"/*.polls 2>/dev/null", dir);
        FILE *p = popen(cmd, "r");
        if (p) { char l[1200]; while (fgets(l, sizeof l, p)) { trim(l); if (count_lines(l) > 600) { snprintf(newest, sizeof newest, "%s", l); break; } } pclose(p); }
        (void)nt;
    }
    if (*newest) {
        long n = count_lines(newest); long sec = (long)(n / 59.92);
        snprintf(s_ch[s_nch].label, sizeof s_ch[0].label, "CONTINUE  %ld:%02ld", sec / 60, sec % 60);
        snprintf(s_ch[s_nch++].resume, sizeof s_ch[0].resume, "%s:%ld", newest, n);
    }
    for (int k = 1; k <= 4 && s_nch < 8; k++) {
        char sp[1200]; snprintf(sp, sizeof sp, "%s%s/saves/slot%d.polls", s_pref, g->settings, k);
        long n = count_lines(sp);
        if (n <= 0) continue;
        long sec = (long)(n / 59.92);
        snprintf(s_ch[s_nch].label, sizeof s_ch[0].label, "LOAD SLOT %d  %ld:%02ld", k, sec / 60, sec % 60);
        if (snprintf(s_ch[s_nch].resume, sizeof s_ch[0].resume, "%s:%ld", sp, n) < (int)sizeof s_ch[0].resume) s_nch++;
    }
    return s_nch;
}
static void draw_choices(Buf *b, Game *g) {
    int pw = 220, ph = 26 + s_nch * 15, px0 = (W - pw) / 2, py0 = 70;
    rect_a(b, 0, 43, W, H - 61, 0x000000, 150);
    rect(b, px0, py0, pw, ph, rgb(18, 18, 22));
    rect(b, px0, py0, pw, 12, s_disc ? rgb(255, 224, 0) : rgb(200, 24, 22));     /* the disc look: its yellow */
    s_italic = 1; text(b, px0 + 6, py0 + 3, g->name, 1, s_disc ? 0x101010 : 0xFFFFFF, s_disc ? 0x101010 : 0xFFD8D8, 0); s_italic = 0;
    for (int i = 0; i < s_nch; i++) {
        int y = py0 + 18 + i * 15;
        if (i == s_choose) { rect(b, px0 + 4, y - 4, pw - 8, 15, rgb(40, 40, 52)); text(b, px0 + 10, y, ">", 1, s_accent, s_accent, 0); }
        text(b, px0 + 22, y, s_ch[i].label, 1, i == s_choose ? 0xFFFFFF : 0xA0A4B0, i == s_choose ? 0xFFFFFF : 0xA0A4B0, 0);
    }
}

/* ------------------------------------------------------------------ the cartridge */
/* a frame scaled into dw x dh pixels (area average; a wider dump: its 4:3 middle) */
static void scale_into(uint32_t *dst, int dw, int dh, int dpitch, SDL_Surface *s) {
    SDL_Surface *t = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_ARGB8888, 0);
    if (!t) return;
    const uint32_t *sp = t->pixels; int pitch = t->pitch / 4;
    int sw = t->w > 320 ? 320 : t->w, sx0 = (t->w - sw) / 2;
    for (int y = 0; y < dh; y++)
        for (int x = 0; x < dw; x++) {
            int ax = sx0 + x * sw / dw, bx = sx0 + (x + 1) * sw / dw, ay = y * t->h / dh, by = (y + 1) * t->h / dh;
            int r = 0, gg = 0, b = 0, n = 0;
            for (int j = ay; j < by; j++) for (int i = ax; i < bx; i++) {
                uint32_t p = sp[j * pitch + i]; r += p >> 16 & 255; gg += p >> 8 & 255; b += p & 255; n++;
            }
            if (n) dst[y * dpitch + x] = rgb(r / n, gg / n, b / n);
        }
    SDL_FreeSurface(t);
}
static void label_from_bmp(Buf *c, int x0, int y0, SDL_Surface *s) { scale_into(c->p + y0 * c->w + x0, LW, LH, c->w, s); }

static void load_clip(Game *g) {
    char demo[1100]; snprintf(demo, sizeof demo, "%s/demo.bmp", g->store);
    SDL_Surface *s = SDL_LoadBMP(demo);
    if (s && s->w == LW && s->h == LH * DEMO_N) {
        g->clip = SDL_CreateTextureFromSurface(R, s);
        SDL_SetTextureBlendMode(g->clip, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(g->clip, SDL_ScaleModeLinear);
    }
    if (s) SDL_FreeSurface(s);
    g->demo_state = g->clip ? 2 : 3;
}

static void wrap_text(Buf *c, int cx, int y, const char *s, uint32_t col) {   /* centred, LW wide */
    char w[128]; snprintf(w, sizeof w, "%s", s);
    char line[32] = ""; int row = 0;
    for (char *tok = strtok(w, " "); tok; tok = strtok(NULL, " ")) {
        if (*line && (int)(strlen(line) + 1 + strlen(tok)) * 6 > LW - 12) {
            text_c(c, cx, y + row++ * 10, line, 1, col, col, 1); *line = 0;
        }
        if (*line) strcat(line, " ");
        strncat(line, tok, sizeof line - strlen(line) - 1);
    }
    if (*line) text_c(c, cx, y + row * 10, line, 1, col, col, 1);
}

static uint32_t pix[CW * CH];                       /* the last cartridge drawn */
static void build_cart(Game *g) {
    Buf c = { pix, CW, CH };
    memset(pix, 0, sizeof pix);
    const uint32_t body = rgb(22, 22, 26), hi = rgb(62, 62, 72), lo = rgb(6, 6, 8);
    for (int y = 0; y < CH; y++)                       /* body, chamfered top corners */
        for (int x = 0; x < CW; x++) {
            int ch = 12 - y; if (ch > 0 && (x < ch || x >= CW - ch)) continue;
            if (y >= CH - 3 && (x < 3 - (CH - 1 - y) || x >= CW - 3 + (CH - 1 - y))) continue;
            float k = 1.0f + 0.10f * (1.0f - y / (float)CH);                     /* lit from above */
            px(&c, x, y, shade(body, k));
        }
    for (int y = 0; y < CH; y++)                       /* bevel: light top-left, dark bottom-right */
        for (int x = 0; x < CW; x++) {
            if (!(pix[y * CW + x] >> 24)) continue;
            int l = x == 0 || !(pix[y * CW + x - 1] >> 24), t = y == 0 || !(pix[(y - 1) * CW + x] >> 24);
            int r = x == CW - 1 || !(pix[y * CW + x + 1] >> 24), b = y == CH - 1 || !(pix[(y + 1) * CW + x] >> 24);
            if (l || t) pix[y * CW + x] = hi; else if (r || b) pix[y * CW + x] = lo;
        }
    /* label: a red band over the picture, as the shelf's stripe */
    int lx = (CW - LW) / 2, ly = 22;
    rect(&c, lx - 3, ly - 14, LW + 6, LH + 17, rgb(236, 236, 232));                 /* label paper */
    rect(&c, lx - 3, ly - 14, LW + 6, 11, rgb(200, 24, 22));
    text(&c, lx, ly - 12, "16-BIT", 1, 0xFFFFFF, 0xFFD8D8, 0);
    text(&c, lx + LW - text_w(g->year, 1), ly - 12, g->year, 1, 0xFFFFFF, 0xFFD8D8, 0);
    rect(&c, lx - 1, ly - 1, LW + 2, LH + 2, rgb(20, 20, 24));
    char label[1100]; snprintf(label, sizeof label, "%s/label.bmp", g->store);
    SDL_Surface *s = g->label_state == 1 || g->label_state == 2 ? SDL_LoadBMP(label) : NULL;
    if (s) { label_from_bmp(&c, lx, ly, s); SDL_FreeSurface(s); g->label_state = 2; }
    else {                                             /* no label yet: the name on a night sky */
        for (int y = 0; y < LH; y++) rect(&c, lx, ly + y, LW, 1, rgb(10 + y / 3, 18 + y / 3, 60 + y));
        for (int i = 0; i < 40; i++) px(&c, lx + (i * 53 + g->id[0] * 7) % LW, ly + (i * 29 + 5) % (LH - 30), rgb(200, 200, 255));
        wrap_text(&c, lx + LW / 2, ly + 30, g->name, 0xFFFFFF);
        const char *m = !g->have_rom ? "INSERT ROM" : g->label_state == 3 ? "" : "PREPARING...";
        text_c(&c, lx + LW / 2, ly + LH - 12, m, 1, s_accent, s_accent, 1);
    }
    /* grip: ribs under the label */
    for (int i = 0; i < 6; i++) {
        int y = ly + LH + 10 + i * 4;
        rect(&c, 18, y, CW - 36, 1, lo); rect(&c, 18, y + 1, CW - 36, 1, rgb(40, 40, 48));
    }
    text_c(&c, CW / 2, CH - 9, "PIXELSORCERY", 1, rgb(48, 48, 56), rgb(48, 48, 56), 0);
    if (!g->cart) {
        g->cart = SDL_CreateTexture(R, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, CW, CH);
        SDL_SetTextureBlendMode(g->cart, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(g->cart, SDL_ScaleModeLinear);
    }
    SDL_UpdateTexture(g->cart, NULL, pix, CW * 4);
}

/* ------------------------------------------------------------------ screen */
static uint32_t bgpix[WMAX * H], fgpix[WMAX * H];
static float s_clip_fade, s_clip_t;                 /* the selected cartridge's clip: opacity, time */
static SDL_Texture *bgtex, *fgtex;

static float s_launch;                               /* start animation 0..1 (0: none) */
/* version distance of game i from its column's shown version: the others stack behind it,
 * peeking out above (earlier versions) and below (later ones) */
static float cart_vdist(int i) { return s_rowof[i] - s_vpos[s_colof[i]]; }
static void cart_place(int i, float pos, float t, int sel, SDL_FRect *r, float *dim) {
    float d = s_colof[i] - pos, ad = fabsf(d);
    float v = cart_vdist(i), av = fabsf(v) < 2 ? fabsf(v) : 2;
    float sc = (1.0f - (ad < 2 ? ad : 2) * 0.2f) * (1.0f - av * 0.12f);
    float cx = W / 2 + d * 128 - (d > 0 ? 1 : -1) * (ad > 1 ? (ad - 1) * 22 : 0);
    r->w = CW * sc; r->h = CH * sc;
    r->x = cx - r->w / 2; r->y = 58 + (CH - r->h) * 0.55f + v * (v < 0 ? 24 : 9) * (1.0f - (ad < 2 ? ad : 2) * 0.2f);
    if (i == sel) r->y += sinf(t * 2.6f) * 2.0f - 3.0f * (1 - (ad < 1 ? ad : 1));
    *dim = (1.0f - (ad < 1.6f ? ad : 1.6f) * 0.38f) * (1.0f - (av < 1 ? av : 1) * 0.5f);
    if (s_launch > 0) {                                /* the chosen cartridge drops into the slot */
        if (i == sel) { float k = s_launch < 0.2f ? -s_launch * 40 : (s_launch - 0.2f) * (s_launch - 0.2f) * 420; r->y += k; }
        else *dim *= 1 - s_launch;
    }
}

static void draw_bg(float pos, float t) {
    Buf b = { bgpix, W, H };
    for (int y = 0; y < H; y++)                        /* ribbed black plastic, a vignette */
        for (int x = 0; x < W; x++) {
            float vx = (x - W / 2) / (float)(W / 2), vy = (y - H * 0.45f) / (float)(H / 2);
            float v = 1.0f - 0.45f * (vx * vx * 0.6f + vy * vy * 0.8f);
            int rib = y % 5;
            int base = rib == 0 ? 30 : rib == 1 ? 9 : 17;
            bgpix[y * W + x] = rgb((int)(base * v), (int)(base * v), (int)((base + 3) * v));
        }
    /* the big circle of the console's top, faint, behind the shelf */
    for (int y = 40; y < 240; y++)
        for (int x = 0; x < W; x++) {
            float dx = x - W / 2.0f, dy = (y - 138) * 1.0f, r = sqrtf(dx * dx + dy * dy);
            if (fabsf(r - 104) < 1.0f) blend(&b, x, y, 0x3A3A44, 140);
            if (fabsf(r - 101) < 0.6f) blend(&b, x, y, 0x000000, 160);
        }
    /* shadows under the cartridges */
    for (int i = 0; i < NG; i++) {
        if (fabsf(cart_vdist(i)) >= 0.5f) continue;
        SDL_FRect r; float dim; cart_place(i, pos, t, -1, &r, &dim);
        float cx = r.x + r.w / 2, cy = r.y + r.h + 6, rx = r.w * 0.55f, ry = 5;
        for (int y = (int)(cy - ry); y <= (int)(cy + ry); y++)
            for (int x = (int)(cx - rx); x <= (int)(cx + rx); x++) {
                float e = ((x - cx) / rx) * ((x - cx) / rx) + ((y - cy) / ry) * ((y - cy) / ry);
                if (e < 1) blend(&b, x, y, 0, (int)((1 - e) * 170 * dim));
            }
    }
}

static int s_panel = -1;                            /* settings panel: selected row, -1 closed */
#define PANEL_ROWS (s_disc ? 11 : 10)              /* the disc look: + its home screen's style */
static void panel_row(int i, char *out, size_t n) {
    switch (i) {
    case 0: snprintf(out, n, "ASPECT RATIO          < %s >", k_aspects[SET.aspect]); break;
    case 1: snprintf(out, n, "WIDESCREEN ENEMIES    %s", SET.objects ? "ON" : "OFF"); break;
    case 2: snprintf(out, n, "MODERN WATER          %s", SET.water ? "ON" : "OFF"); break;
    case 3: snprintf(out, n, "FULL SCREEN           %s", SET.fullscreen ? "ON" : "OFF"); break;
    case 4: snprintf(out, n, "LOW LATENCY           %s", SET.latency ? "ON" : "OFF"); break;
    case 5: snprintf(out, n, "VOLUME                < %d%% >", SET.volume); break;
    case 6: snprintf(out, n, "RENDERER              %s", SET.metal ? "METAL" : "OPENGL"); break;
    case 7: snprintf(out, n, "CONTROLLER SETUP      %s", s_pad ? "" : "(NO GAMEPAD)"); break;
    case 8: snprintf(out, n, "SHELF MUSIC           %s", SET.music ? "ON" : "OFF"); break;
    case 9: if (s_disc) { snprintf(out, n, "HOME SCREEN           < %s >", k_styles[SET.style]); break; }
            /* fall through */
    default: snprintf(out, n, "BACK"); break;
    }
}
static void panel_change(int d) {
    switch (s_panel) {
    case 0: SET.aspect = (SET.aspect + d + 5) % 5; break;
    case 1: SET.objects ^= 1; break;
    case 2: SET.water ^= 1; break;
    case 3: SET.fullscreen ^= 1; s_fullscreen = SET.fullscreen;
            SDL_SetWindowFullscreen(Win, s_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0); break;
    case 4: SET.latency ^= 1; break;
    case 5: SET.volume = SET.volume + d * 10 < 0 ? 0 : SET.volume + d * 10 > 100 ? 100 : SET.volume + d * 10; break;
    case 6: SET.metal ^= 1; break;
    case 7: if (s_pad && d > 0) { s_cap = 0; for (int i = 0; i < 8; i++) s_cap_map[i] = -1; } break;
    case 8: SET.music ^= 1; break;
    case 9: if (s_disc) { SET.style ^= 1; break; }
            s_panel = -1; break;
    default: s_panel = -1; break;
    }
    settings_save();
}
static void draw_panel(Buf *b) {
    int pw = 300, ph = 26 + PANEL_ROWS * 15, px0 = (W - pw) / 2, py0 = 48;
    rect_a(b, 0, 43, W, H - 61, 0x000000, 150);
    rect(b, px0, py0, pw, ph, rgb(18, 18, 22));
    rect(b, px0, py0, pw, 12, rgb(200, 24, 22));
    s_italic = 1; text(b, px0 + 6, py0 + 3, "SETTINGS", 1, 0xFFFFFF, 0xFFD8D8, 0); s_italic = 0;
    text(b, px0 + pw - 6 - text_w("FOR EVERY GAME", 1), py0 + 3, "FOR EVERY GAME", 1, 0xFFD8D8, 0xFFD8D8, 0);
    if (s_cap >= 0) {                                  /* controller setup: one button at a time */
        char t[64]; snprintf(t, sizeof t, "PRESS THE BUTTON FOR  %s", k_pad_names[s_cap]);
        text_c(b, W / 2, py0 + 40, t, 2, 0xFFFFFF, 0xC8CCD8, 1);
        text_c(b, W / 2, py0 + 70, SDL_GameControllerName(s_pad) ? SDL_GameControllerName(s_pad) : "", 1, 0x9AA4BA, 0x9AA4BA, 0);
        text_c(b, W / 2, py0 + 100, "SPACE  SKIP     ESC  CANCEL", 1, 0x9AA4BA, 0x9AA4BA, 0);
        return;
    }
    for (int i = 0; i < PANEL_ROWS; i++) {
        char t[64]; panel_row(i, t, sizeof t);
        int y = py0 + 20 + i * 15;
        if (i == s_panel) { rect(b, px0 + 4, y - 4, pw - 8, 15, rgb(40, 40, 52)); text(b, px0 + 10, y, ">", 1, s_accent, s_accent, 0); }
        text(b, px0 + 22, y, t, 1, i == s_panel ? 0xFFFFFF : 0xA0A4B0, i == s_panel ? 0xFFFFFF : 0xA0A4B0, 0);
    }
}

static void draw_fg(int sel, float t, const char *msg) {
    Buf b = { fgpix, W, H };
    memset(fgpix, 0, sizeof fgpix);
    /* header: chrome title, the red stripe, a 16-BIT tag */
    rect_a(&b, 0, 0, W, 40, 0x000000, 120);
    int sc = text_w(s_header, 3) > W - 120 ? 2 : 3;
    s_italic = 1;
    text(&b, 16, 20 - 7 * sc / 2, s_header, sc, 0xFFFFFF, 0x7C8498, 1);
    s_italic = 0;
    rect(&b, 0, 40, W, 2, rgb(220, 30, 26)); rect(&b, 0, 42, W, 1, rgb(90, 10, 10));
    int tw = text_w("16-BIT", 2) + 14, tx = W - 16 - tw;
    for (int y = 0; y < 18; y++) rect(&b, tx + (18 - y) / 3, 11 + y, tw, 1, rgb(220 - y * 3, 30, 26));   /* slanted tag */
    s_italic = 1; text(&b, tx + 7, 13, "16-BIT", 2, 0xFFFFFF, 0xFFE0E0, 1); s_italic = 0;
    /* the selected title */
    Game *g = &G[sel];
    text_c(&b, W / 2, 219, g->name, text_w(g->name, 2) > W - 20 ? 1 : 2, 0xFFFFFF, 0xC8CCD8, 1);
    char info[96]; snprintf(info, sizeof info, "%s   %s %s", g->year, g->players, strcmp(g->players, "1") ? "PLAYERS" : "PLAYER");
    if (s_nrow[s_colof[sel]] > 1)                   /* which version of the game */
        snprintf(info + strlen(info), sizeof info - strlen(info), "   VERSION %d/%d", s_rowof[sel] + 1, s_nrow[s_colof[sel]]);
    text_c(&b, W / 2, 237, info, 1, 0x9AA4BA, 0x9AA4BA, 1);
    /* page dots */
    for (int c = 0; c < s_ncol; c++) {
        int x = W / 2 - s_ncol * 5 + c * 10 + 3;
        rect(&b, x, 211, 4, 4, c == s_colof[sel] ? s_accent | 0xFF000000u : rgb(70, 70, 80));
    }
    /* footer */
    rect_a(&b, 0, H - 17, W, 17, 0x000000, 170);
    rect(&b, 0, H - 18, W, 1, rgb(60, 60, 70));
    const char *act = !g->have_rom ? "START  CHOOSE ROM" : "START  PLAY";
    if (msg && *msg) text_c(&b, W / 2, H - 12, msg, 1, s_accent, s_accent, 0);
    else {
        text(&b, 10, H - 12, s_nrow[s_colof[sel]] > 1 ? "< >  GAME   UP DOWN  VERSION" : "< >  SELECT", 1, 0xB0B4C0, 0xB0B4C0, 0);
        text_c(&b, W / 2, H - 12, act, 1, ((int)(t * 2) & 1) ? 0xFFFFFF : s_accent, ((int)(t * 2) & 1) ? 0xFFFFFF : s_accent, 0);
        text(&b, W - 10 - text_w("TAB  SETTINGS   ESC  QUIT", 1), H - 12, "TAB  SETTINGS   ESC  QUIT", 1, 0xB0B4C0, 0xB0B4C0, 0);
    }
    if (s_panel >= 0) draw_panel(&b);
    if (s_choose >= 0) draw_choices(&b, g);
}


/* ------------------------------------------------------------------ the disc look */
/* "look disc" (Ultimate SH, BACKLOG 0.7.5): the Mega CD compilation's game-select screen. The
 * player's menu picture fills the screen, which is as wide as the aspect ratio chosen in the settings
 * (4:3 to 32:9, 270 lines); the cards (one per column of the config: a game and its versions) sit in
 * rows of four with the game's title screen as their picture, the selected one in a yellow frame
 * playing its attract clip. START on a card with versions asks which (SH1 / SHK1,
 * the soundtrack of SHCD...); a COMING SOON card is shown and not started. */
static SDL_Surface *s_bgsurf; static int s_bgw = -1;
static const int k_anum[] = { 4, 16, 16, 21, 32 }, k_aden[] = { 3, 10, 9, 9, 9 };
static int disc_width(void) {
    int a = SET.aspect < 0 || SET.aspect > 4 ? 2 : SET.aspect;
    if (getenv("PS_LAUNCHER_ASPECT")) a = atoi(getenv("PS_LAUNCHER_ASPECT")) % 5;   /* tests */
    int w = (H * k_anum[a] / k_aden[a] + 1) & ~1;
    return w > WMAX ? WMAX : w;
}
static void disc_aspect(void) {
    int w = disc_width();
    if (w == W && bgtex) return;
    W = w;
    if (bgtex) SDL_DestroyTexture(bgtex);
    if (fgtex) SDL_DestroyTexture(fgtex);
    bgtex = SDL_CreateTexture(R, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, W, H);
    fgtex = SDL_CreateTexture(R, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, W, H);
    SDL_SetTextureBlendMode(fgtex, SDL_BLENDMODE_BLEND);
    SDL_RenderSetLogicalSize(R, W, H);
    s_bgw = -1;
}
static void disc_bg(void) {                          /* the picture, scaled to cover the screen, dimmed */
    if (s_bgw == W) return;
    s_bgw = W;
    if (!s_bgsurf && *s_bgfile) {
        SDL_Surface *l = SDL_LoadBMP(s_bgfile);
        if (l) { s_bgsurf = SDL_ConvertSurfaceFormat(l, SDL_PIXELFORMAT_ARGB8888, 0); SDL_FreeSurface(l); }
        if (!s_bgsurf) *s_bgfile = 0;
    }
    if (s_bgsurf) {
        const uint32_t *sp = s_bgsurf->pixels; int pitch = s_bgsurf->pitch / 4, sw = s_bgsurf->w, sh = s_bgsurf->h;
        float k = (float)W / sw > (float)H / sh ? (float)W / sw : (float)H / sh;      /* cover */
        float ox = (sw * k - W) / 2, oy = (sh * k - H) / 2;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                int sx = (int)((x + ox) / k), sy = (int)((y + oy) / k);
                if (sx >= sw) sx = sw - 1;
                if (sy >= sh) sy = sh - 1;
                bgpix[y * W + x] = sp[sy * pitch + sx];
            }
        /* a backdrop: blurred (the picture's own drawing - the disc's cards - stays behind ours) and dimmed */
        uint32_t *tmp = malloc((size_t)W * H * 4);
        for (int pass = 0; pass < 3 && tmp; pass++)
            for (int dir = 0; dir < 2; dir++) {
                uint32_t *src = dir ? tmp : bgpix, *dst = dir ? bgpix : tmp;
                for (int y = 0; y < H; y++)
                    for (int x = 0; x < W; x++) {
                        int r = 0, gg = 0, bb = 0, n = 0;
                        for (int k = -3; k <= 3; k++) {
                            int xx = dir ? x : x + k, yy = dir ? y + k : y;
                            if (xx < 0 || yy < 0 || xx >= W || yy >= H) continue;
                            uint32_t c = src[yy * W + xx]; r += c >> 16 & 255; gg += c >> 8 & 255; bb += c & 255; n++;
                        }
                        dst[y * W + x] = rgb(r / n, gg / n, bb / n);
                    }
            }
        free(tmp);
        for (int i = 0; i < W * H; i++) {
            uint32_t c = bgpix[i];
            bgpix[i] = rgb((c >> 16 & 255) * 3 / 8, (c >> 8 & 255) * 3 / 8, (c & 255) * 3 / 8 + 6);
        }
    } else
        for (int y = 0; y < H; y++)                  /* no picture: the plain menu's night blue */
            for (int x = 0; x < W; x++) {
                int glow = 40 - abs(x - W / 2) * 40 / (W / 2);
                bgpix[y * W + x] = rgb(12 + glow / 4, 18 + glow / 3, 52 + y / 6 + glow / 2);
            }
}
static int disc_rows(void) { return (s_ncol + 3) / 4; }
static void disc_card(int c, SDL_FRect *r) {         /* column c's card */
    int rows = disc_rows();
    float gap = 10, top = 44, bottom = 44, name = W < 440 ? 21 : 12;   /* narrow: names on two lines */
    float cw = (W - 32 - 3 * gap) / 4, ch = cw * LH / LW;
    float maxh = (H - top - bottom - (rows - 1) * gap) / rows - name;
    if (ch > maxh) { ch = maxh; cw = ch * LW / LH; }
    float x0 = (W - (4 * cw + 3 * gap)) / 2, y0 = top + (H - top - bottom - rows * (ch + name) - (rows - 1) * gap) / 2;
    r->x = x0 + (c % 4) * (cw + gap); r->y = y0 + (c / 4) * (ch + name + gap); r->w = cw; r->h = ch;
}
static void disc_label(Game *g) {                    /* the title screen as a texture, once captured */
    if (g->labeltex || g->label_state != 2) return;
    char label[1100]; snprintf(label, sizeof label, "%s/label.bmp", g->store);
    SDL_Surface *s = SDL_LoadBMP(label);
    if (!s) return;
    g->labeltex = SDL_CreateTextureFromSurface(R, s);
    if (g->labeltex) SDL_SetTextureScaleMode(g->labeltex, SDL_ScaleModeLinear);
    SDL_FreeSurface(s);
}
static void fit_text(Buf *b, int cx, int y, const char *s, int maxw, uint32_t c) {   /* one line, cut to fit */
    char t[160]; snprintf(t, sizeof t, "%s", s);
    while (strlen(t) > 1 && text_w(t, 1) > maxw) t[strlen(t) - 1] = 0;
    text_c(b, cx, y, t, 1, c, c, 1);
}
static int fit_text2(Buf *b, int cx, int y, const char *s, int maxw, uint32_t c) {  /* two lines at most */
    if (text_w(s, 1) <= maxw) { text_c(b, cx, y, s, 1, c, c, 1); return 1; }
    char t[160]; snprintf(t, sizeof t, "%s", s);
    char *cut = NULL;
    for (char *p = t; *p; p++) if (*p == ' ') { *p = 0; if (text_w(t, 1) <= maxw) cut = p; *p = ' '; }
    if (!cut) { fit_text(b, cx, y, s, maxw, c); return 1; }
    *cut = 0;
    text_c(b, cx, y, t, 1, c, c, 1);
    fit_text(b, cx, y + 9, cut + 1, maxw, c);
    return 2;
}
/* a soft backdrop at the window's resolution: the picture averaged down to a few dozen pixels, drawn back
 * up with linear filtering (a blur for free); NULL when there is no picture */
static SDL_Texture *soft_tex(SDL_Surface *src, int w, int h) {
    SDL_Surface *t = src ? SDL_ConvertSurfaceFormat(src, SDL_PIXELFORMAT_ARGB8888, 0) : NULL;
    if (!t) return NULL;
    uint32_t *o = malloc((size_t)w * h * 4);
    const uint32_t *sp = t->pixels; int pitch = t->pitch / 4;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int ax = x * t->w / w, bx = (x + 1) * t->w / w, ay = y * t->h / h, by = (y + 1) * t->h / h, r = 0, g2 = 0, b = 0, n = 0;
            for (int j = ay; j < by; j++) for (int i = ax; i < bx; i++) { uint32_t c = sp[j * pitch + i]; r += c >> 16 & 255; g2 += c >> 8 & 255; b += c & 255; n++; }
            o[y * w + x] = n ? rgb(r / n, g2 / n, b / n) : 0xFF000000u;
        }
    SDL_Texture *tx = SDL_CreateTexture(R, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, w, h);
    if (tx) { SDL_UpdateTexture(tx, NULL, o, w * 4); SDL_SetTextureScaleMode(tx, SDL_ScaleModeLinear); }
    free(o); SDL_FreeSurface(t);
    return tx;
}
static void cover(SDL_Texture *tx, int lw, int lh, Uint8 mod) {   /* the texture filling lw x lh, cropped */
    int tw, th; SDL_QueryTexture(tx, NULL, NULL, &tw, &th);
    float k = (float)lw / tw > (float)lh / th ? (float)lw / tw : (float)lh / th;
    SDL_FRect d = { (lw - tw * k) / 2, (lh - th * k) / 2, tw * k, th * k };
    SDL_SetTextureColorMod(tx, mod, mod, mod);
    SDL_RenderCopyF(R, tx, NULL, &d);
}
static SDL_Texture *s_menusoft;
static int s_menusoft_tried;
static SDL_Texture *menu_soft(void) {
    if (!s_menusoft_tried) {
        s_menusoft_tried = 1;
        SDL_Surface *l = *s_bgfile ? SDL_LoadBMP(s_bgfile) : NULL;
        if (l) { s_menusoft = soft_tex(l, 64, 45); SDL_FreeSurface(l); }
    }
    return s_menusoft;
}
static void present_modern(float pos, float t, int sel, const char *msg);
static void present_disc(float t, int sel, const char *msg) {
    disc_aspect(); disc_bg();
    Buf b = { fgpix, W, H };
    memset(fgpix, 0, sizeof fgpix);
    int sc = text_w(s_header, 3) > W - 40 ? 2 : 3;      /* the heading, yellow as the disc's frame */
    text_c(&b, W / 2, 22 - 7 * sc / 2, s_header, sc, 0xFFFFFF, 0xFFE000, 1);
    int selc = s_colof[sel];
    for (int c = 0; c < s_ncol; c++) {
        Game *g = &G[s_cell[c][0]];
        SDL_FRect r; disc_card(c, &r);
        int x = (int)r.x, y = (int)r.y, w = (int)r.w, h = (int)r.h;
        if (!g->labeltex) {                          /* no picture: the name on the card */
            uint32_t top = g->soon == 1 ? rgb(34, 34, 40) : rgb(30, 54, 130), bot = g->soon == 1 ? rgb(18, 18, 22) : rgb(16, 28, 80);
            for (int j = 0; j < h; j++) rect(&b, x, y + j, w, 1, j < h / 2 ? top : bot);
            rect(&b, x, y, w, 1, g->soon == 1 ? rgb(70, 70, 80) : rgb(90, 140, 230));
            const char *m = g->soon == 1 ? "COMING SOON" : g->soon == 2 ? "YOUR COPY" : !g->have_rom ? "INSERT ROM" : g->label_state == 3 ? "" : "PREPARING...";
            fit_text2(&b, x + w / 2, y + h / 2 - 12, g->name, w - 6, g->soon == 1 ? 0x8A8A96 : 0xFFFFFF);
            fit_text(&b, x + w / 2, y + h / 2 + 8, m, w - 6, g->soon == 1 ? 0xC8A040 : s_accent);
        }
        if (s_nrow[c] > 1) {                         /* a card with versions: how many */
            char v[16]; snprintf(v, sizeof v, "x%d", s_nrow[c]);
            int vw = text_w(v, 1) + 6;
            rect(&b, x + w - vw - 2, y + 2, vw, 11, rgb(255, 224, 0));
            text(&b, x + w - vw + 1, y + 4, v, 1, 0x101010, 0x101010, 0);
        }
        fit_text2(&b, x + w / 2, y + h + 3, g->name, w + 8, c == selc ? 0xFFE000 : g->soon == 1 ? 0x8A8A96 : 0xE0E4F0);
        if (c == selc) {                             /* the yellow frame */
            int p = 200 + (int)(55 * sinf(t * 5.0f));
            uint32_t yc = rgb(p, p * 224 / 255, 0);
            for (int e = 1; e <= 3; e++) {
                rect(&b, x - e, y - e, w + 2 * e, 1, yc); rect(&b, x - e, y + h + e - 1, w + 2 * e, 1, yc);
                rect(&b, x - e, y - e, 1, h + 2 * e, yc); rect(&b, x + w + e - 1, y - e, 1, h + 2 * e, yc);
            }
        }
    }
    Game *g = &G[sel];                               /* footer: the selection, the keys */
    rect_a(&b, 0, H - 30, W, 30, 0x000000, 150);
    char info[128]; snprintf(info, sizeof info, "%s  %s", g->name, g->year);
    fit_text(&b, W / 2, H - 27, info, W - 20, 0xFFFFFF);
    const char *act = g->soon == 1 ? "COMING SOON" : !g->have_rom ? "START  CHOOSE ROM" : s_nrow[selc] > 1 ? "START  CHOOSE VERSION" : "START  PLAY";
    if (msg && *msg) text_c(&b, W / 2, H - 13, msg, 1, s_accent, s_accent, 0);
    else if (W >= 440) {
        text(&b, 8, H - 13, "ARROWS  SELECT", 1, 0xB0B4C0, 0xB0B4C0, 0);
        text_c(&b, W / 2, H - 13, act, 1, ((int)(t * 2) & 1) ? 0xFFFFFF : 0xFFE000, ((int)(t * 2) & 1) ? 0xFFFFFF : 0xFFE000, 0);
        text(&b, W - 8 - text_w("TAB  SETTINGS  ESC  QUIT", 1), H - 13, "TAB  SETTINGS  ESC  QUIT", 1, 0xB0B4C0, 0xB0B4C0, 0);
    } else text_c(&b, W / 2, H - 13, act, 1, 0xFFE000, 0xFFE000, 0);
    if (s_panel >= 0) draw_panel(&b);
    if (s_choose >= 0) draw_choices(&b, g);
    SDL_UpdateTexture(bgtex, NULL, bgpix, W * 4);
    SDL_UpdateTexture(fgtex, NULL, fgpix, W * 4);
    SDL_SetRenderDrawColor(R, 0, 0, 0, 255);
    SDL_RenderClear(R);
    if (menu_soft()) cover(menu_soft(), W, H, 100);  /* the picture, soft, at the window's resolution */
    else SDL_RenderCopy(R, bgtex, NULL, NULL);
    for (int c = 0; c < s_ncol; c++) {               /* the pictures */
        Game *cg = &G[s_cell[c][0]];
        SDL_FRect r; disc_card(c, &r);
        SDL_SetRenderDrawColor(R, 0, 0, 0, 160);     /* a shadow */
        SDL_FRect sh = { r.x + 3, r.y + 4, r.w, r.h }; SDL_RenderFillRectF(R, &sh);
        if (!cg->labeltex) continue;
        int tw, th; SDL_QueryTexture(cg->labeltex, NULL, NULL, &tw, &th);
        SDL_Rect src = { tw > 320 ? (tw - 320) / 2 : 0, 0, tw > 320 ? 320 : tw, th };     /* a wide dump: its 4:3 middle */
        Uint8 m = c == selc ? 255 : 210;
        SDL_SetTextureColorMod(cg->labeltex, m, m, m);
        SDL_RenderCopyF(R, cg->labeltex, &src, &r);
        if (c == selc && G[sel].clip && s_clip_fade > 0) {   /* the attract clip over it */
            int f = (int)(s_clip_t * 15.0f) % DEMO_N;
            SDL_Rect cs = { 0, f * LH, LW, LH };
            SDL_SetTextureAlphaMod(G[sel].clip, (Uint8)(255 * s_clip_fade));
            SDL_RenderCopyF(R, G[sel].clip, &cs, &r);
        }
    }
    SDL_RenderCopy(R, fgtex, NULL, NULL);
    if (!s_noflip) SDL_RenderPresent(R);
}


/* the chooser's lines for the disc look: column c's versions that can start */
static int disc_versions(int c) {
    s_nch = 0;
    for (int r = 0; r < s_nrow[c] && s_nch < 8; r++) {
        Game *v = &G[s_cell[c][r]];
        if (*v->audio && !*v->audio_path) continue;            /* that soundtrack's disc is not there */
        int other = 0;                      /* beside another disc's soundtrack: this one's own */
        for (int k = 0; k < s_nrow[c]; k++) other |= *G[s_cell[c][k]].audio != 0;
        snprintf(s_ch[s_nch].label, sizeof s_ch[0].label, "%s%s", v->name, other && !*v->audio ? " - JP/EU soundtrack" : "");
        *s_ch[s_nch].resume = 0;
        s_ch[s_nch++].game = s_cell[c][r];
    }
    return s_nch;
}

/* ------------------------------------------------------------------ the disc look, MODERN */
/* the home screen as a modern landing (the settings' HOME SCREEN): drawn at twice the lines (540), the games
 * in a carousel - the selected title screen large in the middle, the others smaller and dimmed beside it -,
 * the selected game's art behind, soft; its name large under it. The keys and choosers are the same. */
static uint32_t *s_hipix; static SDL_Texture *s_hitex; static int s_hiw;
static void modern_card(int c, float pos, SDL_FRect *r, float *dim) {
    int W2 = W * 2;
    float d = c - pos, ad = fabsf(d) < 3 ? fabsf(d) : 3;
    float h = 250 * (1.0f - ad * 0.2f), w = h * LW / LH;
    float spread = W2 < 900 ? 200 : 300;
    float cx = W2 / 2 + d * spread - (d > 0 ? 1 : -1) * (ad > 1 ? (ad - 1) * spread * 0.25f : 0);
    r->w = w; r->h = h; r->x = cx - w / 2; r->y = 228 - h / 2;
    *dim = 1.0f - (ad < 1.5f ? ad : 1.5f) * 0.4f;
}
static SDL_Texture *s_soft[MAXG]; static int s_soft_tried[MAXG];
static void present_modern(float pos, float t, int sel, const char *msg) {
    disc_aspect();
    int W2 = W * 2, H2 = H * 2;
    if (s_hiw != W2 || !s_hipix) {
        free(s_hipix); s_hipix = malloc((size_t)W2 * H2 * 4); s_hiw = W2;
        if (s_hitex) SDL_DestroyTexture(s_hitex);
        s_hitex = SDL_CreateTexture(R, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, W2, H2);
        SDL_SetTextureBlendMode(s_hitex, SDL_BLENDMODE_BLEND);
    }
    SDL_RenderSetLogicalSize(R, W2, H2);
    SDL_SetRenderDrawColor(R, 6, 10, 28, 255);
    SDL_RenderClear(R);
    /* behind: the selected game's art, soft (its title screen), else the menu picture, else night blue */
    int gi = s_cell[s_colof[sel]][0];
    if (!s_soft_tried[gi] && G[gi].label_state == 2) {
        s_soft_tried[gi] = 1;
        char label[1100]; snprintf(label, sizeof label, "%s/label.bmp", G[gi].store);
        SDL_Surface *ls = SDL_LoadBMP(label);
        if (ls) { s_soft[gi] = soft_tex(ls, 32, 22); SDL_FreeSurface(ls); }
    }
    SDL_Texture *back = s_soft[gi] ? s_soft[gi] : menu_soft();
    if (back) cover(back, W2, H2, 120);
    SDL_SetRenderDrawBlendMode(R, SDL_BLENDMODE_BLEND);
    for (int y = 0; y < H2; y += 6) {               /* darker towards the bottom, where the text is */
        float k = y / (float)H2;
        SDL_SetRenderDrawColor(R, 4, 6, 18, (Uint8)(60 + 170 * k * k));
        SDL_FRect band = { 0, (float)y, (float)W2, 6 }; SDL_RenderFillRectF(R, &band);
    }
    Buf b = { s_hipix, W2, H2 };
    memset(s_hipix, 0, (size_t)W2 * H2 * 4);
    /* the cards, far ones first */
    int order[MAXG], n = 0;
    for (int c = 0; c < s_ncol; c++) order[n++] = c;
    for (int a = 0; a < n; a++) for (int z = a + 1; z < n; z++)
        if (fabsf(order[z] - pos) > fabsf(order[a] - pos)) { int x = order[a]; order[a] = order[z]; order[z] = x; }
    int selc = s_colof[sel];
    for (int k = 0; k < n; k++) {
        int c = order[k]; Game *g = &G[s_cell[c][0]];
        SDL_FRect r; float dim; modern_card(c, pos, &r, &dim);
        if (r.x > W2 || r.x + r.w < 0) continue;
        SDL_SetRenderDrawColor(R, 0, 0, 0, 120);
        SDL_FRect sh = { r.x + 6, r.y + 10, r.w, r.h }; SDL_RenderFillRectF(R, &sh);
        if (c == selc && fabsf(c - pos) < 0.3f) {     /* the selection: a glow, a white edge */
            float p = 0.6f + 0.4f * sinf(t * 3.0f);
            for (int e = 10; e >= 1; e--) {
                SDL_SetRenderDrawColor(R, 255, 224, 0, (Uint8)(p * 10 * (11 - e)));
                SDL_FRect o = { r.x - e - 3, r.y - e - 3, r.w + 2 * (e + 3), r.h + 2 * (e + 3) };
                SDL_RenderDrawRectF(R, &o);
            }
            SDL_SetRenderDrawColor(R, 255, 255, 255, 255);
            for (int e = 1; e <= 3; e++) { SDL_FRect o = { r.x - e, r.y - e, r.w + 2 * e, r.h + 2 * e }; SDL_RenderDrawRectF(R, &o); }
        }
        if (g->labeltex) {
            int tw, th; SDL_QueryTexture(g->labeltex, NULL, NULL, &tw, &th);
            SDL_Rect src = { tw > 320 ? (tw - 320) / 2 : 0, 0, tw > 320 ? 320 : tw, th };
            Uint8 m = (Uint8)(255 * dim * (g->soon == 1 ? 0.5f : 1));
            SDL_SetTextureColorMod(g->labeltex, m, m, m);
            SDL_RenderCopyF(R, g->labeltex, &src, &r);
            if (c == selc && G[sel].clip && s_clip_fade > 0) {
                int f = (int)(s_clip_t * 15.0f) % DEMO_N;
                SDL_Rect cs = { 0, f * LH, LW, LH };
                SDL_SetTextureAlphaMod(G[sel].clip, (Uint8)(255 * s_clip_fade));
                SDL_RenderCopyF(R, G[sel].clip, &cs, &r);
            }
        } else {                                     /* no picture: a panel with the name */
            Uint8 m = (Uint8)(dim * (g->soon == 1 ? 40 : 70));
            SDL_SetRenderDrawColor(R, m / 2, m / 2, (Uint8)(g->soon == 1 ? m : m * 2), 235);
            SDL_RenderFillRectF(R, &r);
            uint32_t col = g->soon == 1 ? 0x9A9AA6 : 0xFFFFFF;
            int sc = r.h > 200 ? 3 : 2;
            if (text_w(g->name, sc) > r.w - 16) sc = 2;
            if (text_w(g->name, sc) > r.w - 16) sc = 1;
            text_c(&b, (int)(r.x + r.w / 2), (int)(r.y + r.h / 2 - 7 * sc), g->name, sc, col, col, 1);
            const char *m2 = g->soon == 1 ? "COMING SOON" : g->soon == 2 ? "YOUR COPY" : !g->have_rom ? "INSERT ROM" : "PREPARING...";
            text_c(&b, (int)(r.x + r.w / 2), (int)(r.y + r.h / 2 + 6 * sc), m2, sc > 1 ? sc - 1 : 1, 0xFFC840, 0xFFC840, 1);
        }
    }
    /* text: the collection, the selected game, the keys */
    s_italic = 1; text(&b, 28, 22, s_header, 3, 0xFFFFFF, 0xFFE000, 1); s_italic = 0;
    Game *g = &G[sel];
    int sc = text_w(g->name, 5) > W2 - 40 ? 3 : 5;
    text_c(&b, W2 / 2, 382, g->name, sc, 0xFFFFFF, 0xD8DCE8, 1);
    char info[96];
    int nv = s_nrow[selc];
    if (g->soon == 1) snprintf(info, sizeof info, "%s   COMING SOON", g->year);
    else snprintf(info, sizeof info, "%s   %s %s%s", g->year, g->players, strcmp(g->players, "1") ? "PLAYERS" : "PLAYER",
                  nv > 1 ? (nv == 2 ? "   2 VERSIONS" : "   VERSIONS") : "");
    text_c(&b, W2 / 2, 382 + 8 * sc + 10, info, 2, 0xB8C0D8, 0xB8C0D8, 1);
    for (int c = 0; c < s_ncol; c++) {               /* where we are */
        int x = W2 / 2 - s_ncol * 9 + c * 18 + 4;
        rect(&b, x, 462, 10, 4, c == selc ? 0xFFFFE000u : rgb(90, 96, 120));
    }
    const char *act = g->soon == 1 ? "COMING SOON" : !g->have_rom ? "START  CHOOSE ROM" : nv > 1 ? "START  CHOOSE VERSION" : "START  PLAY";
    if (msg && *msg) text_c(&b, W2 / 2, H2 - 36, msg, 2, 0xFFE000, 0xFFE000, 1);
    else {
        text_c(&b, W2 / 2, H2 - 36, act, 2, ((int)(t * 2) & 1) ? 0xFFFFFF : 0xFFE000, ((int)(t * 2) & 1) ? 0xFFFFFF : 0xFFE000, 1);
        if (W2 >= 880) {
            text(&b, 28, H2 - 36, "< >  SELECT", 2, 0xB0B4C0, 0xB0B4C0, 1);
            text(&b, W2 - 28 - text_w("TAB  SETTINGS", 2), H2 - 36, "TAB  SETTINGS", 2, 0xB0B4C0, 0xB0B4C0, 1);
        }
    }
    SDL_UpdateTexture(s_hitex, NULL, s_hipix, W2 * 4);
    SDL_RenderCopy(R, s_hitex, NULL, NULL);
    if (s_panel >= 0 || s_choose >= 0) {             /* the panels: as the pixel look's, over everything */
        Buf p = { fgpix, W, H };
        memset(fgpix, 0, sizeof fgpix);
        if (s_panel >= 0) draw_panel(&p);
        if (s_choose >= 0) draw_choices(&p, g);
        SDL_UpdateTexture(fgtex, NULL, fgpix, W * 4);
        SDL_RenderCopy(R, fgtex, NULL, NULL);
    }
    if (!s_noflip) SDL_RenderPresent(R);
}
static int disc_style(void) { return getenv("PS_LAUNCHER_STYLE") ? atoi(getenv("PS_LAUNCHER_STYLE")) & 1 : SET.style; }
static void present(float pos, float t, int sel, const char *msg) {
    if (s_disc && disc_style()) { present_modern(pos, t, sel, msg); return; }
    if (s_disc) { SDL_RenderSetLogicalSize(R, W, H); present_disc(t, sel, msg); return; }
    draw_bg(pos, t); draw_fg(sel, t, msg);
    SDL_UpdateTexture(bgtex, NULL, bgpix, W * 4);
    SDL_UpdateTexture(fgtex, NULL, fgpix, W * 4);
    SDL_SetRenderDrawColor(R, 0, 0, 0, 255);
    SDL_RenderClear(R);
    SDL_RenderCopy(R, bgtex, NULL, NULL);
    int order[MAXG], no = 0;                         /* far cartridges first, stacked versions behind */
    float far[MAXG];
    for (int i = 0; i < NG; i++) if (fabsf(cart_vdist(i)) < 2.2f) { far[i] = fabsf(s_colof[i] - pos) * 4 + fabsf(cart_vdist(i)); order[no++] = i; }
    for (int a = 0; a < no; a++) for (int b2 = a + 1; b2 < no; b2++)
        if (far[order[b2]] > far[order[a]]) { int x = order[a]; order[a] = order[b2]; order[b2] = x; }
    for (int k = 0; k < no; k++) {
        int i = order[k];
        SDL_FRect r; float dim; cart_place(i, pos, t, sel, &r, &dim);
        if (i == sel && fabsf(s_colof[i] - pos) < 0.5f && fabsf(cart_vdist(i)) < 0.5f) {           /* glow around the selected cartridge */
            SDL_SetRenderDrawBlendMode(R, SDL_BLENDMODE_ADD);
            float p = 0.6f + 0.4f * sinf(t * 4.0f);
            for (int e = 4; e >= 1; e--) {
                SDL_SetRenderDrawColor(R, s_accent >> 16 & 255, s_accent >> 8 & 255, s_accent & 255, (Uint8)(p * 26 * (5 - e)));
                SDL_FRect o = { r.x - e, r.y - e + 10, r.w + 2 * e, r.h + 2 * e - 10 };
                SDL_RenderDrawRectF(R, &o);
            }
            SDL_SetRenderDrawBlendMode(R, SDL_BLENDMODE_BLEND);
        }
        Uint8 m = (Uint8)(255 * dim);
        SDL_SetTextureColorMod(G[i].cart, m, m, m);
        SDL_RenderCopyF(R, G[i].cart, NULL, &r);
        if (i == sel && G[i].clip && s_clip_fade > 0) {    /* the attract clip over the label */
            float k = r.w / CW;
            int f = (int)(s_clip_t * 15.0f) % DEMO_N;
            SDL_Rect src = { 0, f * LH, LW, LH };
            SDL_FRect dst = { r.x + (CW - LW) / 2 * k, r.y + 22 * k, LW * k, LH * k };
            SDL_SetTextureAlphaMod(G[i].clip, (Uint8)(255 * s_clip_fade));
            SDL_RenderCopyF(R, G[i].clip, &src, &dst);
        }
    }
    SDL_RenderCopy(R, fgtex, NULL, NULL);
    /* scanlines at the window's resolution */
    int ow, oh; SDL_GetRendererOutputSize(R, &ow, &oh);
    SDL_RenderSetLogicalSize(R, 0, 0);
    SDL_SetRenderDrawBlendMode(R, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(R, 0, 0, 0, 38);
    int step = oh / H >= 3 ? oh / H : 3;
    for (int y = step - 1; y < oh; y += step) SDL_RenderDrawLine(R, 0, y, ow, y);
    SDL_RenderSetLogicalSize(R, W, H);
    if (!s_noflip) SDL_RenderPresent(R);
}

int main(int argc, char **argv) {
#ifdef SDL_MAIN_HANDLED
    SDL_SetMainReady();                              /* inside another program's main (tools/ultimate) */
#endif
    const char *cfg = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--roms") && i + 1 < argc) snprintf(s_romdir, sizeof s_romdir, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--fullscreen")) s_fullscreen = 1;
        else if (!cfg) cfg = argv[i];
        else snprintf(s_gamesdir, sizeof s_gamesdir, "%s", argv[i]);
    }
    {   /* this executable (GAMESDIR "@self": the games are in it, --game ID) */
#ifdef _WIN32
        char *rp = _fullpath(NULL, argv[0], 0);
#else
        char *rp = realpath(argv[0], NULL);
#endif
        snprintf(s_self, sizeof s_self, "%s", rp ? rp : argv[0]); free(rp);
    }
    if (!cfg || !*s_gamesdir || !load_config(cfg)) {
        fprintf(stderr, "usage: ps_launcher CONFIG GAMESDIR [--roms DIR] [--fullscreen]\n");
        return 2;
    }
    char pref[1024];
    if (getenv("PS_LAUNCHER_HOME")) snprintf(pref, sizeof pref, "%s/", getenv("PS_LAUNCHER_HOME"));
    else { char *p = SDL_GetPrefPath("", "PixelSorcery"); snprintf(pref, sizeof pref, "%s", p ? p : "./"); SDL_free(p); }
    snprintf(s_pref, sizeof s_pref, "%s", pref);
    {   /* the shelf's settings: PREF/<name>.ini */
        char nm[128]; int k = 0; for (const char *c = s_name; *c && k < 120; c++) nm[k++] = (*c == ' ' || *c == '/') ? '-' : *c; nm[k] = 0;
        if (snprintf(s_setpath, sizeof s_setpath, "%s%s.ini", pref, nm) >= (int)sizeof s_setpath) *s_setpath = 0;   /* no settings file */
        settings_load();
        if (SET.fullscreen) s_fullscreen = 1;
    }
    for (int i = 0; i < NG; i++) {
        /* a version playing another disc's soundtrack keeps the disc of the title it runs */
        snprintf(G[i].store, sizeof G[i].store, "%s%s", pref, *G[i].audio ? G[i].port : G[i].id);
        mkdirs(G[i].store);
        find_rom(&G[i]);
    }
    if (s_disc) W = disc_width();
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    /* fullscreen as a borderless window over the screen, not a macOS Space of its own: the launcher and the
     * games it starts then cover each other in place, without the Space switch (no leaving fullscreen) */
    SDL_SetHint(SDL_HINT_VIDEO_MAC_FULLSCREEN_SPACES, "0");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) { fprintf(stderr, "%s\n", SDL_GetError()); return 1; }
    Win = SDL_CreateWindow(s_name, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, W * 2 + W * 2 / 3, H * 2 + H * 2 / 3,
                           SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | (s_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
    R = SDL_CreateRenderer(Win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!Win || !R) { fprintf(stderr, "%s\n", SDL_GetError()); return 1; }
    SDL_RenderSetLogicalSize(R, W, H);
    SDL_SetRenderDrawBlendMode(R, SDL_BLENDMODE_BLEND);
    bgtex = SDL_CreateTexture(R, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, W, H);
    fgtex = SDL_CreateTexture(R, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, W, H);
    SDL_SetTextureBlendMode(fgtex, SDL_BLENDMODE_BLEND);
    SDL_AudioSpec want = { 0 }, have; want.freq = SR; want.format = AUDIO_S16SYS; want.channels = 1; want.samples = 1024;
    want.callback = audio_cb;
    s_audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    s_music_on = SET.music; s_music_vol = SET.volume;
    if (s_audio && !getenv("PS_LAUNCHER_SHOT") && !getenv("PS_LAUNCHER_ICON")) SDL_PauseAudioDevice(s_audio, 0);
    for (int i = 0; i < SDL_NumJoysticks(); i++) if (SDL_IsGameController(i)) { SDL_GameController *c = SDL_GameControllerOpen(i); if (!s_pad) s_pad = c; }
    for (int i = 0; i < NG; i++) build_cart(&G[i]);
    if (!getenv("PS_LAUNCHER_ICON")) start_labels();

    int sel = 0, quit = 0, running = 0;
    float pos = 0, t = 0;
    for (int c = 0; c < s_ncol; c++) { s_cur[c] = 0; s_vpos[c] = 0; }
    Uint32 last = SDL_GetTicks(), hold_t = 0; int hold = 0;
    char msg[128] = ""; Uint32 msg_until = 0;
    if (getenv("PS_LAUNCHER_WAV")) {                /* tests: 16 s of the shelf music as a WAV */
        int n = SR * 16; int16_t *buf = malloc((size_t)n * 2);
        s_music_on = 1; audio_cb(NULL, (Uint8 *)buf, n * 2);
        FILE *f = fopen(getenv("PS_LAUNCHER_WAV"), "wb");
        if (f) {
            uint32_t r = 36 + (uint32_t)n * 2, d = (uint32_t)n * 2, sr = SR, br = SR * 2; uint16_t pcm = 1, ch = 1, ba = 2, bits = 16; uint32_t fsz = 16;
            fwrite("RIFF", 1, 4, f); fwrite(&r, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fsz, 4, 1, f);
            fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&sr, 4, 1, f); fwrite(&br, 4, 1, f); fwrite(&ba, 2, 1, f); fwrite(&bits, 2, 1, f);
            fwrite("data", 1, 4, f); fwrite(&d, 4, 1, f); fwrite(buf, 2, (size_t)n, f); fclose(f);
        }
        free(buf); return 0;
    }
    if (getenv("PS_LAUNCHER_ICON")) {               /* the app icon: a blank cartridge, the family's name */
        Game ic; memset(&ic, 0, sizeof ic);
        snprintf(ic.name, sizeof ic.name, "%s", s_header); snprintf(ic.id, sizeof ic.id, "icon");
        ic.have_rom = 1; ic.label_state = 3;
        build_cart(&ic);
        SDL_Surface *o = SDL_CreateRGBSurfaceWithFormatFrom(pix, CW, CH, 32, CW * 4, SDL_PIXELFORMAT_ARGB8888);
        SDL_SaveBMP(o, getenv("PS_LAUNCHER_ICON"));
        return 0;
    }
    if (getenv("PS_LAUNCHER_SHOT")) {               /* tests: draw the shelf once, save it, quit */
        SDL_WaitThread(s_labels, NULL); s_labels = NULL;
        for (int i = 0; i < NG; i++) { build_cart(&G[i]); disc_label(&G[i]); if (G[i].demo_state == 1) load_clip(&G[i]); }
        if (getenv("PS_LAUNCHER_CLIP")) { s_clip_fade = 1; s_clip_t = (float)atof(getenv("PS_LAUNCHER_CLIP")); }
        sel = getenv("PS_LAUNCHER_SEL") ? atoi(getenv("PS_LAUNCHER_SEL")) : 0;
        if (sel < 0 || sel >= NG) sel = 0;
        s_cur[s_colof[sel]] = s_rowof[sel]; s_vpos[s_colof[sel]] = (float)s_rowof[sel]; pos = (float)s_colof[sel];
        if (getenv("PS_LAUNCHER_PANEL")) s_panel = atoi(getenv("PS_LAUNCHER_PANEL"));
        if (getenv("PS_LAUNCHER_CHOOSE") && (s_disc ? disc_versions(s_colof[sel]) : build_choices(&G[sel])) > 1) s_choose = 0;
        s_noflip = 1; present(pos, 0.3f, sel, "");
        int ow, oh; SDL_GetRendererOutputSize(R, &ow, &oh);
        SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, ow, oh, 32, SDL_PIXELFORMAT_RGB888);
        SDL_RenderSetLogicalSize(R, 0, 0);
        SDL_RenderReadPixels(R, NULL, SDL_PIXELFORMAT_RGB888, s->pixels, s->pitch);
        SDL_SaveBMP(s, getenv("PS_LAUNCHER_SHOT"));
        return 0;
    }
    if (getenv("PS_LAUNCHER_TEST")) {               /* tests: every version started, A+B+C+Start back */
        long at = atol(getenv("PS_LAUNCHER_TEST")); if (at < 60) at = 600;
        char movie[1200]; snprintf(movie, sizeof movie, "%shome-test.movie", pref);
        FILE *mf = fopen(movie, "w");
        if (!mf) return 1;
        for (long f = 0; f < at + 30; f++) fputs(f < at ? "00\n" : "F0\n", mf);
        fclose(mf);
        int bad = 0;
        for (int i = 0; i < NG; i++) {
            Game *g = &G[i];
            if (g->soon || !g->have_rom || (*g->audio && !*g->audio_path)) {
                if (g->soon == 2) printf("%-12s %-36s found, starts %s\n", g->id, g->name, g->app);
                else printf("%-12s skipped (%s)\n", g->id, g->soon ? "coming soon" : !g->have_rom ? "no ROM" : "no soundtrack disc");
                continue;
            }
            char mv[1300], log[1300], aud[1200], frames[32];
            snprintf(mv, sizeof mv, "GENRECOMP_MOVIE=%s", movie);
            snprintf(log, sizeof log, "%shome-test-%s.log", pref, g->id);
            snprintf(aud, sizeof aud, "PS_CD_AUDIO=%s", g->audio_path);
            snprintf(frames, sizeof frames, "%ld", at + 600);
            const char *env[] = { mv, "PS_HOME=1", "PS_RENDERER=native", "PS_Z80=recomp", aud, NULL };
            const char *args[] = { "--headless", "--frames", frames, NULL };
            Uint32 t0 = SDL_GetTicks();
            run_game(g, args, env, log);
            char line[512]; int home = 0; FILE *lf = fopen(log, "r");
            while (lf && fgets(line, sizeof line, lf)) home |= strstr(line, "home: A+B+C+Start") != NULL;
            if (lf) fclose(lf);
            printf("%-12s %-36s %s (%.1f s)%s\n", g->id, g->name, home ? "started, back with A+B+C+Start" : "FAILED", (SDL_GetTicks() - t0) / 1000.0,
                   *g->audio_path ? "  [soundtrack from another disc]" : "");
            bad += !home;
        }
        return bad ? 1 : 0;
    }
    while (!quit) {
        SDL_Event e; int move = 0, vmove = 0, go = 0, click = -1;
        while (SDL_PollEvent(&e)) {
            if (running) { if (e.type == SDL_QUIT) quit = 1; continue; }   /* the game has the input */
            switch (e.type) {
            case SDL_QUIT: quit = 1; break;
            case SDL_JOYBUTTONDOWN: case SDL_JOYAXISMOTION:
                if (s_cap >= 0 && s_pad && e.jbutton.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(s_pad))) {
                    int c = -1;
                    if (e.type == SDL_JOYBUTTONDOWN && e.jbutton.button < 100) c = e.jbutton.button;
                    else if (e.type == SDL_JOYAXISMOTION && e.jaxis.value > 16000) {
                        int16_t rest = 0;
                        if (!(SDL_JoystickGetAxisInitialState(SDL_GameControllerGetJoystick(s_pad), e.jaxis.axis, &rest) && rest < -16000)) c = 100 + e.jaxis.axis;
                    }
                    int dup = 0; for (int i = 0; i < s_cap; i++) dup |= c >= 0 && s_cap_map[i] == c;
                    if (c >= 0 && !dup) { s_cap_map[s_cap++] = c; snd_move(); if (s_cap == 8) { pad_save(); s_cap = -1; snd_start(); } }
                }
                break;
            case SDL_KEYDOWN:
                if (s_choose >= 0) {                    /* the start chooser */
                    switch (e.key.keysym.sym) {
                    case SDLK_UP: s_choose = (s_choose + s_nch - 1) % s_nch; snd_move(); break;
                    case SDLK_DOWN: s_choose = (s_choose + 1) % s_nch; snd_move(); break;
                    case SDLK_RETURN: case SDLK_SPACE: case SDLK_z: case SDLK_KP_ENTER: if (!e.key.repeat) go = 2; break;
                    case SDLK_ESCAPE: case SDLK_BACKSPACE: s_choose = -1; s_vchoose = 0; snd_nope(); break;
                    }
                    break;
                }
                if (s_cap >= 0) {                       /* controller setup: Space skips, Esc cancels */
                    if (e.key.keysym.sym == SDLK_SPACE) { s_cap_map[s_cap++] = -1; if (s_cap == 8) { pad_save(); s_cap = -1; } }
                    else if (e.key.keysym.sym == SDLK_ESCAPE) { s_cap = -1; snd_nope(); }
                    break;
                }
                if (s_panel >= 0) {                     /* the settings panel has the keys */
                    switch (e.key.keysym.sym) {
                    case SDLK_UP: s_panel = (s_panel + PANEL_ROWS - 1) % PANEL_ROWS; snd_move(); break;
                    case SDLK_DOWN: s_panel = (s_panel + 1) % PANEL_ROWS; snd_move(); break;
                    case SDLK_LEFT: panel_change(-1); snd_move(); break;
                    case SDLK_RIGHT: case SDLK_RETURN: case SDLK_SPACE: case SDLK_z: panel_change(1); snd_move(); break;
                    case SDLK_ESCAPE: case SDLK_TAB: case SDLK_BACKSPACE: s_panel = -1; snd_nope(); break;
                    }
                    break;
                }
                switch (e.key.keysym.sym) {
                case SDLK_TAB: case SDLK_s: s_panel = 0; snd_start(); break;
                case SDLK_LEFT: case SDLK_a: move = -1; break;
                case SDLK_RIGHT: case SDLK_d: move = 1; break;
                case SDLK_UP: case SDLK_w: vmove = -1; break;
                case SDLK_DOWN: vmove = 1; break;
                case SDLK_RETURN: case SDLK_SPACE: case SDLK_z: case SDLK_KP_ENTER: if (!e.key.repeat) go = 1; break;
                case SDLK_ESCAPE: quit = 1; break;
                case SDLK_f: case SDLK_F11:
                    s_fullscreen = !s_fullscreen; SET.fullscreen = s_fullscreen; settings_save();
                    SDL_SetWindowFullscreen(Win, s_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0); break;
                }
                break;
            case SDL_CONTROLLERDEVICEADDED: { SDL_GameController *c = SDL_GameControllerOpen(e.cdevice.which); if (!s_pad) s_pad = c; } break;
            case SDL_CONTROLLERBUTTONDOWN:
                if (s_cap >= 0) break;                  /* raw buttons only while mapping */
                if (s_choose >= 0) {
                    int b = e.cbutton.button;
                    if (b == SDL_CONTROLLER_BUTTON_DPAD_UP) { s_choose = (s_choose + s_nch - 1) % s_nch; snd_move(); }
                    else if (b == SDL_CONTROLLER_BUTTON_DPAD_DOWN) { s_choose = (s_choose + 1) % s_nch; snd_move(); }
                    else if (b == SDL_CONTROLLER_BUTTON_A || b == SDL_CONTROLLER_BUTTON_START) go = 2;
                    else if (b == SDL_CONTROLLER_BUTTON_B) { s_choose = -1; s_vchoose = 0; snd_nope(); }
                    break;
                }
                if (s_panel >= 0) {
                    int b = e.cbutton.button;
                    if (b == SDL_CONTROLLER_BUTTON_DPAD_UP) { s_panel = (s_panel + PANEL_ROWS - 1) % PANEL_ROWS; snd_move(); }
                    else if (b == SDL_CONTROLLER_BUTTON_DPAD_DOWN) { s_panel = (s_panel + 1) % PANEL_ROWS; snd_move(); }
                    else if (b == SDL_CONTROLLER_BUTTON_DPAD_LEFT) { panel_change(-1); snd_move(); }
                    else if (b == SDL_CONTROLLER_BUTTON_DPAD_RIGHT || b == SDL_CONTROLLER_BUTTON_A) { panel_change(1); snd_move(); }
                    else if (b == SDL_CONTROLLER_BUTTON_B || b == SDL_CONTROLLER_BUTTON_Y || b == SDL_CONTROLLER_BUTTON_BACK) { s_panel = -1; snd_nope(); }
                    break;
                }
                if (e.cbutton.button == SDL_CONTROLLER_BUTTON_Y || e.cbutton.button == SDL_CONTROLLER_BUTTON_BACK) { s_panel = 0; snd_start(); break; }
                if (e.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_LEFT) { move = -1; hold = -1; hold_t = SDL_GetTicks() + 380; }
                if (e.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) { move = 1; hold = 1; hold_t = SDL_GetTicks() + 380; }
                if (e.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_UP) vmove = -1;
                if (e.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN) vmove = 1;
                if (e.cbutton.button == SDL_CONTROLLER_BUTTON_A || e.cbutton.button == SDL_CONTROLLER_BUTTON_START) go = 1;
                break;
            case SDL_CONTROLLERBUTTONUP:
                if (e.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_LEFT || e.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) hold = 0;
                break;
            case SDL_CONTROLLERAXISMOTION:
                if (e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX) {
                    int d = e.caxis.value < -16000 ? -1 : e.caxis.value > 16000 ? 1 : 0;
                    if (d && d != hold) { move = d; hold_t = SDL_GetTicks() + 380; }
                    hold = d;
                } else if (e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) {
                    static int vhold;
                    int d = e.caxis.value < -16000 ? -1 : e.caxis.value > 16000 ? 1 : 0;
                    if (d && d != vhold) vmove = d;
                    vhold = d;
                }
                break;
            case SDL_MOUSEWHEEL: move = e.wheel.y > 0 || e.wheel.x < 0 ? -1 : e.wheel.y < 0 || e.wheel.x > 0 ? 1 : 0; break;
            case SDL_MOUSEBUTTONDOWN:
                if (e.button.button == SDL_BUTTON_LEFT && s_disc) {
                    float lx, ly; SDL_RenderWindowToLogical(R, e.button.x, e.button.y, &lx, &ly);
                    for (int c = 0; c < s_ncol; c++) {
                        SDL_FRect r; float dm;
                        if (disc_style()) modern_card(c, pos, &r, &dm); else disc_card(c, &r);
                        if (lx >= r.x && lx < r.x + r.w && ly >= r.y && ly < r.y + r.h) click = s_cell[c][0];
                    }
                } else if (e.button.button == SDL_BUTTON_LEFT) {
                    for (int i = 0; i < NG; i++) {
                        SDL_FRect r; float dim; cart_place(i, pos, t, sel, &r, &dim);
                        if (e.button.x >= r.x && e.button.x < r.x + r.w && e.button.y >= r.y && e.button.y < r.y + r.h &&
                            fabsf(cart_vdist(i)) < 2.2f &&
                            (click < 0 || fabsf(s_colof[i] - pos) * 4 + fabsf(cart_vdist(i)) < fabsf(s_colof[click] - pos) * 4 + fabsf(cart_vdist(click)))) click = i;
                    }
                }
                break;
            }
        }
        if (!running && hold && SDL_GetTicks() > hold_t) { move = hold; hold_t = SDL_GetTicks() + 140; }
        if (click >= 0) { if (click == sel) go = 1; else { sel = click; s_cur[s_colof[sel]] = s_rowof[sel]; snd_move(); } }
        if (s_panel >= 0 || s_choose >= 0) move = vmove = 0;
        if (s_disc && (move || vmove)) {                /* the disc look: a grid of cards, four a row */
            int c = s_colof[sel] + move + (disc_style() ? 0 : vmove * 4);   /* MODERN: one row */
            if (c >= 0 && c < s_ncol) { sel = s_cell[c][0]; snd_move(); } else snd_nope();
            move = vmove = 0;
        }
        if (s_disc && go == 1 && !running && s_panel < 0 && s_choose < 0) {
            int c = s_colof[sel];
            if (G[sel].soon == 1) { snd_nope(); snprintf(msg, sizeof msg, "%s: COMING SOON", G[sel].name); msg_until = SDL_GetTicks() + 3000; go = 0; }
            else {                                      /* which version: only those that can start */
                disc_versions(c);
                if (s_nch > 1) { s_vchoose = 1; s_choose = 0; snd_move(); go = 0; }
                else if (s_nch == 1) sel = s_ch[0].game;
            }
        }
        if (go == 2 && s_vchoose) { sel = s_ch[s_choose].game; s_vchoose = 0; s_choose = -1; go = 1; }
        if (move) {                                     /* another game: the version shown in its column */
            int c = s_colof[sel] + move;
            if (c >= 0 && c < s_ncol) { sel = s_cell[c][s_cur[c]]; snd_move(); } else snd_nope();
        }
        if (vmove) {                                    /* another version of this game */
            int c = s_colof[sel], r = s_rowof[sel] + vmove;
            if (r >= 0 && r < s_nrow[c]) { s_cur[c] = r; sel = s_cell[c][r]; snd_move(); } else snd_nope();
        }
        if (go == 1 && !running && s_panel < 0 && s_choose < 0 && G[sel].have_rom && build_choices(&G[sel]) > 1) {
            s_choose = 0; s_vchoose = 0; snd_move(); go = 0;            /* continue or a save: ask first */
        }
        if (go == 2) { snprintf(G[sel].resume, sizeof G[sel].resume, "%s", s_ch[s_choose].resume); s_choose = -1; go = 1; }
        else if (go == 1) *G[sel].resume = 0;
        if (go && !running && s_panel < 0) {
            Game *g = &G[sel];
            if (!g->have_rom && ask_rom(g)) { g->label_state = 0; build_cart(g); start_labels(); }
            if (!g->have_rom) {
                snd_nope();
#ifndef __APPLE__
                snprintf(msg, sizeof msg, "PUT %s IN %s", g->romname, g->store);
#else
                snprintf(msg, sizeof msg, "NO ROM CHOSEN");
#endif
                msg_until = SDL_GetTicks() + 4000;
            } else {
                snd_start();
                Uint32 t0 = SDL_GetTicks(), until = t0 + 650;    /* the jingle, the cartridge going in */
                while (SDL_GetTicks() < until) {
                    s_launch = (SDL_GetTicks() - t0) / 650.0f;
                    t += 0.016f; present(pos, t, sel, "LOADING...");
                    SDL_PumpEvents();
                }
                SDL_HideWindow(Win);
                s_music_paused = 1; s_launch = 0;
                s_playing = 1; running = 1; s_play_game = g;
                SDL_DetachThread(SDL_CreateThread(play_thread, "play", g));
            }
        }
        if (running) {
            if (!s_playing) {
                running = 0;
                SDL_ShowWindow(Win); SDL_RaiseWindow(Win);
                s_music_paused = 0;
                SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
                last = SDL_GetTicks();
            } else { SDL_Delay(50); continue; }
        }
        for (int i = 0; i < NG; i++)                    /* labels captured in the background */
            if (G[i].label_state == 1 || (G[i].label_state == 3 && G[i].cart)) {
                int st = G[i].label_state; build_cart(&G[i]); if (st == 3) G[i].label_state = 4;
            }
        if (s_disc) for (int i = 0; i < NG; i++) disc_label(&G[i]);
        for (int i = 0; i < NG; i++) if (G[i].demo_state == 1) load_clip(&G[i]);
        Uint32 now = SDL_GetTicks(); float dt = (now - last) / 1000.0f; last = now;
        if (dt > 0.1f) dt = 0.1f;
        t += dt;
        {   /* the clip fades in once the cartridge has stayed selected a moment */
            static int was = -1; static float still;
            if (sel != was) { was = sel; still = 0; s_clip_t = 0; }
            still += dt;
            float target = still > 1.2f ? 1.0f : 0.0f;
            s_clip_fade += (target - s_clip_fade) * (1 - expf(-dt * (target > 0 ? 3.0f : 14.0f)));
            if (still > 1.2f) s_clip_t += dt;
        }
        pos += (s_colof[sel] - pos) * (1 - expf(-dt * 12));
        for (int c = 0; c < s_ncol; c++) s_vpos[c] += (s_cur[c] - s_vpos[c]) * (1 - expf(-dt * 12));
        if (msg_until && now > msg_until) { *msg = 0; msg_until = 0; }
        present(pos, t, sel, msg);
    }
    if (s_labels) SDL_WaitThread(s_labels, NULL);
    SDL_Quit();
    return 0;
}
