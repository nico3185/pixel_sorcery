/* ps_menu.c — in-game settings menu (F1 / Tab, or the gamepad's Back/Guide button).
 *
 * Aspect ratio, widescreen enemy activation, low-latency pacing, volume, fullscreen, the
 * title's cheats (recomp.json "cheats", e.g. Game Genie code patches) and RAM toggles the
 * title registers (e.g. Action Replay codes). The game is paused while the menu is open.
 * Settings persist in ~/Library/Application Support/PixelSorcery/<title>/settings.ini;
 * headless runs (tests, replays) ignore them so they stay deterministic.
 */
#include "genrecomp/ps_menu.h"
#include "genrecomp/ps_wide.h"
#include "genrecomp/platform.h"
#include "genrecomp/bus.h"
#include "genrecomp/input.h"
#include "ps_portable.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ------------------------------------------------------------------ 5x7 font (own design) */
static const char k_chars[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789:./-<>%()";
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
};

static void draw_text(uint32_t *fb, int pitch, int h, int x, int y, const char *s, uint32_t col) {
    for (; *s; s++, x += 6) {
        char c = (*s >= 'a' && *s <= 'z') ? (char)(*s - 32) : *s;
        const char *p = strchr(k_chars, c);
        int g = p ? (int)(p - k_chars) : 0;
        for (int r = 0; r < 7; r++)
            for (int b = 0; b < 5; b++)
                if (k_font[g][r] & (16 >> b)) {
                    int px = x + b, py = y + r;
                    if (px >= 0 && px < pitch && py >= 0 && py < h) fb[py * pitch + px] = col;
                }
    }
}

/* ------------------------------------------------------------------ settings */
static const char *k_aspects[] = { "4:3", "16:10", "16:9", "21:9", "32:9" };
static struct {
    int aspect, wide_objects, low_latency, volume, fullscreen;
    uint32_t cheats, toggles;
    int water;
} S = { 2, 1, 1, 100, 0, 0, 0, 1 };

/* weak defaults: a title's generated recomp_cheats.c provides the real table */
#if defined(_MSC_VER)
extern const char *ps_cheat_names[];
const char *ps_cheat_names_ps_default[] = { 0 };
PS_ALTNAME(ps_cheat_names, ps_cheat_names_ps_default)
extern int ps_cheat_count;
int ps_cheat_count_ps_default = 0;
PS_ALTNAME(ps_cheat_count, ps_cheat_count_ps_default)
#else
__attribute__((weak)) const char *ps_cheat_names[] = { 0 };
__attribute__((weak)) int ps_cheat_count = 0;
#endif
int ps_cheat(int n) { return (S.cheats >> n) & 1; }

/* addr < $400000: a ROM word patch (Game Genie data code), applied from frame 300 so
 * the game's boot checksum still reads the original, restored when switched off */
typedef struct { const char *name; uint32_t addr; uint16_t value; bool word; int applied; uint16_t orig; bool (*gate)(void); } Toggle;
static Toggle s_toggles[16];
static int s_ntoggles;
static const char *s_title = "game";
static bool s_open, s_loaded;

/* Battery save RAM (ps_hw.c, native builds): loaded before the game runs. Live play uses
 * <title>/save.srm and writes it back when it changes; a recorded session keeps the save RAM
 * it booted with beside its .polls (.srm), so replays and LOAD STATE (PS_RESUME, which takes
 * the slot's copy) start from the same contents. */
