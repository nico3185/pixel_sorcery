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
#ifndef _WIN32
#include <unistd.h>                                 /* truncate */
#endif

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
#elif defined(__MINGW32__)
/* MinGW (PE/COFF): weak data is unreliable there (a .weak alias was read instead of the
 * title's pointer; weak undefined refs do not link through .refptr): plain externs, the
 * defaults are members of the ps_defaults archive, linked last (engine/src/defaults/). */
extern const char *ps_cheat_names[];
extern int ps_cheat_count;
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
    char tmp[1100]; if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp) return;   /* path too long */
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
    for (char *q = dir + 1; *q; q++) if (*q == '/') { *q = 0; ps_mkdir(dir, 0755); *q = '/'; }
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
    for (char *p = dir + 1; *p; p++) if (*p == '/') { *p = 0; ps_mkdir(dir, 0755); *p = '/'; }
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

static void copy_lines(const char *src, const char *dst, long lines);
/* REWIND (Backspace, the gamepad's left shoulder): back STEPS seconds through ps_state.c's ring;
 * the session's recording is cut to that poll, so the session still replays to what is played */
void ps_menu_rewind(int steps) {
    uint8_t *ps_state_rewind_take(int, size_t *, long *); const char *ps_state_load_now(uint8_t *, size_t);
    size_t n; long poll; uint8_t *b = ps_state_rewind_take(steps, &n, &poll);
    if (!b) return;
    const char *rec = getenv("PS_POLL_RECORD");
    if (rec && *rec) {
        void ps_poll_flush(void), ps_poll_record_close(void), ps_poll_record_reopen(void);
        ps_poll_flush(); ps_poll_record_close();
#ifndef _WIN32
        if (truncate(rec, (off_t)poll * 10) != 0)           /* "XXXX XXXX\n" per poll (ps_poll.c) */
#endif
        {   char tmp[1100]; snprintf(tmp, sizeof tmp, "%s.rewind", rec);
            copy_lines(rec, tmp, poll); copy_lines(tmp, rec, -1); remove(tmp); }
        ps_poll_record_reopen();
    }
    fprintf(stderr, "menu: rewound %d step(s) to poll %ld\n", steps, poll);
    const char *why = ps_state_load_now(b, n);   /* returns only if refused */
    fprintf(stderr, "menu: rewind refused: %s\n", why);
}

enum { IT_ASPECT, IT_OBJ, IT_LAT, IT_VOL, IT_FULL, IT_REND, IT_WATER, IT_FIXED };

