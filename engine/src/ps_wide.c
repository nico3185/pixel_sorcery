/* ps_wide.c — widescreen for the native renderer.
 *
 * The view is extended by E pixels each side (aspect ratio from PS_WIDE, default 16:9;
 * PS_WIDE=0 or 4:3 disables). Plane A (the level) is drawn there from a world-space cell
 * cache: every frame, the cells visible on the original screen are recorded under their
 * world coordinates (camera from the game, plane cell = world cell modulo the plane size).
 * Extra columns use the cached cell when the area has been seen before, else the game's
 * level map when the title can read it, else the live plane (what the hardware would show). Caches persist per map, so a replayed
 * recording pre-bakes whole levels. Plane B and sprites are drawn live.
 *
 * The game tells the engine when to widen (gameplay only), which map is shown and where
 * the camera is (ps_wide_set_game). Without that, nothing is widened.
 */
#include "genrecomp/ps_wide.h"
#include "genrecomp/ps_vdp.h"
#include "genrecomp/ps_render.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#ifndef _WIN32
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;
#endif

static PsWideGame s_game;
static int s_extra = -1;                 /* pixels each side when active */
static uint32_t s_map = 0xFFFFFFFF;
static int s_camx, s_camy;
static bool s_active;
static bool s_arena;                     /* boss room (title hook): sides mirror the room */
static int s_ax0, s_ax1;                 /* its world cell columns [s_ax0, s_ax1] */
static int s_wsrc[2], s_ww;              /* wall source cell columns per side, width in cells */
static int s_wt0 = -1, s_wt1 = -1;       /* its sprites' tile range (-1: planes only) */
static int s_brick[4];                   /* brick block in cells: x, y, w, h (w 0: none) */
static bool s_mirror_only;
static int s_mw;                         /* mirrored strip next to each wall, cells (0: all) */
static int s_fy0, s_fh;                  /* its floor: first cell row and height (0: none) */
static uint16_t (*s_plane_entry)(int plane, int cx, int cy);
static int s_art;                        /* wall art shown now: index + 1 (0: none) */
static int s_art_sides, s_art_dy, s_art_dx;

static bool blank_cell(uint16_t e) {     /* tile with no opaque pixel */
    const uint8_t *t = g_ps_vdp.vram + (uint32_t)(e & 0x7FF) * 32;
    for (int i = 0; i < 32; i++) if (t[i]) return false;
    return true;
}
/* The arena's floor: the lowest run of rows (above the status bar) that are solid across the
 * arena. Stacked upward past the walls it makes a wall of the level's own material (pink
 * cubes, stone blocks, the forest ground) where Mickey stops, instead of open floor he
 * cannot walk on. */
static void find_floor(void) {
    s_fh = 0;
    if (!s_plane_entry) return;
    int wl = (g_ps_vdp.reg[18] & 0x1F) << 3, wdown = g_ps_vdp.reg[18] >> 7;
    int bottom = s_camy + (wdown && wl ? wl : 224);
    int c0 = s_camx >> 3, c1 = (s_camx + 319) >> 3, y = (bottom >> 3) - 1, run = 0, top = y;
    for (; y >= (s_camy >> 3); y--) {
        int n = 0, solid = 0;
        for (int cx = c0 + 1; cx < c1; cx += 3, n++) solid += !blank_cell(s_plane_entry(0, cx, y));
        if (solid * 4 >= n * 3) { top = y; run++; }
        else if (run) break;
    }
    if (!run) return;
    /* the band continues under the status bar: whole blocks stack (clown: 32-px cubes) */
    for (y = top + run; run < 8; y++, run++) {
        int n = 0, solid = 0;
        for (int cx = c0 + 1; cx < c1; cx += 3, n++) solid += !blank_cell(s_plane_entry(0, cx, y));
        if (solid * 4 < n * 3) break;
    }
    s_fy0 = top; s_fh = run;
}

/* open-addressing table: world cell (x, y) -> name-table entry */
#define CAP (1 << 18)
static uint32_t s_key[CAP];
static uint16_t s_val[CAP];
static int s_count;
static bool s_dirty;
static int s_bx0 = 1 << 30, s_bx1 = -(1 << 30), s_by0 = 1 << 30, s_by1 = -(1 << 30);   /* explored cells */
static void grow(int wx, int wy) {
    if (wx < s_bx0) s_bx0 = wx; if (wx > s_bx1) s_bx1 = wx;
    if (wy < s_by0) s_by0 = wy; if (wy > s_by1) s_by1 = wy;
}
static FILE *s_misslog;