#if defined(_MSC_VER)
uint8_t *ps_hw_sram(bool *present);
uint8_t *ps_hw_sram_ps_default(bool *present) { if (present) *present = false; return NULL; }
PS_ALTNAME(ps_hw_sram, ps_hw_sram_ps_default)
#else
__attribute__((weak)) uint8_t *ps_hw_sram(bool *present) { if (present) *present = false; return NULL; }   /* no native layer (tools, oracle) */
#endif
static uint8_t s_sram_boot[0x10000], s_sram_disk[0x10000];
static bool s_sram_live;
static void sidecar_path(const char *polls, const char *ext, char *out, size_t n) {
    snprintf(out, n, "%s", polls);
    char *c = strrchr(out, ':'); if (c && c > strrchr(out, '/')) *c = 0;      /* PS_RESUME=file:N */
    char *d = strrchr(out, '.'); if (d && !strcmp(d, ".polls")) *d = 0;
    strncat(out, ext, n - strlen(out) - 1);
}
static void sram_save_path(char *out, size_t n) {
    const char *home = getenv("HOME");
    snprintf(out, n, "%s/Library/Application Support/PixelSorcery/%s/save.srm", home ? home : ".", s_title);
}
static bool read_file(const char *path, uint8_t *buf, size_t n) {
    FILE *f = fopen(path, "rb"); if (!f) return false;
    bool ok = fread(buf, 1, n, f) == n; fclose(f); return ok;
}
static void write_file(const char *path, const uint8_t *buf, size_t n) {
    char tmp[1100]; snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb"); if (!f) return;
    bool ok = fwrite(buf, 1, n, f) == n; ok = fclose(f) == 0 && ok;
    if (ok) rename(tmp, path); else remove(tmp);
}
static void sram_boot(void) {
    bool present = false;
    uint8_t *sram = ps_hw_sram(&present);
    if (!sram || !present) return;
    const char *src = getenv("PS_RESUME"); if (!src || !*src) src = getenv("PS_POLL_REPLAY");
    char path[1100];
    if (src && *src) { sidecar_path(src, ".srm", path, sizeof path); read_file(path, sram, 0x10000); }
    else if (!platform_is_headless()) { sram_save_path(path, sizeof path); read_file(path, sram, 0x10000); }
    memcpy(s_sram_boot, sram, sizeof s_sram_boot);
    memcpy(s_sram_disk, sram, sizeof s_sram_disk);
    s_sram_live = !platform_is_headless() && !(getenv("PS_POLL_REPLAY") && *getenv("PS_POLL_REPLAY"));
    const char *rec = getenv("PS_POLL_RECORD");
    if (rec && *rec) { sidecar_path(rec, ".srm", path, sizeof path); write_file(path, s_sram_boot, sizeof s_sram_boot); }
}
static void sram_flush(void) {                       /* every second, when the game changed it */
    extern long ps_frames;
    if (!s_sram_live || ps_frames % 60) return;
    uint8_t *sram = ps_hw_sram(NULL);
    if (!memcmp(sram, s_sram_disk, sizeof s_sram_disk)) return;
    memcpy(s_sram_disk, sram, sizeof s_sram_disk);
    char path[1100]; sram_save_path(path, sizeof path);
    char dir[1100]; snprintf(dir, sizeof dir, "%s", path);
    for (char *q = dir + 1; *q; q++) if (*q == '/') { *q = 0; mkdir(dir, 0755); *q = '/'; }
    write_file(path, s_sram_disk, sizeof s_sram_disk);
}
void ps_menu_set_title(const char *title) { s_title = title; sram_boot(); }
void ps_menu_add_toggle(const char *name, uint32_t addr, uint16_t value, bool word) {
    if (s_ntoggles < 16) s_toggles[s_ntoggles++] = (Toggle){ name, addr, value, word, 0, 0, NULL };
}
void ps_menu_set_toggle_gate(const char *name, bool (*gate)(void)) {
    for (int i = 0; i < s_ntoggles; i++) if (!strcmp(s_toggles[i].name, name)) s_toggles[i].gate = gate;
}

static void ini_path(char *out, size_t n) {
    const char *home = getenv("HOME");
    snprintf(out, n, "%s/Library/Application Support/PixelSorcery/%s/settings.ini", home ? home : ".", s_title);
}

static void apply(void) {
    static const int num[] = { 4, 16, 16, 21, 32 }, den[] = { 3, 10, 9, 9, 9 };
    ps_wide_set_aspect(num[S.aspect], den[S.aspect]);
    ps_wide_set_objects(S.wide_objects);
    platform_set_frame_delay(S.low_latency);
    platform_set_volume(S.volume);
    platform_set_fullscreen(S.fullscreen);
    { void ps_render_set_water(bool on); if (!getenv("PS_WATER")) ps_render_set_water(S.water); }
}

static void save(void) {
    char path[1024]; ini_path(path, sizeof path);
    char dir[1024]; snprintf(dir, sizeof dir, "%s", path);
    for (char *p = dir + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(dir, 0755); *p = '/'; }
    FILE *f = fopen(path, "w"); if (!f) return;
    fprintf(f, "aspect=%d\nwide_objects=%d\nlow_latency=%d\nvolume=%d\nfullscreen=%d\ncheats=%u\ntoggles=%u\nwater=%d\n",
            S.aspect, S.wide_objects, S.low_latency, S.volume, S.fullscreen, S.cheats, S.toggles, S.water);
    fclose(f);
}

/* ------------------------------------------------------- versioned sessions
 * Settings that change game logic (widescreen enemies, cheats, RAM toggles) are part of a
 * run: a session SESSION.polls gets SESSION.meta with the build version and every logic
 * setting change keyed by frame ("logic FRAME OBJ CHEATS TOGGLES"). Replaying or resuming
 * a session applies them at the same frames, whatever the player's current settings are
 * (no .meta: an older session, all off). Applied at the top of ps_menu_run, where live
 * menu changes take effect. */