static bool level_select_ready(void);
static char s_stage_name[64][16], s_stage_poke[64][160]; static int s_nstages, s_stage_sel;   /* the recipe's "stages" */
static int item_count(void) { return IT_FIXED + ps_cheat_count + s_ntoggles + 6 + level_select_ready(); }
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
    } else if (level_select_ready() && i == item_count() - 7) {
        if (s_nstages) snprintf(out, n, "LEVEL SELECT        < %s >", s_stage_name[s_stage_sel]);
        else snprintf(out, n, "LEVEL SELECT");
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
    if (level_select_ready() && i == item_count() - 7 && s_nstages) { s_stage_sel = (s_stage_sel + dir + s_nstages) % s_nstages; return; }
    if (i == item_count() - 6 || (level_select_ready() && i == item_count() - 7)) return;
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
#ifndef _WIN32
#include <unistd.h>
#else
#include <process.h>
#define setenv(k, v, o) _putenv_s(k, v)
#define unsetenv(k) _putenv_s(k, "")
#define execv _execv
#endif
static void ps_restart_resume(const char *resume);
static void ps_restart(void) { unsetenv("GENRECOMP_MOVIE"); unsetenv("GENRECOMP_POKE"); unsetenv("PS_MOVIE_FAST"); ps_restart_resume(NULL); }

/* LEVEL SELECT: the title's level_select.json (games/TITLE, beside the build folder) types the game's
 * own level-select code for the player - the game restarts with the recipe's "code" presses played as
 * an input movie from power-on (GENRECOMP_MOVIE: added to the pad, then the pad alone), or with
 * "unit": "poll" as controller reads replayed at full speed (PS_RESUME), so the session's recording
 * holds only inputs. Entries: [frame or poll, "BUTTON+BUTTON", frames or polls held]. */
static char s_ls_path[1100];
static bool level_select_ready(void) {
    static int ready = -1;
    if (ready >= 0) return ready;
    ready = 0;
    extern char **platform_launch_argv(void);
    char **av = platform_launch_argv(), exe[1024];
    if (!av || !av[0]) return 0;
    snprintf(exe, sizeof exe, "%s", av[0]);
    char *sl = strrchr(exe, '/'); if (!sl) sl = strrchr(exe, '\\');
    if (!sl) return 0;
    *sl = 0;                                          /* games/TITLE/build */
    snprintf(s_ls_path, sizeof s_ls_path, "%s/../level_select.json", exe);
    FILE *f = fopen(s_ls_path, "r");
    if (!f) return ready;
    ready = 1;
    static char js[1 << 16]; size_t len = fread(js, 1, sizeof js - 1, f); fclose(f); js[len] = 0;
    const char *st = strstr(js, "\"stages\"");             /* [["NAME", "C=V:A=W[;...]"], ...]: one start per stage */
    for (const char *q = st ? strchr(st, '[') + 1 : NULL; q && s_nstages < 64; ) {
        const char *o = strchr(q, '['), *e = strchr(q, ']');
        if (!o || (e && e < o)) break;
        char a[160] = "", b[160] = "";
        if (sscanf(o, "[ \"%159[^\"]\" , \"%159[^\"]\"", a, b) == 2) {
            snprintf(s_stage_name[s_nstages], sizeof s_stage_name[0], "%s", a);
            snprintf(s_stage_poke[s_nstages], sizeof s_stage_poke[0], "%s", b); s_nstages++;
        }
        q = strchr(o, ']'); if (q) q++;
    }
    return ready;
}
static void level_select(void) {
    FILE *f = fopen(s_ls_path, "r");
    if (!f) return;
    static char js[1 << 16]; size_t len = fread(js, 1, sizeof js - 1, f); fclose(f); js[len] = 0;
    static uint16_t mv[20000]; long last = 0;
    memset(mv, 0, sizeof mv);
    {   /* RAM starts: the chosen stage's pokes, or the recipe's "poke" (GENRECOMP_POKE, kept beside the session) */
        char pk[200] = "";
        const char *pp = strstr(js, "\"poke\"");
        if (s_nstages) snprintf(pk, sizeof pk, "%s", s_stage_poke[s_stage_sel]);
        else if (pp) sscanf(strchr(pp, ':') + 1, " \"%199[^\"]\"", pk);
        if (*pk) setenv("GENRECOMP_POKE", pk, 1); else unsetenv("GENRECOMP_POKE");
    }
    const char *c = strstr(js, "\"code\"");
    if (!c) { unsetenv("GENRECOMP_MOVIE"); fprintf(stderr, "menu: level select: %s\n", getenv("GENRECOMP_POKE") ? getenv("GENRECOMP_POKE") : "-"); ps_restart_resume(NULL); return; }
    c = strchr(c, '[');
    for (const char *e = c + 1; e && *e; ) {               /* each [frame, "BUTTONS", held] */
        const char *o = strchr(e, '['), *end = strchr(e, ']');
        if (!o || (end && end < o)) break;                 /* the list's own closing bracket */
        long fr = strtol(o + 1, NULL, 10);
        const char *q = strchr(o, '"'); if (!q) break;
        const char *q2 = strchr(q + 1, '"'); if (!q2) break;
        long held = strtol(strchr(q2, ',') ? strchr(q2, ',') + 1 : q2, NULL, 10);
        uint16_t m = 0; char btn[64]; size_t bl = (size_t)(q2 - q - 1) < sizeof btn - 1 ? (size_t)(q2 - q - 1) : sizeof btn - 1;
        memcpy(btn, q + 1, bl); btn[bl] = 0;
        static const struct { const char *n; uint16_t m; } k[] = { {"UP", GEN_BTN_UP}, {"DOWN", GEN_BTN_DOWN}, {"LEFT", GEN_BTN_LEFT},
            {"RIGHT", GEN_BTN_RIGHT}, {"A", GEN_BTN_A}, {"B", GEN_BTN_B}, {"C", GEN_BTN_C}, {"START", GEN_BTN_START} };
        for (char *t = strtok(btn, "+"); t; t = strtok(NULL, "+"))
            for (size_t j = 0; j < sizeof k / sizeof k[0]; j++) if (!strcmp(t, k[j].n)) m |= k[j].m;
        for (long i = fr; i < fr + held && i < (long)(sizeof mv / sizeof mv[0]); i++) { mv[i] |= m; if (i + 1 > last) last = i + 1; }
        e = strchr(o, ']'); if (e) e++;
    }
    const char *rec = getenv("PS_POLL_RECORD");
    bool polls = strstr(js, "\"unit\"") && strstr(strstr(js, "\"unit\""), "\"poll\"");   /* counted in controller reads */
    char path[1200]; snprintf(path, sizeof path, "%s.levelselect.%s", rec && *rec ? rec : s_ls_path, polls ? "polls" : "txt");
    FILE *o = fopen(path, "w");
    if (!o) return;
    for (long i = 0; i < last; i++) fprintf(o, polls ? "%04X 0000\n" : "%x\n", mv[i]);
    fclose(o);
    fprintf(stderr, "menu: level select: %ld %s of the recipe's input from power-on (%s)\n", last, polls ? "polls" : "frames", s_ls_path);
    unsetenv("GENRECOMP_MOVIE");
    if (polls) {                                  /* replayed to its last poll at full speed, then the pad */
        char r[1300]; snprintf(r, sizeof r, "%s:%ld", path, last);
        ps_restart_resume(r);
    }
    setenv("GENRECOMP_MOVIE", path, 1);
    setenv("PS_MOVIE_FAST", "1", 1);                   /* typed unseen at full speed: the game lands on its level select */
    ps_restart_resume(NULL);
}
static void ps_restart_resume(const char *resume) {
    extern char **platform_launch_argv(void);
    char **argv = platform_launch_argv();              /* as launched, options included */
    if (!argv) { save(); exit(0); }
    const char *rec = getenv("PS_POLL_RECORD");
    if (rec && *rec) {
        static char path[1100]; static int n;
        char base[1024]; snprintf(base, sizeof base, "%s", rec);
        char *d = strrchr(base, '.'); if (d && !strcmp(d, ".polls")) *d = 0;
        char *r = strstr(base, "-reset"); if (r) { n = atoi(r + 6); *r = 0; }
        snprintf(path, sizeof path, "%s-reset%d.polls", base, n + 1);
        setenv("PS_POLL_RECORD", path, 1);
        const char *pk = getenv("GENRECOMP_POKE");       /* a RAM start: kept beside the session, which replays with it */
        if (pk && *pk) {
            char pf[1100]; snprintf(pf, sizeof pf, "%s-reset%d.poke", base, n + 1);
            FILE *o = fopen(pf, "w"); if (o) { fprintf(o, "%s\n", pk); fclose(o); }
        }
    }
    if (resume) setenv("PS_RESUME", resume, 1); else unsetenv("PS_RESUME");
    if (s_meta) fclose(s_meta);
    platform_shutdown();
    ps_execv(argv[0], argv);
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
enum { TH_W = 160, TH_H = 112 };                     /* a slot's picture: the frame at half size */
static uint32_t s_thumb[5][TH_W * TH_H]; static int s_thumb_ok[5];   /* per slot: 0 unread, 1 none, 2 loaded */
static void state_taken(const uint8_t *b, size_t n, long poll) {
    const char *rec = getenv("PS_POLL_RECORD");
    char p[1100]; slot_path(s_state_slot, "state", p, sizeof p);
    write_file(p, b, n);
    {   /* its picture, from the frame on screen */
        const uint8_t *genrecomp_get_framebuffer(void); int genrecomp_present_width(void);
        const uint32_t *f = (const uint32_t *)genrecomp_get_framebuffer(); int w = genrecomp_present_width(), k = s_state_slot;
        for (int y = 0; y < TH_H; y++) for (int x = 0; x < TH_W; x++) s_thumb[k][y * TH_W + x] = f[(y * 2) * w + x * w / TH_W];
        s_thumb_ok[k] = 2; slot_path(k, "thumb", p, sizeof p); write_file(p, (const uint8_t *)s_thumb[k], sizeof s_thumb[k]);
    }
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
    for (char *q = dir + 1; *q; q++) if (*q == '/') { *q = 0; ps_mkdir(dir, 0755); *q = '/'; }
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
    for (int c = 0; pn[c]; c++) if (pn[c] >= 'a' && pn[c] <= 'z') pn[c] -= 32;
    void input_get_mapping(int *), input_raw_name(int, char *, size_t);
    /* one row per function, its button by the pad's own name; Return / A changes the selected one only */
    static const struct { const char *name; int slot; } rows[] = { { "A", 0 }, { "B", 1 }, { "C", 2 }, { "START", 3 },
        { "X", 4 }, { "Y", 5 }, { "Z", 6 }, { "REWIND", 8 }, { "FORWARD", 9 } };
    /* (unset rows show their default by name; the pad's default layout: input.c k_default) */
    enum { NROW = 9, RESET = 9, DONE = 10 };
    int m[10]; input_get_mapping(m);
    int sel = 0; bool prev[4] = { true, true, true, true }, capture = false, released = false;
    for (;;) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) if (ev.type == SDL_QUIT) { save(); exit(0); }
        const uint8_t *ks = SDL_GetKeyboardState(NULL);
        if (capture) {                                      /* waiting for the new button */
            int r = input_raw_pressed();
            if (ks[SDL_SCANCODE_ESCAPE]) capture = false;
            else if (r < 0) released = true;
            else if (released) {
                int slot = rows[sel].slot;
                for (int i = 0; i < 10; i++) if (i != slot && m[i] == r) m[i] = m[slot];   /* already used: swapped */
                m[slot] = r; capture = false; input_set_mapping(m);
                while (input_raw_pressed() >= 0) { SDL_PumpEvents(); SDL_Delay(10); }
                prev[2] = true;
            }
        } else {
            uint16_t pad = recomp_input_read_pad(0); recomp_input_update();
            bool k[4] = { ks[SDL_SCANCODE_UP] || (pad & 0x01), ks[SDL_SCANCODE_DOWN] || (pad & 0x02),
                          ks[SDL_SCANCODE_RETURN] || (pad & 0x40) || (pad & 0x80), ks[SDL_SCANCODE_ESCAPE] || (pad & 0x10) };
            bool up = k[0] && !prev[0], down = k[1] && !prev[1], ok = k[2] && !prev[2], back = k[3] && !prev[3];
            memcpy(prev, k, sizeof prev);
            if (up) sel = (sel + DONE) % (DONE + 1);
            if (down) sel = (sel + 1) % (DONE + 1);
            if (back || (ok && sel == DONE)) break;
            if (ok && sel == RESET) { for (int i = 0; i < 10; i++) m[i] = -1; input_set_mapping(m); }
            else if (ok) { capture = true; released = false; }
        }
        const uint32_t *src = (const uint32_t *)frame;
        for (int i = 0; i < width * GEN_RENDER_HEIGHT; i++) buf[i] = (src[i] >> 2) & 0x3F3F3F;
        int x0 = width / 2 - 110, y0 = 34;
        draw_text(buf, width, GEN_RENDER_HEIGHT, x0, y0 - 22, "CONTROLLER", 0xFFE070);
        draw_text(buf, width, GEN_RENDER_HEIGHT, x0, y0 - 11, pn, 0x9090A0);
        for (int r = 0; r <= DONE; r++) {
            char t[64], b[32];
            if (r < NROW) { input_raw_name(m[rows[r].slot] >= 0 ? m[rows[r].slot] : 1000 + rows[r].slot, b, sizeof b);
                            snprintf(t, sizeof t, "%-9s %s", rows[r].name, capture && r == sel ? "PRESS A BUTTON..." : b); }
            else snprintf(t, sizeof t, "%s", r == RESET ? "RESET TO DEFAULTS" : "DONE");
            int y = y0 + 6 + r * 13 + (r >= NROW ? 6 : 0);
            draw_text(buf, width, GEN_RENDER_HEIGHT, x0, y, t, r == sel ? (capture ? 0xFFE070 : 0xFFFFFF) : 0x9090A0);
            if (r == sel) draw_text(buf, width, GEN_RENDER_HEIGHT, x0 - 10, y, ">", 0xFFE070);
        }
        draw_text(buf, width, GEN_RENDER_HEIGHT, x0, GEN_RENDER_HEIGHT - 14,
                  capture ? "PRESS THE NEW BUTTON   ESC: KEEP" : "A/RETURN: CHANGE   B/ESC: BACK", 0x707080);
        platform_show_frame(buf, width);
        SDL_Delay(16);
    }
    while (input_raw_pressed() >= 0) { SDL_PumpEvents(); SDL_Delay(10); }
}