static uint32_t key_of(int wx, int wy) { return ((uint32_t)(uint16_t)wx << 16 | (uint16_t)wy) + 1; }
static uint32_t slot_of(uint32_t k) { return (k * 2654435761u) >> (32 - 18); }   /* high bits: the low ones only see wy */

static void put(int wx, int wy, uint16_t e) {
    grow(wx, wy);
    uint32_t k = key_of(wx, wy), i = slot_of(k);
    while (s_key[i] && s_key[i] != k) i = (i + 1) & (CAP - 1);
    if (!s_key[i]) { if (s_count > CAP * 3 / 4) return; s_key[i] = k; s_count++; s_dirty = true; }
    else if (s_val[i] != e) s_dirty = true;
    s_val[i] = e;
}
static bool get(int wx, int wy, uint16_t *e) {
    uint32_t k = key_of(wx, wy), i = slot_of(k);
    while (s_key[i]) { if (s_key[i] == k) { *e = s_val[i]; return true; } i = (i + 1) & (CAP - 1); }
    return false;
}

/* ------------------------------------------------------------------ persistence */
static void cache_path(char *out, size_t n, uint32_t map) {
    const char *home = getenv("PS_WIDE_HOME") ? getenv("PS_WIDE_HOME") : getenv("HOME");
    snprintf(out, n, "%s/Library/Application Support/PixelSorcery/%s/wide/%08X.bin",
             home ? home : ".", s_game.name ? s_game.name : "game", map);
}
static void mkdirs(const char *path) {
    char tmp[1024]; snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
}
static void cache_save(void) {
    if (s_map == 0xFFFFFFFF || !s_dirty) return;
    char path[1024]; cache_path(path, sizeof path, s_map); mkdirs(path);
    FILE *f = fopen(path, "wb"); if (!f) return;
    for (uint32_t i = 0; i < CAP; i++) if (s_key[i]) { fwrite(&s_key[i], 4, 1, f); fwrite(&s_val[i], 2, 1, f); }
    fclose(f); s_dirty = false;
}
static void cache_load(uint32_t map) {
    memset(s_key, 0, sizeof s_key); s_count = 0; s_dirty = false;
    s_bx0 = s_by0 = 1 << 30; s_bx1 = s_by1 = -(1 << 30);
    char path[1024]; cache_path(path, sizeof path, map);
    FILE *f = fopen(path, "rb"); if (!f) return;
    uint32_t k; uint16_t v;
    while (fread(&k, 4, 1, f) == 1 && fread(&v, 2, 1, f) == 1) {
        uint32_t i = slot_of(k);
        while (s_key[i] && s_key[i] != k) i = (i + 1) & (CAP - 1);
        if (!s_key[i]) { s_key[i] = k; s_count++; }
        s_val[i] = v;
        grow((int16_t)((k - 1) >> 16), (int16_t)((k - 1) & 0xFFFF));
    }
    fclose(f);
}
/* ------------------------------------------------------------------ wall art */
#define ART_MAX 4
static uint32_t *s_art_px[ART_MAX];      /* w*h ARGB, alpha 0: transparent */
static int8_t s_art_state[ART_MAX];      /* 0 not looked for, 1 have pixels, -1 none yet */
static long s_art_lum[ART_MAX];          /* brightness of the capture held (fades are darker) */
static bool s_art_dirty[ART_MAX];
static void art_path(char *out, size_t n, int i) {
    const PsWideArt *a = &s_game.arts[i];
    char dir[1024]; cache_path(dir, sizeof dir, a->map);
    *strrchr(dir, '/') = 0;
    snprintf(out, n, "%s/art_%08X_%d_%d.bin", dir, a->map, a->x, a->y);
}
static bool art_have(int i) {
    if (s_art_state[i] == 0) {
        const PsWideArt *a = &s_game.arts[i];
        s_art_state[i] = -1;
        s_art_px[i] = calloc((size_t)a->w * a->h, 4);
        char path[1024]; art_path(path, sizeof path, i);
        FILE *f = fopen(path, "rb");
        if (f) {
            int wh[2];
            if (fread(wh, 4, 2, f) == 2 && wh[0] == a->w && wh[1] == a->h &&
                fread(s_art_px[i], 4, (size_t)a->w * a->h, f) == (size_t)a->w * a->h) s_art_state[i] = 1;
            fclose(f);
        }
    }
    return s_art_state[i] > 0;
}
static void art_save(void) {
    for (int i = 0; i < s_game.n_arts && i < ART_MAX; i++) {
        if (!s_art_dirty[i]) continue;
        const PsWideArt *a = &s_game.arts[i];
        char path[1024]; art_path(path, sizeof path, i); mkdirs(path);
        FILE *f = fopen(path, "wb"); if (!f) continue;
        int wh[2] = { a->w, a->h };
        fwrite(wh, 4, 2, f); fwrite(s_art_px[i], 4, (size_t)a->w * a->h, f); fclose(f);
        s_art_dirty[i] = false;
    }
}
/* In the art's own room, with the block on the original screen: its pixels with the live
 * tiles and colours. The brightest frame is kept (rooms fade in). */