#ifndef PS_VERSION
#define PS_VERSION "unknown"
#endif
extern long ps_frames;
extern int ps_poll_fast_forward(void);
static FILE *s_meta;
static struct { long frame; int obj; uint32_t cheats, toggles; int aspect; } s_ev[256];
static int s_nev, s_iev, s_forced;           /* forced: replaying, logic from the session */
static int s_user_obj, s_user_aspect = 2; static uint32_t s_user_cheats, s_user_toggles;
static void set_aspect(int i) {
    static const int num[] = { 4, 16, 16, 21, 32 }, den[] = { 3, 10, 9, 9, 9 };
    ps_wide_set_aspect(num[i % 5], den[i % 5]);
}

static void logic_log(void) {
    if (s_meta) { fprintf(s_meta, "logic %ld %d %u %u %d\n", ps_frames, S.wide_objects, S.cheats, S.toggles, S.aspect); fflush(s_meta); }
}
static void meta_path(const char *polls, char *out, size_t n) {
    snprintf(out, n, "%s", polls);
    char *c = strrchr(out, ':'); if (c && c > strrchr(out, '/')) *c = 0;      /* PS_RESUME=file:N */
    char *d = strrchr(out, '.'); if (d && !strcmp(d, ".polls")) *d = 0;
    strncat(out, ".meta", n - strlen(out) - 1);
}
static void session_init(void) {
    const char *src = getenv("PS_RESUME"); if (!src || !*src) src = getenv("PS_POLL_REPLAY");
    char path[1024];
    if (src && *src) {
        s_forced = 1;
        meta_path(src, path, sizeof path);
        FILE *f = fopen(path, "r");
        if (f) {
            char line[256];
            while (fgets(line, sizeof line, f) && s_nev < 256) {
                s_ev[s_nev].aspect = 0;            /* older .meta: aspect not logged; was 4:3 */
                if (sscanf(line, "logic %ld %d %u %u %d", &s_ev[s_nev].frame, &s_ev[s_nev].obj,
                           &s_ev[s_nev].cheats, &s_ev[s_nev].toggles, &s_ev[s_nev].aspect) >= 4) s_nev++;
            }
            fclose(f);
        }
        if (!s_nev) s_nev = 1;               /* older session: logic settings all off */
        fprintf(stderr, "session: %d logic setting change(s) from %s\n", s_nev, f ? path : "(no .meta: all off)");
    }
    const char *lg = getenv("PS_LOGIC");          /* tests: PS_LOGIC=OBJ:CHEATS:TOGGLES */
    if (!s_forced && lg && sscanf(lg, "%d:%u:%u", &s_ev[0].obj, &s_ev[0].cheats, &s_ev[0].toggles) == 3) {
        s_forced = 1; s_nev = 1;
    }
    const char *rec = getenv("PS_POLL_RECORD");
    if (rec && *rec) {
        meta_path(rec, path, sizeof path);
        if ((s_meta = fopen(path, "w")))
            fprintf(s_meta, "version %s\ntitle %s\n", PS_VERSION, s_title ? s_title : "?");
    }
}
/* top of every frame's menu step: replayed setting changes, then the hand-over to live */
static void logic_tick(void) {
    while (s_forced && s_iev < s_nev && s_ev[s_iev].frame <= ps_frames) {
        S.wide_objects = s_ev[s_iev].obj; S.cheats = s_ev[s_iev].cheats; S.toggles = s_ev[s_iev].toggles;
        S.aspect = s_ev[s_iev].aspect;
        s_iev++; ps_wide_set_objects(S.wide_objects); if (S.wide_objects) set_aspect(S.aspect); logic_log();
    }
    if (s_forced && getenv("PS_RESUME") && !ps_poll_fast_forward()) {     /* resumed: player's settings */
        s_forced = 0;
        S.wide_objects = s_user_obj; S.cheats = s_user_cheats; S.toggles = s_user_toggles; S.aspect = s_user_aspect;
        ps_wide_set_objects(S.wide_objects); if (S.wide_objects) set_aspect(S.aspect); logic_log();
        fprintf(stderr, "session: live, logic settings from settings.ini\n");
    }
}

void ps_menu_load(void) {
    static bool s_session;
    if (!s_session) { s_session = true; session_init(); if (!s_forced) logic_log(); }
    if (s_loaded || platform_is_headless()) return;   /* tests and replays: defaults only */
    s_loaded = true;
    char path[1024]; ini_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (f) {
        char k[64]; long v;
        while (fscanf(f, "%63[^=]=%ld\n", k, &v) == 2) {
            if (!strcmp(k, "aspect")) S.aspect = (int)v % 5;
            else if (!strcmp(k, "wide_objects")) S.wide_objects = (int)v;
            else if (!strcmp(k, "low_latency")) S.low_latency = (int)v;
            else if (!strcmp(k, "volume")) S.volume = (int)v;
            else if (!strcmp(k, "fullscreen")) S.fullscreen = (int)v;
            else if (!strcmp(k, "cheats")) S.cheats = (uint32_t)v;
            else if (!strcmp(k, "toggles")) S.toggles = (uint32_t)v;
            else if (!strcmp(k, "water")) S.water = (int)v;
        }
        fclose(f);
    }
    s_user_obj = S.wide_objects; s_user_cheats = S.cheats; s_user_toggles = S.toggles; s_user_aspect = S.aspect;
    if (s_forced) { S.wide_objects = s_ev[0].obj; S.cheats = s_ev[0].cheats; S.toggles = s_ev[0].toggles; S.aspect = s_ev[0].aspect; }
    else logic_log();
    if (getenv("PS_WIDE")) return;                 /* explicit environment wins */
    apply();
}