/* The rewind timeline (Backspace / left shoulder): the game paused on the last 30 seconds of
 * snapshots (ps_state.c), the selected one full screen, a filmstrip of its neighbours (older to the
 * left), a bar and its age. Left goes back, Right forward to the present; A/Return/Start plays from the
 * selected moment, B/Esc/Backspace returns to the present unchanged. Held arrows repeat. */
static bool s_rw_open;
void ps_menu_rewind_request(void) { s_rw_open = true; }
static void fill(uint32_t *fb, int pitch, int x, int y, int w, int h, uint32_t c) {
    for (int j = y; j < y + h; j++) if (j >= 0 && j < GEN_RENDER_HEIGHT)
        for (int i = x; i < x + w; i++) if (i >= 0 && i < pitch) fb[j * pitch + i] = c;
}
static void draw_rewind(uint32_t *buf, const uint8_t *frame, int width, int sel, int n) {
    const uint32_t *ps_state_rewind_thumb(int, int *, int *, long *);
    /* the selected moment, full size (the snapshots keep a half-size picture) */
    int tw = 0, th = 0; long age = 0;
    const uint32_t *t = sel ? ps_state_rewind_thumb(sel, &tw, &th, &age) : NULL;
    if (t) {
        for (int y = 0; y < GEN_RENDER_HEIGHT; y++)            /* the picture kept the frame's width */
            for (int x = 0; x < width; x++) buf[y * width + x] = t[(y * th / GEN_RENDER_HEIGHT) * tw + x * tw / width];
    } else memcpy(buf, frame, (size_t)width * GEN_RENDER_HEIGHT * 4);
    /* top band: title and age */
    for (int i = 0; i < width * 14; i++) buf[i] = (buf[i] >> 2) & 0x3F3F3F;
    draw_text(buf, width, GEN_RENDER_HEIGHT, 6, 4, "<< REWIND", 0xFFE070);
    char lab[24];
    if (sel) snprintf(lab, sizeof lab, "-%ld S", (age + 30) / 60); else snprintf(lab, sizeof lab, "NOW");
    draw_text(buf, width, GEN_RENDER_HEIGHT, width - 6 - (int)strlen(lab) * 6, 4, lab, 0xFFFFFF);
    /* bottom band: filmstrip, bar, keys */
    int by = GEN_RENDER_HEIGHT - 66;
    for (int i = by * width; i < width * GEN_RENDER_HEIGHT; i++) buf[i] = (buf[i] >> 2) & 0x3F3F3F;
    const int mw = 48, mh = 34, gap = 6;
    int ps_state_rewind_every(void); int st = 60 / ps_state_rewind_every(); if (st < 1) st = 1;
    for (int s = -2; s <= 2; s++) {
        int step = sel - s * st, x = width / 2 - mw / 2 + s * (mw + gap), y = by + 4;   /* older to the left */
        if (step < 0 || step > n) continue;
        int w2, h2; long a2; const uint32_t *m = step ? ps_state_rewind_thumb(step, &w2, &h2, &a2) : NULL;
        fill(buf, width, x - 1, y - 1, mw + 2, mh + 2, s == 0 ? 0xFFE070 : 0x404050);
        for (int j = 0; j < mh; j++)
            for (int i = 0; i < mw; i++) {
                uint32_t c = m ? m[(j * h2 / mh) * w2 + i * w2 / mw]
                               : ((const uint32_t *)frame)[(j * GEN_RENDER_HEIGHT / mh) * width + i * width / mw];
                buf[(y + j) * width + x + i] = s == 0 ? c : (c >> 1) & 0x7F7F7F;
            }
    }
    int bx = 20, bw = width - 40, bar = by + mh + 12;  /* the 30 s window, the present at the right */
    fill(buf, width, bx, bar, bw, 2, 0x505060);
    fill(buf, width, bx + bw - 1 - sel * (bw - 1) / (n > 0 ? n : 1) - 1, bar - 3, 3, 8, 0xFFE070);
    draw_text(buf, width, GEN_RENDER_HEIGHT, bx, bar + 8, "HOLD REWIND / FORWARD   A: PLAY FROM HERE   B: BACK", 0x9090A0);
}
static void rewind_ui(const uint8_t *frame, int width) {
    int ps_state_rewind_count(void);
    const uint32_t *ps_state_rewind_thumb(int, int *, int *, long *);
    int n = ps_state_rewind_count();
    if (!n) return;
    static uint32_t buf[GEN_RENDER_MAX_WIDTH * GEN_RENDER_HEIGHT];
    int sel = 0, held = 0;                          /* snapshots back: 0 = now (the buttons move from there) */
    bool prev[5] = { true, true, true, true, true };
    for (;;) {
        SDL_Event ev; bool quit = false;
        while (SDL_PollEvent(&ev)) if (ev.type == SDL_QUIT) quit = true;
        if (quit) { save(); exit(0); }
        const uint8_t *ks = SDL_GetKeyboardState(NULL);
        uint16_t pad = recomp_input_read_pad(0);
        recomp_input_update();
        extern bool input_hot_held(int);
        bool k[5] = { ks[SDL_SCANCODE_LEFT] || (pad & 0x04), ks[SDL_SCANCODE_RIGHT] || (pad & 0x08),
                      ks[SDL_SCANCODE_RETURN] || ks[SDL_SCANCODE_Z] || (pad & 0x40) || (pad & 0x80),
                      ks[SDL_SCANCODE_ESCAPE] || ks[SDL_SCANCODE_X] || (pad & 0x10), false };
        bool left = k[0] && !prev[0], right = k[1] && !prev[1], ok = k[2] && !prev[2], back = k[3] && !prev[3];
        if (k[0] || k[1]) { if (++held > 18 && held % 4 == 0) { left |= k[0]; right |= k[1]; } } else held = 0;
        memcpy(prev, k, sizeof prev);
        int ps_state_rewind_every(void); int st = 60 / ps_state_rewind_every(); if (st < 1) st = 1;   /* ~1 s a press */
        if (left) sel = sel + st > n ? n : sel + st;
        if (right) sel = sel - st < 0 ? 0 : sel - st;
        /* REWIND / FORWARD held (Backspace / ], or the pad's buttons): through the snapshots, one a frame */
        if (ks[SDL_SCANCODE_BACKSPACE] || input_hot_held(0)) { if (sel < n) sel++; }
        else if (ks[SDL_SCANCODE_RIGHTBRACKET] || input_hot_held(1)) { if (sel > 0) sel--; }
        if (back || (ok && sel == 0)) break;
        if (ok) {                                   /* play from there: returns only if refused */
            while (input_raw_pressed() >= 0) { SDL_PumpEvents(); SDL_Delay(10); }
            save(); ps_menu_rewind(sel); break;
        }
        draw_rewind(buf, frame, width, sel, n);
        platform_show_frame(buf, width);
        SDL_Delay(16);
    }
    while (input_raw_pressed() >= 0) { SDL_PumpEvents(); SDL_Delay(10); }   /* nothing leaks into the game */
}