static void art_capture(void) {
    for (int i = 0; i < s_game.n_arts && i < ART_MAX; i++) {
        const PsWideArt *a = &s_game.arts[i];
        if (a->map != s_map || a->x < s_camx || a->y < s_camy ||
            a->x + a->w > s_camx + 320 || a->y + a->h > s_camy + 224) continue;
        art_have(i);
        uint32_t *px = calloc((size_t)a->w * a->h, 4);
        long lum = 0;
        for (int y = 0; y < a->h; y++)
            for (int cx = a->x >> 3; cx < (a->x + a->w) >> 3; cx++) {
                uint16_t e = s_plane_entry(0, cx, (a->y + y) >> 3);
                uint8_t row[8];
                ps_render_tile_row(e, (a->y + y) & 7, e & 0x0800, e & 0x1000, row);
                for (int p = 0; p < 8; p++) {
                    if (!row[p]) continue;
                    uint32_t c = ps_render_color(((e >> 13) & 3) * 16 + row[p], 1);
                    px[y * a->w + cx * 8 + p - a->x] = c;
                    lum += (c >> 16 & 0xFF) + (c >> 8 & 0xFF) + (c & 0xFF);
                }
            }
        if (lum > s_art_lum[i]) {
            memcpy(s_art_px[i], px, (size_t)a->w * a->h * 4);
            s_art_lum[i] = lum; s_art_state[i] = 1; s_art_dirty[i] = true;
        }
        free(px);
    }
}

bool ps_wide_scout_arts(const char *exe, char *const args[], char *const env[]) {
    bool missing = false;
    for (int i = 0; i < s_game.n_arts && i < ART_MAX; i++) missing |= !art_have(i);
    if (!missing) return true;
#if defined(_WIN32) || !defined(PS_NATIVE)
    (void)exe; (void)args; (void)env; return false;
#else
    if (getenv("PS_ART_SCOUT") || !exe || !strchr(exe, '/')) return false;
    const char *home = getenv("PS_WIDE_HOME") ? getenv("PS_WIDE_HOME") : getenv("HOME");
    char wh[1100]; snprintf(wh, sizeof wh, "PS_WIDE_HOME=%s", home ? home : ".");
    char *envp[32]; int n = 0;
    envp[n++] = wh;
    envp[n++] = "HOME=/nonexistent";            /* no settings.ini: default 16:9, no codes */
    envp[n++] = "PS_WIDE=16:9";
    envp[n++] = "PS_MUTE=1";
    envp[n++] = "PS_ART_SCOUT=1";
    static char dyld[2048];                      /* the app's bundled SDL */
    const char *dl = getenv("DYLD_FALLBACK_LIBRARY_PATH");
    if (dl) { snprintf(dyld, sizeof dyld, "DYLD_FALLBACK_LIBRARY_PATH=%s", dl); envp[n++] = dyld; }
    for (int i = 0; env && env[i] && n < 31; i++) envp[n++] = env[i];
    envp[n] = NULL;
    char *argv[48]; int m = 0;
    argv[m++] = (char *)exe;
    for (int i = 0; args && args[i] && m < 47; i++) argv[m++] = args[i];
    argv[m] = NULL;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    pid_t pid; int st = 0;
    int r = posix_spawn(&pid, exe, &fa, NULL, argv, envp);
    posix_spawn_file_actions_destroy(&fa);
    if (r != 0 || waitpid(pid, &st, 0) < 0) return false;
    for (int i = 0; i < s_game.n_arts && i < ART_MAX; i++)
        if (s_art_state[i] <= 0) { s_art_state[i] = 0; free(s_art_px[i]); s_art_px[i] = NULL; }
    missing = false;
    for (int i = 0; i < s_game.n_arts && i < ART_MAX; i++) missing |= !art_have(i);
    if (getenv("PS_WIDE_STATS")) fprintf(stderr, "wide: wall art scout run %s\n", missing ? "failed" : "done");
    return !missing;
#endif
}