/* RAM toggles (Action Replay style) are re-applied every frame while on */
void ps_menu_frame_pokes(void) {
    extern long ps_frames;
    sram_flush();
    for (int i = 0; i < s_ntoggles; i++) {
        Toggle *t = &s_toggles[i];
        if (t->addr >= 0x400000) continue;
        bool on = (S.toggles >> i & 1) && ps_frames >= 300;
        if (on != (t->applied != 0)) {
            extern uint16_t ps_hw_rom_patch16(uint32_t addr, uint16_t value);
            uint16_t old = ps_hw_rom_patch16(t->addr, on ? t->value : t->orig);
            if (on) t->orig = old;
            t->applied = on;
        }
    }
    for (int i = 0; i < s_ntoggles; i++)
        if ((S.toggles & (1u << i)) && s_toggles[i].addr >= 0x400000 && (!s_toggles[i].gate || s_toggles[i].gate())) {
            if (s_toggles[i].word) bus_write16(s_toggles[i].addr, s_toggles[i].value);
            else bus_write8(s_toggles[i].addr, (uint8_t)s_toggles[i].value);
        }
}

/* ------------------------------------------------------------------ menu loop */
void ps_menu_request(void) { s_open = true; }

enum { IT_ASPECT, IT_OBJ, IT_LAT, IT_VOL, IT_FULL, IT_REND, IT_WATER, IT_FIXED };

static int item_count(void) { return IT_FIXED + ps_cheat_count + s_ntoggles + 6; }
/* SAVE / LOAD: a save is a machine snapshot (ps_state.c, slotN.state, taken at the next
 * frame's VBlank point) plus the session's inputs up to it (per controller poll) and its
 * setting log. LOAD restores the snapshot in place, instantly; a snapshot of another ROM,
 * build layout or version (or none) falls back to powering on again and replaying the
 * inputs at full speed (PS_RESUME), then handing over at that exact moment (both exact:
 * side-by-side checked). */
static int s_slot = 1;
static char s_note[48];
static void slot_path(int k, const char *ext, char *out, size_t n) {
    const char *home = getenv("HOME");
    snprintf(out, n, "%s/Library/Application Support/PixelSorcery/%s/saves/slot%d.%s", home ? home : ".", s_title, k, ext);
}
static long slot_polls(int k) {
    char p[1100]; slot_path(k, "polls", p, sizeof p);
    FILE *f = fopen(p, "r"); if (!f) return -1;
    long n = 0; int c; while ((c = fgetc(f)) != EOF) n += c == '\n';
    fclose(f); return n;
}

static void item_text(int i, char *out, size_t n) {
    if (i == IT_ASPECT) snprintf(out, n, "ASPECT RATIO        < %s >", k_aspects[S.aspect]);
    else if (i == IT_OBJ) snprintf(out, n, "WIDESCREEN ENEMIES  %s", S.wide_objects ? "ON" : "OFF");
    else if (i == IT_LAT) snprintf(out, n, "LOW LATENCY         %s", S.low_latency ? "ON" : "OFF");
    else if (i == IT_VOL) snprintf(out, n, "VOLUME              < %d%% >", S.volume);
    else if (i == IT_FULL) snprintf(out, n, "FULLSCREEN          %s", S.fullscreen ? "ON" : "OFF");
    else if (i == IT_WATER) { bool ps_render_water_allowed(void);
        snprintf(out, n, "MODERN WATER        %s", !ps_render_water_allowed() ? "N/A" : S.water ? "ON" : "OFF"); }
    else if (i == IT_REND) {
#ifdef __APPLE__
        int pref = platform_renderer_pref();
        snprintf(out, n, "RENDERER            < %s >%s", pref ? "METAL" : "OPENGL",
                 pref != platform_renderer_active() ? " RESTART" : "");
#else
        snprintf(out, n, "RENDERER            SDL");
#endif
    }
    else if (i < IT_FIXED + ps_cheat_count) {
        int c = i - IT_FIXED; char name[40]; snprintf(name, sizeof name, "%s", ps_cheat_names[c]);
        for (char *p = name; *p; p++) if (*p == '_') *p = ' ';
        snprintf(out, n, "%-19s %s", name, ps_cheat(c) ? "ON" : "OFF");
    } else if (i < IT_FIXED + ps_cheat_count + s_ntoggles) {
        int t = i - IT_FIXED - ps_cheat_count;
        snprintf(out, n, "%-19s %s", s_toggles[t].name, (S.toggles >> t) & 1 ? "ON" : "OFF");
    } else if (i == item_count() - 6) {
        char pn[40]; bool have = input_pad_name(pn, sizeof pn);
        snprintf(out, n, "CONTROLLER SETUP    %s", have ? "" : "(NO GAMEPAD)");
    } else if (i == item_count() - 5) {
        long p = slot_polls(s_slot);
        snprintf(out, n, "SAVE STATE          < SLOT %d >%s", s_slot, s_note[0] && s_note[0] == 'S' ? " SAVED" : "");
        (void)p;
    } else if (i == item_count() - 4) {
        long p = slot_polls(s_slot);
        snprintf(out, n, "LOAD STATE          < SLOT %d >%s", s_slot, p < 0 ? " EMPTY" : "");
    } else if (i == item_count() - 3) snprintf(out, n, "RESUME");
    else if (i == item_count() - 2) snprintf(out, n, "RESET");
    else snprintf(out, n, "QUIT GAME");
}