/* Modal: called before presenting a frame; returns when the menu is closed. */
void ps_menu_run(uint8_t *frame, int width) {
    {   /* PS_LEVEL_SELECT_NOW=N: the LEVEL SELECT entry at the first frame, its Nth stage (tests) */
        static int once; if (!once++ && getenv("PS_LEVEL_SELECT_NOW")) { int k = atoi(getenv("PS_LEVEL_SELECT_NOW")); unsetenv("PS_LEVEL_SELECT_NOW");
            if (level_select_ready()) { if (k > 0 && k <= s_nstages) s_stage_sel = k - 1; level_select(); } }
    }
    logic_tick();
    if (s_rw_open) { s_rw_open = false; rewind_ui(frame, width); }
    {   /* PS_REWIND_SHOT=FRAME:FILE: the timeline drawn 3 s back at that frame, as a PNG (tests) */
        static long at = -2; static const char *path; extern long ps_frames;
        if (at == -2) { const char *e = getenv("PS_REWIND_SHOT"); at = e && strchr(e, ':') ? atol(e) : -1; path = at >= 0 ? strchr(e, ':') + 1 : NULL; }
        int ps_state_rewind_count(void);
        if (at >= 0 && ps_frames >= at && ps_state_rewind_count() >= 3) {
            static uint32_t sb[GEN_RENDER_MAX_WIDTH * GEN_RENDER_HEIGHT]; void ps_png_write(const char *, const uint8_t *, int, int);
            draw_rewind(sb, frame, width, 3, ps_state_rewind_count()); ps_png_write(path, (const uint8_t *)sb, width, GEN_RENDER_HEIGHT); at = -1;
        }
    }
    if (!s_open) return;
    static uint32_t buf[GEN_RENDER_MAX_WIDTH * GEN_RENDER_HEIGHT];
    /* pages: the main one (frequent actions, then the sections), DISPLAY, SOUND & CONTROLS, CHEATS - each a list
     * of the items above (item_text / change keep their meaning) or a link (negative ids) */
    enum { P_DISPLAY = -10, P_SOUND = -11, P_CHEATS = -12, A_REWIND = -13 };
    static const char *k_title[4] = { "SETTINGS", "SETTINGS > DISPLAY", "SETTINGS > SOUND & CONTROLS", "SETTINGS > CHEATS" };
    int n = item_count(), page = 0, sel = 0, list[64], cnt = 0;
    bool prev[8] = { true, true, true, true, true, true, true, true };
    while (s_open) {
        n = item_count(); cnt = 0;
        if (page == 0) {
            list[cnt++] = n - 3; list[cnt++] = n - 5; list[cnt++] = n - 4; list[cnt++] = A_REWIND;
            if (level_select_ready()) list[cnt++] = n - 7;
            list[cnt++] = P_DISPLAY; list[cnt++] = P_SOUND;
            if (ps_cheat_count + s_ntoggles) list[cnt++] = P_CHEATS;
            list[cnt++] = n - 2; list[cnt++] = n - 1;
        } else if (page == 1) {
            static const int d[] = { IT_ASPECT, IT_OBJ, IT_FULL, IT_REND, IT_WATER, IT_LAT };
            for (size_t q = 0; q < sizeof d / sizeof d[0]; q++) list[cnt++] = d[q];
        } else if (page == 2) { list[cnt++] = IT_VOL; list[cnt++] = n - 6; }
        else for (int q = 0; q < ps_cheat_count + s_ntoggles && cnt < 64; q++) list[cnt++] = IT_FIXED + q;
        if (sel >= cnt) sel = cnt - 1;
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
        /* close from anywhere: F1 / Tab / Start (still held as the game reads the pad, so a paused game
         * unpauses in the same press) or C (the button that opened the menu); back a page: Esc or B */
        k[5] = ks[SDL_SCANCODE_F1] || ks[SDL_SCANCODE_TAB] || (pad & 0x80) || (pad & 0x20);
        k[6] = ks[SDL_SCANCODE_ESCAPE] || ks[SDL_SCANCODE_X] || (pad & 0x10);
        bool up = k[0] && !prev[0], down = k[1] && !prev[1], left = k[2] && !prev[2],
             right = k[3] && !prev[3], ok = k[4] && !prev[4], close = k[5] && !prev[5], back = k[6] && !prev[6];
        memcpy(prev, k, sizeof prev);
        recomp_input_update();                       /* refresh pad state for the next pass */
        if (quit) { save(); exit(0); }
        if (up) sel = (sel + cnt - 1) % cnt;
        if (down) sel = (sel + 1) % cnt;
        int id = list[sel];
        if (id >= 0 && left) change(id, -1);
        if (id >= 0 && right) change(id, +1);
        if (ok) {
            if (id == P_DISPLAY || id == P_SOUND || id == P_CHEATS) { page = id == P_DISPLAY ? 1 : id == P_SOUND ? 2 : 3; sel = 0; continue; }
            if (id == A_REWIND) { s_open = false; ps_menu_rewind_request(); break; }
            if (id == n - 1) { save(); exit(0); }
            if (id == n - 2) { save(); ps_restart(); }
            if (id == n - 6) { pad_setup(frame, width, buf); prev[4] = true; continue; }
            if (level_select_ready() && id == n - 7) { save(); level_select(); }
            if (id == n - 5) { save_slot(); continue; }
            if (id == n - 4) { load_slot(); continue; }
            if (id == n - 3) s_open = false; else change(id, +1);
        }
        if (close) s_open = false;
        if (back) { if (page) { page = 0; sel = 0; continue; } s_open = false; }

        /* dimmed frame + the page */
        const uint32_t *src = (const uint32_t *)frame;
        for (int i = 0; i < width * GEN_RENDER_HEIGHT; i++) buf[i] = (src[i] >> 2) & 0x3F3F3F;
        int x0 = width / 2 - 100, y0 = 40;
        draw_text(buf, width, GEN_RENDER_HEIGHT, x0, y0 - 16, k_title[page], 0xFFE070);
        int rows = (GEN_RENDER_HEIGHT - 20 - y0) / 12, first = 0;
        if (cnt > rows) { first = sel - rows / 2; if (first < 0) first = 0; if (first > cnt - rows) first = cnt - rows; }
        for (int r = 0; r < rows && first + r < cnt; r++) {
            int q = first + r, it = list[q];
            char t[64];
            if (it == P_DISPLAY) snprintf(t, sizeof t, "DISPLAY             >");
            else if (it == P_SOUND) snprintf(t, sizeof t, "SOUND & CONTROLS    >");
            else if (it == P_CHEATS) snprintf(t, sizeof t, "CHEATS              >");
            else if (it == A_REWIND) snprintf(t, sizeof t, "REWIND");
            else item_text(it, t, sizeof t);
            int y = y0 + r * 12 + (page == 0 && (it == P_DISPLAY || it == n - 2) ? 4 : 0);   /* gaps before the sections and RESET */
            draw_text(buf, width, GEN_RENDER_HEIGHT, x0, y, t, q == sel ? 0xFFFFFF : 0x9090A0);
            if (q == sel) draw_text(buf, width, GEN_RENDER_HEIGHT, x0 - 10, y, ">", 0xFFE070);
        }
        if (first > 0) draw_text(buf, width, GEN_RENDER_HEIGHT, x0 + 190, y0, "^", 0xFFE070);
        if (first + rows < cnt) draw_text(buf, width, GEN_RENDER_HEIGHT, x0 + 190, y0 + (rows - 1) * 12, "v", 0xFFE070);
        if (page == 0 && (list[sel] == n - 5 || list[sel] == n - 4)) {   /* SAVE / LOAD: the slot's picture */
            int k = s_slot;
            if (!s_thumb_ok[k]) {
                char tp[1100]; slot_path(k, "thumb", tp, sizeof tp);
                FILE *f = fopen(tp, "rb"); s_thumb_ok[k] = f && fread(s_thumb[k], sizeof s_thumb[k], 1, f) == 1 ? 2 : 1; if (f) fclose(f);
            }
            int tx = width - TH_W / 2 - 12, ty = GEN_RENDER_HEIGHT - TH_H / 2 - 24;
            for (int y = -1; y <= TH_H / 2; y++) for (int x = -1; x <= TH_W / 2; x++) buf[(ty + y) * width + tx + x] = 0x606070;
            for (int y = 0; y < TH_H / 2; y++) for (int x = 0; x < TH_W / 2; x++)
                buf[(ty + y) * width + tx + x] = s_thumb_ok[k] == 2 ? s_thumb[k][(y * 2) * TH_W + x * 2] : 0x101018;
            if (s_thumb_ok[k] != 2) draw_text(buf, width, GEN_RENDER_HEIGHT, tx + 22, ty + 22, "EMPTY", 0x707080);
        }
        draw_text(buf, width, GEN_RENDER_HEIGHT, x0, GEN_RENDER_HEIGHT - 14,
                  page ? "A: SELECT  </>: CHANGE  B/ESC: BACK" : "A: SELECT  </>: CHANGE  START: RESUME", 0x707080);
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
/* on-screen notices: a short line top left for a second and a half ("<< REWIND", "x4", ...) */
static char s_notice[40]; static int s_notice_left;
void ps_menu_notice(const char *t) { snprintf(s_notice, sizeof s_notice, "%s", t); s_notice_left = 90; }
static void draw_notice(uint8_t *frame, int width) {
    if (s_notice_left <= 0) return;
    s_notice_left--;
    uint32_t *fb = (uint32_t *)frame; int x = 8, y = 8, w = 6 * (int)strlen(s_notice) + 8;
    for (int yy = y - 3; yy < y + 10; yy++)
        for (int xx = x - 4; xx < x - 4 + w; xx++) fb[yy * width + xx] = (fb[yy * width + xx] >> 2) & 0x3F3F3F;
    draw_text(fb, width, GEN_RENDER_HEIGHT, x, y, s_notice, s_notice_left > 20 ? 0xFFE070 : 0x9C8A46);
}
void ps_menu_pause_hint(uint8_t *frame, int width) {
    draw_notice(frame, width);
    if (!ps_menu_paused()) return;
    const char *t = "C: SETTINGS";
    int x = width - 6 * (int)strlen(t) - 8, y = GEN_RENDER_HEIGHT - 12;
    uint32_t *fb = (uint32_t *)frame;
    for (int yy = y - 2; yy < y + 9; yy++)
        for (int xx = x - 3; xx < width - 5; xx++) fb[yy * width + xx] = (fb[yy * width + xx] >> 1) & 0x7F7F7F;
    draw_text(fb, width, GEN_RENDER_HEIGHT, x, y, t, 0xFFE070);
}