static void stats(void);
static void at_exit(void) { cache_save(); art_save(); stats(); }

/* ------------------------------------------------------------------ API */
void ps_wide_set_game(const PsWideGame *g) {
    s_game = *g; atexit(at_exit);
    const char *m = getenv("PS_WIDE_MISSLOG"); if (m && *m) s_misslog = fopen(m, "w");
}

int ps_wide_max_extra(void) {
#ifndef PS_NATIVE
    return 0;                                    /* oracle build: 320-wide compares only */
#endif
    if (s_extra < 0) {
        const char *a = getenv("PS_WIDE");
        double num = 16, den = 9;
        if (a && (!strcmp(a, "0") || !strcmp(a, "4:3"))) { s_extra = 0; return 0; }
        if (a && strchr(a, ':')) { num = atof(a); den = atof(strchr(a, ':') + 1); }
        double w = 224.0 * num / (den > 0 ? den : 9);
        int e = (int)((w - 320) / 2 + 4) / 8 * 8;        /* multiple of 8 */
        s_extra = e < 0 ? 0 : e > PS_WIDE_MAX ? PS_WIDE_MAX : e;
    }
    return s_extra;
}

int ps_wide_extra(void) { return s_active ? ps_wide_max_extra() : 0; }

void ps_wide_begin_frame(void) {
    s_active = false;
    if (!s_game.active || !ps_wide_max_extra() || !(g_ps_vdp.reg[12] & 1)) return;   /* H40 only */
    if (!s_game.active()) return;
    uint32_t map = s_game.map_id();
    if (map != s_map) { cache_save(); art_save(); cache_load(map); s_map = map; }
    s_game.camera(&s_camx, &s_camy);
    s_active = true;
    PsWideArena a = { 0, 0, { -1, -1 }, 0, { -1, -1 }, { 0, 0, 0, 0 }, false, 0, 0, 0, 0, 0 };
    s_arena = s_game.arena && s_game.arena(&a) && a.x1 - a.x0 >= 64;
    s_art = 0;
    if (s_arena) {
        s_ax0 = a.x0 >> 3; s_ax1 = (a.x1 - 1) >> 3; find_floor();
        /* the floor is found on screen; when the camera scrolls up (the clown boss's high
         * jumps) it slides under the status bar: keep this arena's floor, or the walls
         * built from it turn into plain mirrored tiles for those frames */
        static uint32_t f_map; static int f_ax0 = -1, f_fy0, f_fh;
        bool same = f_ax0 == s_ax0 && f_map == s_map;
        if (s_fh > 0 && (!same || s_fy0 >= f_fy0)) { f_map = s_map; f_ax0 = s_ax0; f_fy0 = s_fy0; f_fh = s_fh; }
        else if (same) { s_fy0 = f_fy0; s_fh = f_fh; }   /* the lowest band found is the floor */
        s_ww = a.wall_w >> 3;
        for (int i = 0; i < 2; i++) s_wsrc[i] = s_ww > 0 && a.wall_src[i] >= 0 ? a.wall_src[i] >> 3 : -1;
        s_wt0 = a.wall_tiles[0]; s_wt1 = a.wall_tiles[1];
        for (int i = 0; i < 4; i++) s_brick[i] = a.brick[i] >> 3;
        s_mirror_only = a.mirror; s_mw = a.mirror_w >> 3;
        if (a.art > 0 && a.art <= s_game.n_arts && a.art <= ART_MAX && s_fh && art_have(a.art - 1)) s_art = a.art, s_art_sides = a.art_sides, s_art_dy = a.art_dy, s_art_dx = a.art_dx;
        else if (a.art > 0) s_mirror_only = false, s_mw = 0;
    }
}

