/* ps_wide.h — widescreen: extra columns drawn from a persistent world-space cell cache. */
#ifndef PS_WIDE_H
#define PS_WIDE_H
#include <stdbool.h>
#include <stdint.h>

#define PS_WIDE_MAX 240                  /* max extra pixels each side (32:9 at 224 lines) */

/* Boss arena: world x range [x0, x1) in pixels. Past its walls (side 0 left, 1 right) the
 * side areas show, in order of precedence:
 *   wall_src[side] >= 0: a column block of the room wall_w px wide at world x wall_src,
 *     repeated (e.g. a pillar: its planes, plus its sprites in tile range wall_tiles);
 *   brick[2] > 0: a plane A block (world x, y, w, h in px, multiples of 8) tiled above the
 *     floor (a brick wall);
 *   mirror: the room mirrored across its wall (mirror_w > 0: only the mirror_w px next to
 *     the wall, mirrored back and forth);
 *   else: the room's floor stacked into a wall.
 * art > 0: PsWideGame.arts[art - 1], captured in an earlier room, stands at the walls in
 * art_sides (bit 0 left, bit 1 right; flipped on the right) on the floor, art_dy px lower and
 * art_dx px further out from the wall. Until it has been captured, mirror and mirror_w are
 * ignored (the floor is stacked into a wall instead of an open side with no art). */
typedef struct { int x0, x1, wall_src[2], wall_w, wall_tiles[2], brick[4]; bool mirror; int mirror_w, art, art_sides, art_dy, art_dx; } PsWideArena;

/* Wall art: a plane A block of another room (world px x, y, w, h in map), captured as
 * pixels with that room's tiles and colours the first time it is fully on screen, and kept
 * in the cache folder. Its top rep px repeat upward (a trunk). */
typedef struct { uint32_t map; int x, y, w, h, rep; } PsWideArt;

typedef struct {
    const char *name;                    /* cache folder under Application Support/PixelSorcery */
    bool     (*active)(void);            /* widen now (gameplay), else 4:3 */
    uint32_t (*map_id)(void);            /* level/room shown: one cache per map */
    void     (*camera)(int *x, int *y);  /* plane A world position of the screen's top-left */
    bool     (*arena)(PsWideArena *a);   /* optional: boss arena now (see PsWideArena) */
    const PsWideArt *arts; int n_arts;   /* optional: wall art sources */
    /* optional: plane A entry of world cell (cx, cy) from the game's own level map, for
     * side cells never seen on screen (false: none there) */
    bool     (*world_cell)(int cx, int cy, uint16_t *e);
    /* optional: a full-width status bar (window lines) widened inside instead of tiled at its
     * ends: {split_left, fill_left, split_right, fill_right} in 4:3 screen x. Columns below
     * split_left move E px left, those from split_right on move E px right, the middle stays;
     * the two gaps opened repeat columns fill_left / fill_right. All 0: tile the ends. */
    int bar[4];
    /* optional: the game keeps a sprite's full x in the sprite table's x word (its own culls
     * widened by hooks): in widescreen the renderer reads it as 11-bit signed instead of the
     * hardware's 9 bits, so sprites reach the whole side areas at 21:9/32:9 */
    bool wide_x;
    /* optional: the background (plane B) from the game's own data, so the side areas show the
     * level's background beyond the 512-px plane instead of stale columns: a cell of the
     * background layout (plane B entry) and the background camera (its world position) */
    bool     (*bg_cell)(int cx, int cy, uint16_t *e);
    void     (*bg_camera)(int *x, int *y);
} PsWideGame;

void     ps_wide_set_game(const PsWideGame *g);
int      ps_wide_max_extra(void);        /* configured extra pixels each side (PS_WIDE) */
int      ps_wide_extra(void);            /* extra pixels this frame (0 when not active) */
void     ps_wide_begin_frame(void);
void     ps_wide_record(uint16_t (*plane_entry)(int plane, int cx, int cy));
bool     ps_wide_entry(int wx, int wy, uint16_t (*plane_entry)(int plane, int cx, int cy), uint16_t *e);
void     ps_wide_camera(int *x, int *y);
int      ps_wide_obj_margin(void);
bool     ps_wide_sprite_x_range(int *lo, int *hi);
bool     ps_wide_wall_source(int side, int *wall_x, int *src_x, int *w);
bool     ps_wide_wall_tiles(int *first, int *last);   /* widescreen enemies: sprite cull range */
bool     ps_wide_bar(int b[4]);                       /* the game's bar split (PsWideGame.bar) */
int      ps_wide_full_x(void);
bool     ps_wide_bg(void);                            /* the game reads its background (bg_cell) */
bool     ps_wide_bg_cell(int cx, int cy, uint16_t *e);
void     ps_wide_bg_camera(int *x, int *y);                        /* side px when PsWideGame.wide_x applies, else 0 */
/* for PsWideGame.camera: the RAM camera snapped to plane A's scroll on screen (a game whose
 * camera variable runs ahead of the frame shown) */
void     ps_wide_snap_camera(int *x, int *y);
/* Wall art on this line, side 0 left / 1 right: screen x of its first pixel, width, and the
 * row (ARGB, alpha 0 where transparent; flipped on the right). */
bool     ps_wide_art_row(int side, int line, int *x, int *w, const uint32_t **row);
/* Wall art not captured yet: run this program once more, headless, with args (the title's
 * route to the art's room, e.g. through its level select) and env extras, waiting for it
 * (it saves the art in the same cache folder; no settings, no session). Returns whether
 * all arts are there afterwards. No-op in the scout run itself and on Windows. */
bool     ps_wide_scout_arts(const char *exe, char *const args[], char *const env[]);
void     ps_wide_set_aspect(int num, int den);   /* 4:3 disables */
void     ps_wide_set_objects(bool on);         /* widescreen object activation margin (16-px blocks) */
#endif