static void change(int i, int dir) {
    if (i == item_count() - 6) return;
    if (i == item_count() - 5 || i == item_count() - 4) { s_slot = (s_slot - 1 + dir + 4) % 4 + 1; s_note[0] = 0; return; }
    if (s_forced && (i == IT_OBJ || i == IT_ASPECT || (i >= IT_FIXED && i < IT_FIXED + ps_cheat_count + s_ntoggles))) return;
    if (i == IT_ASPECT) S.aspect = (S.aspect + dir + 5) % 5;
    else if (i == IT_OBJ) S.wide_objects ^= 1;
    else if (i == IT_LAT) S.low_latency ^= 1;
    else if (i == IT_VOL) { S.volume += dir * 10; if (S.volume < 0) S.volume = 0; if (S.volume > 100) S.volume = 100; }
    else if (i == IT_FULL) S.fullscreen ^= 1;
    else if (i == IT_WATER) S.water ^= 1;
    else if (i == IT_REND) {
#ifdef __APPLE__
        platform_set_renderer_pref(!platform_renderer_pref());
#endif
    }
    else if (i < IT_FIXED + ps_cheat_count) S.cheats ^= 1u << (i - IT_FIXED);
    else if (i < IT_FIXED + ps_cheat_count + s_ntoggles) S.toggles ^= 1u << (i - IT_FIXED - ps_cheat_count);
    if (i == IT_OBJ || i == IT_ASPECT || i >= IT_FIXED) { s_user_obj = S.wide_objects; s_user_cheats = S.cheats; s_user_toggles = S.toggles; s_user_aspect = S.aspect; logic_log(); }
    apply();
}

/* RESET: a fresh power-on of the same program (the recompiled game's call stack cannot
 * be unwound in place). The new run records to <session>-resetN.polls. */
#ifdef __APPLE__
#include <crt_externs.h>
#endif
#ifndef _WIN32
#include <unistd.h>
#else
#include <process.h>
#define setenv(k, v, o) _putenv_s(k, v)
#define unsetenv(k) _putenv_s(k, "")
#define execv _execv
#endif
static void ps_restart_resume(const char *resume);
static void ps_restart(void) { ps_restart_resume(NULL); }
static void ps_restart_resume(const char *resume) {
    char **argv;
#ifdef __APPLE__
    argv = *_NSGetArgv();
#elif defined(_WIN32)
    argv = __argv;
#else
    extern char **environ; static char *fallback[2]; argv = fallback;   /* no argv access: quit */
    save(); exit(0);
#endif
    const char *rec = getenv("PS_POLL_RECORD");
    if (rec && *rec) {
        static char path[1024]; static int n;
        char base[1024]; snprintf(base, sizeof base, "%s", rec);
        char *d = strrchr(base, '.'); if (d && !strcmp(d, ".polls")) *d = 0;
        char *r = strstr(base, "-reset"); if (r) { n = atoi(r + 6); *r = 0; }
        snprintf(path, sizeof path, "%s-reset%d.polls", base, n + 1);
        setenv("PS_POLL_RECORD", path, 1);
    }
    if (resume) setenv("PS_RESUME", resume, 1); else unsetenv("PS_RESUME");
    if (s_meta) fclose(s_meta);
    platform_shutdown();
    execv(argv[0], argv);
    exit(0);                                       /* exec failed */
}