/* Record the plane A cells shown on the original 320x224 screen. */
void ps_wide_record(uint16_t (*plane_entry)(int plane, int cx, int cy)) {
    s_plane_entry = plane_entry;
    if (!s_active) return;
    int x0 = s_camx >> 3, x1 = (s_camx + 319) >> 3, y0 = s_camy >> 3, y1 = (s_camy + 223) >> 3;
    /* stale cache: most cached cells on screen disagree with the live plane (the game
     * redrew the room - COI's upside-down rooms - or reused the map id): forget the
     * map and relearn, rather than drawing the old world on the sides */
    int known = 0, differ = 0;
    for (int wy = y0; wy <= y1; wy += 2)
        for (int wx = x0; wx <= x1; wx += 2) {
            uint16_t c;
            if (get(wx, wy, &c)) { known++; differ += c != plane_entry(0, wx, wy); }
        }
    if (known >= 64 && differ * 2 > known) {
        memset(s_key, 0, sizeof s_key); s_count = 0; s_dirty = true;
        s_bx0 = s_by0 = 1 << 30; s_bx1 = s_by1 = -(1 << 30);
        static long n; n++;
        if (getenv("PS_WIDE_STATS")) fprintf(stderr, "wide: map %08X cache stale (%d/%d cells differ), relearning (#%ld)\n", s_map, differ, known, n);
    }
    for (int wy = y0; wy <= y1; wy++)
        for (int wx = x0; wx <= x1; wx++)
            put(wx, wy, plane_entry(0, wx, wy));
    art_capture();
}

/* Plane A entry for world cell (wx, wy): cached if seen, else the live plane. */
static long s_hits, s_misses, s_outside, s_mirrored, s_mapped, s_cache_vs_map;
/* Plane A entry for world cell (wx, wy): cached if seen; outside the explored area of
 * the map nothing is drawn (false: plane B shows through); else the live plane. */
bool ps_wide_entry(int wx, int wy, uint16_t (*plane_entry)(int plane, int cx, int cy), uint16_t *e) {
    if (get(wx, wy, e)) {
        s_hits++;
        static int check = -1; if (check < 0) check = getenv("PS_WIDE_STATS") != NULL;
        uint16_t m;                              /* stats: cache vs the game's level map */
        if (check && s_game.world_cell && s_game.world_cell(wx, wy, &m) && m != *e) {
            s_cache_vs_map++;
            if (s_misslog) fprintf(s_misslog, "%08X %d %d cache %04X map %04X cam %d %d\n", s_map, wx, wy, *e, m, s_camx, s_camy);
        }
        return true;
    }
    if (s_arena && (wx < s_ax0 || wx > s_ax1)) {
        /* boss arena: past its walls, the room's own tiles mirrored across the wall, so
         * walls, ground and canopy continue instead of stopping at the 4:3 border */
        if (s_wsrc[wx > s_ax1] >= 0) return false;   /* drawn by ps_wide_wall_source */
        if (s_brick[2] > 0 && s_brick[3] > 0 && (!s_fh || wy < s_fy0)) {   /* brick wall */
            int bx = s_brick[0] + ((wx - s_ax0) % s_brick[2] + s_brick[2]) % s_brick[2];
            int by = s_brick[1] + ((wy - s_brick[1]) % s_brick[3] + s_brick[3]) % s_brick[3];
            if (!get(bx, by, e)) *e = plane_entry(0, bx, by);
            s_mirrored++; return true;
        }
        int mx = wx < s_ax0 ? 2 * s_ax0 - 1 - wx : 2 * s_ax1 + 1 - wx;
        int my = wy;
        uint16_t flip = 0x0800;
        if (s_mw > 0) {                          /* the strip at the wall, back and forth */
            int dist = wx < s_ax0 ? s_ax0 - 1 - wx : wx - s_ax1 - 1, q = dist % (2 * s_mw);
            int off = q < s_mw ? q : 2 * s_mw - 1 - q;
            mx = wx < s_ax0 ? s_ax0 + off : s_ax1 - off;
            if (q >= s_mw) flip = 0;
        }
        if (s_fh && wy < s_fy0 && !s_mirror_only && !s_brick[2])   /* floor stacked into a wall */
            my = s_fy0 + ((wy - s_fy0) % s_fh + s_fh) % s_fh;
        if (mx >= s_ax0 && mx <= s_ax1) {
            if (!get(mx, my, e)) *e = plane_entry(0, mx, my);   /* the arena fits the plane */
            *e ^= flip; s_mirrored++; return true;
        }
    }
    if (s_game.world_cell) {                     /* the game's level map knows the cell */
        if (s_game.world_cell(wx, wy, e)) { s_mapped++; return true; }
        s_outside++; return false;
    }
    if (wx < s_bx0 || wx > s_bx1 || wy < s_by0 || wy > s_by1) { s_outside++; return false; }
    s_misses++;
    *e = plane_entry(0, wx, wy);
    if (s_misslog) fprintf(s_misslog, "%08X %d %d %04X\n", s_map, wx, wy, *e);
    return true;
}
static void stats(void) {
    extern long g_ps_side_sprites;
    if (getenv("PS_WIDE_STATS")) fprintf(stderr, "wide: sprite lines in the side areas %ld\n", g_ps_side_sprites);
    if (getenv("PS_WIDE_STATS")) fprintf(stderr, "wide: side cells from cache %ld, level map %ld, live %ld, outside the map %ld, mirrored (arenas) %ld\n",
                                         s_hits, s_mapped, s_misses, s_outside, s_mirrored);
    if (getenv("PS_WIDE_STATS")) fprintf(stderr, "wide: cached side cells that differ from the level map %ld\n", s_cache_vs_map);
    { extern long g_ps_bg_cells, g_ps_bg_differ;
      if (getenv("PS_WIDE_STATS") && g_ps_bg_cells) fprintf(stderr, "wide: background cells checked in the 4:3 picture %ld, differing from the plane %ld\n", g_ps_bg_cells, g_ps_bg_differ); }
}