static void copy_lines(const char *src, const char *dst, long lines) {
    FILE *in = fopen(src, "r"), *out = fopen(dst, "w");
    if (in && out) { char l[256]; long k = 0; while ((lines < 0 || k < lines) && fgets(l, sizeof l, in)) { fputs(l, out); k += strchr(l, '\n') != NULL; } }
    if (in) fclose(in);
    if (out) fclose(out);
}
/* the snapshot taken for SAVE STATE: the slot's inputs are cut at its poll so a replay
 * (the fallback) reaches the same moment */
static int s_state_slot;
static void state_taken(const uint8_t *b, size_t n, long poll) {
    const char *rec = getenv("PS_POLL_RECORD");
    char p[1100]; slot_path(s_state_slot, "state", p, sizeof p);
    write_file(p, b, n);
    if (rec && *rec) { void ps_poll_flush(void); ps_poll_flush(); slot_path(s_state_slot, "polls", p, sizeof p); copy_lines(rec, p, poll); }
    fprintf(stderr, "menu: slot %d state saved at poll %ld\n", s_state_slot, poll);
}
static void save_slot(void) {
    const char *rec = getenv("PS_POLL_RECORD");
    if (!rec || !*rec) { snprintf(s_note, sizeof s_note, "NO RECORDING"); return; }
    void ps_poll_flush(void); long ps_poll_index(void);
    ps_poll_flush();
    if (s_meta) fflush(s_meta);
    char p[1100], m[1100], dir[1100];
    slot_path(s_slot, "polls", p, sizeof p); slot_path(s_slot, "meta", m, sizeof m);
    snprintf(dir, sizeof dir, "%s", p);
    for (char *q = dir + 1; *q; q++) if (*q == '/') { *q = 0; mkdir(dir, 0755); *q = '/'; }
    copy_lines(rec, p, ps_poll_index());
    {   /* the snapshot comes at the next frame; an older one must not be loaded meanwhile */
        void ps_state_request(void (*)(const uint8_t *, size_t, long));
        char st[1100]; slot_path(s_slot, "state", st, sizeof st); remove(st);
        s_state_slot = s_slot; ps_state_request(state_taken);
    }
    char rm[1100]; meta_path(rec, rm, sizeof rm);
    copy_lines(rm, m, -1);
    { bool present = false;                       /* the save RAM this session booted with */
      if (ps_hw_sram(&present) && present) { char sp[1100]; slot_path(s_slot, "srm", sp, sizeof sp); write_file(sp, s_sram_boot, sizeof s_sram_boot); } }
    snprintf(s_note, sizeof s_note, "SAVED");
}
static void load_slot(void) {
    long n = slot_polls(s_slot);
    if (n < 0) return;
    char p[1100], r[1200]; slot_path(s_slot, "polls", p, sizeof p);
    {   /* the snapshot, restored in place; the session's recording continues from its inputs */
        uint8_t *ps_state_read_file(const char *, size_t *);
        const char *ps_state_check(const uint8_t *, size_t), *ps_state_load_now(uint8_t *, size_t);
        char st[1100]; slot_path(s_slot, "state", st, sizeof st);
        size_t sn; uint8_t *b = ps_state_read_file(st, &sn);
        const char *why = b ? ps_state_check(b, sn) : "no snapshot";
        if (!why) {
            const char *rec = getenv("PS_POLL_RECORD");
            if (rec && *rec) {
                void ps_poll_record_close(void), ps_poll_record_reopen(void);
                ps_poll_record_close(); copy_lines(p, rec, n); ps_poll_record_reopen();
            }
            save(); s_open = false;
            why = ps_state_load_now(b, sn);           /* returns only if refused */
            b = NULL;
        }
        free(b);
        fprintf(stderr, "menu: slot %d: %s, replaying its inputs instead\n", s_slot, why);
    }
    snprintf(r, sizeof r, "%s:%ld", p, n);
    save();
    ps_restart_resume(r);
}

/* CONTROLLER SETUP: press the pad's button for A, B, C, START, X, Y, Z in turn (raw buttons,
 * so any pad's own numbering works); keyboard Return keeps a button as it is, Esc cancels. */
static void pad_setup(uint8_t *frame, int width, uint32_t *buf) {
    char pn[40];
    if (!input_pad_name(pn, sizeof pn)) return;
    static const char *names[7] = { "A", "B", "C", "START", "X", "Y", "Z" };
    int m[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };
    for (int k = 0; k < 7; k++) {
        bool released = false, done = false, prev_ret = true;
        while (!done) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) if (ev.type == SDL_QUIT) { save(); exit(0); }
            const uint8_t *ks = SDL_GetKeyboardState(NULL);
            if (ks[SDL_SCANCODE_ESCAPE]) { while (SDL_GetKeyboardState(NULL)[SDL_SCANCODE_ESCAPE]) { SDL_PumpEvents(); SDL_Delay(10); } return; }
            bool ret = ks[SDL_SCANCODE_RETURN];
            if (ret && !prev_ret) done = true;                 /* skip: keep the default */
            prev_ret = ret;
            int r = input_raw_pressed();
            if (r < 0) released = true;
            else if (released) {
                bool dup = false; for (int i = 0; i < k; i++) dup |= m[i] == r;
                if (!dup) { m[k] = r; done = true; }
            }
            const uint32_t *src = (const uint32_t *)frame;
            for (int i = 0; i < width * GEN_RENDER_HEIGHT; i++) buf[i] = (src[i] >> 2) & 0x3F3F3F;
            int x0 = width / 2 - 100, y0 = 60;
            char t[80];
            draw_text(buf, width, GEN_RENDER_HEIGHT, x0, y0 - 16, "CONTROLLER SETUP", 0xFFE070);
            for (int c = 0; pn[c]; c++) if (pn[c] >= 'a' && pn[c] <= 'z') pn[c] -= 32;
            draw_text(buf, width, GEN_RENDER_HEIGHT, x0, y0, pn, 0x9090A0);
            snprintf(t, sizeof t, "PRESS THE BUTTON FOR  %s", names[k]);
            draw_text(buf, width, GEN_RENDER_HEIGHT, x0, y0 + 24, t, 0xFFFFFF);
            for (int i = 0; i < k; i++) {
                if (m[i] < 0) snprintf(t, sizeof t, "%-6s DEFAULT", names[i]);
                else if (m[i] < 100) snprintf(t, sizeof t, "%-6s BUTTON %d", names[i], m[i]);
                else snprintf(t, sizeof t, "%-6s AXIS %d", names[i], m[i] - 100);
                draw_text(buf, width, GEN_RENDER_HEIGHT, x0, y0 + 44 + i * 11, t, 0x90C090);
            }
            draw_text(buf, width, GEN_RENDER_HEIGHT, x0, GEN_RENDER_HEIGHT - 14, "RETURN: SKIP  ESC: CANCEL", 0x707080);
            platform_show_frame(buf, width);
            SDL_Delay(16);
        }
    }
    bool any = false; for (int i = 0; i < 7; i++) any |= m[i] >= 0;
    if (any) input_set_mapping(m);
    while (input_raw_pressed() >= 0) { SDL_PumpEvents(); SDL_Delay(10); }   /* the last press is not "OK" */
}

/* Modal: called before presenting a frame; returns when the menu is closed. */
void ps_menu_run(uint8_t *frame, int width) {
    logic_tick();
    if (!s_open) return;
    static uint32_t buf[GEN_RENDER_MAX_WIDTH * GEN_RENDER_HEIGHT];
    int sel = 0, n = item_count();
    bool prev[8] = { true, true, true, true, true, true, true, true };
    while (s_open) {
        SDL_Event ev;
        bool quit = false;
        bool k[8] = { false };
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) quit = true;
        }
        const uint8_t *ks = SDL_GetKeyboardState(NULL);
        uint16_t pad = recomp_input_read_pad(0);
        k[0] = ks[SDL_SCANCODE_UP] || (pad & 0x01);
        k[1] = ks[SDL_SCANCODE_DOWN] || (pad & 0x02);
        k[2] = ks[SDL_SCANCODE_LEFT] || (pad & 0x04);
        k[3] = ks[SDL_SCANCODE_RIGHT] || (pad & 0x08);
        k[4] = ks[SDL_SCANCODE_RETURN] || ks[SDL_SCANCODE_Z] || (pad & 0x40);
        /* resume from anywhere in the list: Start (still held as the game reads the pad, so a
         * paused game unpauses in the same press) or C (the button that opened the menu) */
        k[5] = ks[SDL_SCANCODE_ESCAPE] || ks[SDL_SCANCODE_F1] || ks[SDL_SCANCODE_TAB] || (pad & 0x80) || (pad & 0x20);
        bool up = k[0] && !prev[0], down = k[1] && !prev[1], left = k[2] && !prev[2],
             right = k[3] && !prev[3], ok = k[4] && !prev[4], back = k[5] && !prev[5];
        memcpy(prev, k, sizeof prev);
        recomp_input_update();                       /* refresh pad state for the next pass */
        if (quit) { save(); exit(0); }
        if (up) sel = (sel + n - 1) % n;
        if (down) sel = (sel + 1) % n;
        if (left) change(sel, -1);
        if (right) change(sel, +1);
        if (ok) {
            if (sel == n - 1) { save(); exit(0); }
            if (sel == n - 2) { save(); ps_restart(); }
            if (sel == n - 6) { pad_setup(frame, width, buf); prev[4] = true; continue; }
            if (sel == n - 5) { save_slot(); continue; }
            if (sel == n - 4) { load_slot(); continue; }
            if (sel == n - 3) s_open = false; else change(sel, +1);
        }
        if (back) s_open = false;

        /* dimmed frame + menu */
        const uint32_t *src = (const uint32_t *)frame;
        for (int i = 0; i < width * GEN_RENDER_HEIGHT; i++) buf[i] = (src[i] >> 2) & 0x3F3F3F;
        int x0 = width / 2 - 100, y0 = 40;
        draw_text(buf, width, GEN_RENDER_HEIGHT, x0, y0 - 16, "PIXELSORCERY - SETTINGS", 0xFFE070);
        /* the rows that fit above the key hint; the list scrolls with the selection */
        int rows = (GEN_RENDER_HEIGHT - 20 - y0) / 11, first = 0;
        if (n > rows) { first = sel - rows / 2; if (first < 0) first = 0; if (first > n - rows) first = n - rows; }
        for (int r = 0; r < rows && first + r < n; r++) {
            int i = first + r;
            char t[64]; item_text(i, t, sizeof t);
            draw_text(buf, width, GEN_RENDER_HEIGHT, x0, y0 + r * 11, t, i == sel ? 0xFFFFFF : 0x9090A0);
            if (i == sel) draw_text(buf, width, GEN_RENDER_HEIGHT, x0 - 10, y0 + r * 11, ">", 0xFFE070);
        }
        if (first > 0) draw_text(buf, width, GEN_RENDER_HEIGHT, x0 + 190, y0, "^", 0xFFE070);
        if (first + rows < n) draw_text(buf, width, GEN_RENDER_HEIGHT, x0 + 190, y0 + (rows - 1) * 11, "v", 0xFFE070);
        draw_text(buf, width, GEN_RENDER_HEIGHT, x0, GEN_RENDER_HEIGHT - 14, "ENTER/A: TOGGLE   START/C/F1: RESUME", 0x707080);
        platform_show_frame(buf, width);
        SDL_Delay(16);
    }
    save();
}