void ps_wide_camera(int *x, int *y) { *x = s_camx; *y = s_camy; }
bool ps_wide_bg(void) { return s_game.bg_cell && s_game.bg_camera && ps_wide_extra(); }
bool ps_wide_bg_cell(int cx, int cy, uint16_t *e) { return s_game.bg_cell(cx, cy, e); }
void ps_wide_bg_camera(int *x, int *y) { s_game.bg_camera(x, y); }

/* Boss arena wall with a source block (PsWideArena.wall_src): screen x of the wall on that
 * side (pixels beyond it are the side), and the source block's screen x and width. The
 * renderer copies the composited planes from the block, whatever layer the block is on. */
bool ps_wide_wall_tiles(int *first, int *last) {
    if (!s_active || !s_arena || s_wt0 < 0) return false;
    *first = s_wt0; *last = s_wt1; return true;
}
bool ps_wide_wall_source(int side, int *wall_x, int *src_x, int *w) {
    if (!s_active || !s_arena || s_wsrc[side] < 0) return false;
    *wall_x = (side ? (s_ax1 + 1) * 8 : s_ax0 * 8) - s_camx;
    *src_x = s_wsrc[side] * 8 - s_camx; *w = s_ww * 8;
    int e = ps_wide_extra();                       /* the block must be on the drawn line */
    return *src_x >= -e && *src_x + *w <= 320 + e;
}

bool ps_wide_art_row(int side, int line, int *x, int *w, const uint32_t **row) {
    if (!s_active || !s_art || !(s_art_sides >> side & 1)) return false;
    const PsWideArt *a = &s_game.arts[s_art - 1];
    int y = line - (s_fy0 * 8 + s_art_dy - s_camy - a->h);   /* standing on the floor */
    if (y >= a->h) return false;
    if (y < 0) { if (a->rep <= 0) return false; y = ((y % a->rep) + a->rep) % a->rep; }
    static uint32_t flipped[PS_WIDE_MAX * 2];
    const uint32_t *r = s_art_px[s_art - 1] + (size_t)y * a->w;
    *w = a->w;
    if (side == 0) { *x = s_ax0 * 8 - s_camx - a->w - s_art_dx; *row = r; }
    else {
        int n = a->w < PS_WIDE_MAX * 2 ? a->w : PS_WIDE_MAX * 2;
        for (int i = 0; i < n; i++) flipped[i] = r[a->w - 1 - i];
        *x = (s_ax1 + 1) * 8 - s_camx + s_art_dx; *w = n; *row = flipped;
    }
    return true;
}

/* Widescreen object activation margin, in 16-pixel blocks each side (0 when not widened or
 * PS_WIDE_OBJECTS=0). Title hooks widen the game's activation range with it. */
static int s_obj_on = -1;
void ps_wide_set_objects(bool on) { s_obj_on = on; }
void ps_wide_set_aspect(int num, int den) {
#ifdef PS_NATIVE
    double w = 224.0 * num / den;
    int e = (int)((w - 320) / 2 + 4) / 8 * 8;
    s_extra = e < 0 ? 0 : e > PS_WIDE_MAX ? PS_WIDE_MAX : e;
#else
    (void)num; (void)den;
#endif
}
int ps_wide_obj_margin(void) {
    int on = s_obj_on;
    if (on < 0) { const char *e = getenv("PS_WIDE_OBJECTS"); on = s_obj_on = !(e && *e == '0'); }
    return on ? (ps_wide_extra() + 15) / 16 : 0;
}

/* Widescreen enemies: sprite x range (hardware coordinates, screen x + 128) a title's sprite
 * writer accepts instead of its 4:3 cull, so sprites slide in through the side areas instead
 * of appearing at the 4:3 edge. Capped to what a sprite can show: x + 128 in [0, 512).
 * False when not widened or the setting is off (4:3 logic unchanged). */
/* the configured side width, also while a level loads (widescreen not active yet): the object
 * load windows must already be wide for a level's first objects */
int ps_wide_full_x(void) {
    int on = s_obj_on;
    if (on < 0) { const char *e = getenv("PS_WIDE_OBJECTS"); on = s_obj_on = !(e && *e == '0'); }
    return s_game.wide_x && on ? ps_wide_max_extra() : 0;
}

bool ps_wide_sprite_x_range(int *lo, int *hi) {
    if (!ps_wide_obj_margin()) return false;
    int e = ps_wide_extra();
    if (ps_wide_full_x()) { *lo = 0x60 - e; *hi = 0x1C0 + e; return true; }   /* no 9-bit cap */
    *lo = 0x60 - e < 0 ? 0 : 0x60 - e;              /* a 32-px piece reaches x = -E */
    *hi = 0x1C0 + e > 0x200 ? 0x200 : 0x1C0 + e;    /* left edge before x = 320 + E */
    return true;
}

bool ps_wide_bar(int b[4]) {
    if (!s_game.bar[0] && !s_game.bar[2]) return false;
    for (int i = 0; i < 4; i++) b[i] = s_game.bar[i];
    return true;
}

/* Snap a game's RAM camera to plane A's scroll on screen: the value congruent to the VDP's
 * plane A scroll (modulo the plane size) nearest the estimate. For games whose camera
 * variable runs ahead of what the frame shows (SH3 writes its scroll table from the
 * camera before moving it): the side areas then line up with the 4:3 picture exactly. */
#include <genrecomp/ps_vdp.h>
static int snap(int est, int shown, int size) {
    int d = ((shown - est) % size + size) % size;
    return est + (d >= size / 2 ? d - size : d);
}
void ps_wide_snap_camera(int *x, int *y) {
    const PsVdp *v = &g_ps_vdp;
    int w = (v->reg[16] & 3) == 3 ? 1024 : (v->reg[16] & 3) == 1 ? 512 : 256;
    int h = ((v->reg[16] >> 4) & 3) == 3 ? 1024 : ((v->reg[16] >> 4) & 3) == 1 ? 512 : 256;
    int hs = (v->reg[13] & 0x3F) << 10, mode = v->reg[11] & 3, line = 112;   /* mid-screen line */
    int off = mode == 3 ? line * 4 : mode == 2 ? (line & ~7) * 4 : 0;
    int a = ((v->vram[(hs + off) & 0xFFFF] << 8) | v->vram[(hs + off + 1) & 0xFFFF]) & 0x3FF;
    *x = snap(*x, (-a) & (w - 1), w);
    if (!(v->reg[11] & 4)) *y = snap(*y, v->vsram[0] & (h - 1), h);   /* full-screen vscroll */
}