/* PS_MENU_DEMO=1 with frame dumping: a scripted tour of the menu drawn over the dumped
 * frames (for the demo video; the game keeps running underneath). */
void ps_menu_demo_overlay(uint8_t *frame, int width, long f) {
    static int on = -1;
    if (on < 0) on = getenv("PS_MENU_DEMO") != NULL;
    if (!on) return;
    static const struct { long at; int sel; int act; } k_script[] = {
        { 0, 0, 0 }, { 45, 0, 1 }, { 90, 0, 1 }, { 135, 0, -1 }, { 180, 1, 0 }, { 220, 3, 0 },
        { 260, 3, -1 }, { 300, 3, 1 }, { 340, 5, 0 }, { 380, 6, 0 }, { 420, 7, 0 }, { 470, -1, 0 } };
    static int sel, done[16];
    if (f > 470) return;
    for (int i = 0; i < 12; i++)
        if (f >= k_script[i].at && !done[i]) {
            done[i] = 1;
            if (k_script[i].sel < 0) return;
            sel = k_script[i].sel;
            if (k_script[i].act) change(sel, k_script[i].act);
        }
    uint32_t *fb = (uint32_t *)frame;
    for (int i = 0; i < width * GEN_RENDER_HEIGHT; i++) fb[i] = (fb[i] >> 2) & 0x3F3F3F;
    int n = item_count(), x0 = width / 2 - 100, y0 = 40;
    draw_text(fb, width, GEN_RENDER_HEIGHT, x0, y0 - 16, "PIXELSORCERY - SETTINGS (F1)", 0xFFE070);
    for (int i = 0; i < n; i++) {
        char t[64]; item_text(i, t, sizeof t);
        draw_text(fb, width, GEN_RENDER_HEIGHT, x0, y0 + i * 11, t, i == sel ? 0xFFFFFF : 0x9090A0);
        if (i == sel) draw_text(fb, width, GEN_RENDER_HEIGHT, x0 - 10, y0 + i * 11, ">", 0xFFE070);
    }
}

static bool (*s_paused)(void);
void ps_menu_set_pause_hook(bool (*paused)(void)) { s_paused = paused; }
/* headless runs never pause the menu in (tests: PS_PAUSE_HINT=1 shows the hint there too) */
bool ps_menu_paused(void) { return s_paused && (!platform_is_headless() || getenv("PS_PAUSE_HINT")) && s_paused(); }
void ps_menu_pause_hint(uint8_t *frame, int width) {
    if (!ps_menu_paused()) return;
    const char *t = "C: SETTINGS";
    int x = width - 6 * (int)strlen(t) - 8, y = GEN_RENDER_HEIGHT - 12;
    uint32_t *fb = (uint32_t *)frame;
    for (int yy = y - 2; yy < y + 9; yy++)
        for (int xx = x - 3; xx < width - 5; xx++) fb[yy * width + xx] = (fb[yy * width + xx] >> 1) & 0x7F7F7F;
    draw_text(fb, width, GEN_RENDER_HEIGHT, x, y, t, 0xFFE070);
}
