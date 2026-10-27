/*
 * PixelSorcery native renderer. See include/genrecomp/ps_render.h.
 *
 * Per line, three 8-bit layer buffers are built and merged:
 *   pixel byte = d3-d0 colour, d5-d4 palette, d6 priority, d7 layer marker
 * Plane B and plane A (or the window) are merged by a priority table, then
 * sprites are merged on top front-to-back. In shadow/highlight mode the top
 * two bits of the final byte select shadow / normal / highlight intensity.
 */

#include "genrecomp/ps_render.h"
#include "genrecomp/ps_vdp.h"

#include <string.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "genrecomp/ps_wide.h"
#define ORG (32 + PS_WIDE_MAX + 16)  /* left margin: partial columns, sprites, widescreen */
#define LINE_MAX (ORG + 512 + PS_WIDE_MAX + 64)
static int s_water = -1;                 /* real transparency (PS_WATER, settings menu) */
static int s_extra;                  /* widescreen pixels each side for the frame being drawn */

/* Priority tables, indexed [(back << 8) | front]. */
static uint8_t s_lut_bg[0x10000];        /* plane B, plane A */
static uint8_t s_lut_bgobj[0x10000];     /* merged bg, sprite (normal mode) */
static uint8_t s_lut_bg_sh[0x10000];     /* plane B, plane A (shadow/highlight) */
static uint8_t s_lut_obj[0x10000];       /* sprite buffer, sprite (S/H mode) */
static uint8_t s_lut_bgobj_sh[0x10000];  /* merged bg, sprite buffer (S/H mode) */

static bool s_sprite_limits = true;
static int  s_spr_ovr;                   /* sprite masking carried to the next line */
uint16_t g_ps_render_flags;
long g_ps_side_sprites;                  /* widescreen stats: sprite lines drawn in the side areas */              /* VDP status bits seen while drawing: $40 sprite overflow, $20 collision */

/* ------------------------------------------------------------------ */
/* Priority tables                                                    */
/* ------------------------------------------------------------------ */

/* Plane A over plane B unless B has priority over a low-priority A. */
static uint8_t pick_bg(int b, int a) {
    int bp = b & 0x40, ap = a & 0x40;
    int bo = b & 0x0F, ao = a & 0x0F;
    int c = ap ? (ao ? (a & 0x7F) : (b & 0x7F))
               : (bp ? (bo ? (b & 0x7F) : (a & 0x7F)) : (ao ? (a & 0x7F) : (b & 0x7F)));
    return (uint8_t)c;
}

static uint8_t make_bg(int b, int a) {
    int c = pick_bg(b, a);
    if ((c & 0x0F) == 0) c &= 0x80;           /* transparent: backdrop */
    return (uint8_t)c;
}

static uint8_t make_bg_sh(int b, int a) {
    int c = pick_bg(b, a);
    c |= ((a & 0x40) | (b & 0x40)) << 1;      /* d7: normal intensity if either plane has priority */
    if ((c & 0x0F) == 0) c &= 0x80;
    return (uint8_t)c;
}

/* Sprite onto merged background (normal mode). d7 marks a pixel already
 * taken by a sprite earlier in the list (which therefore wins). */
static uint8_t make_bgobj(int bg, int spr) {
    int s = spr & 0x0F;
    if (!s) return (uint8_t)bg;
    if (bg & 0x80) return (uint8_t)bg;
    int bp = bg & 0x40, bo = bg & 0x0F;
    int c = (spr & 0x40) ? (spr & 0x3F) : (bp ? (bo ? (bg & 0x3F) : (spr & 0x3F)) : (spr & 0x3F));
    if ((c & 0x0F) == 0) c &= 0x80;
    return (uint8_t)(c | 0x80);
}

/* Sprite onto sprite buffer (S/H mode): first opaque sprite pixel wins. */
static uint8_t make_obj(int buf, int spr) {
    if (!(spr & 0x0F)) return (uint8_t)buf;
    int c = (buf & 0x80) ? (buf & 0x7F) : (spr & 0x7F);
    if ((c & 0x0F) == 0) c &= 0xC0;
    return (uint8_t)(c | 0x80);
}

/* Shadow/highlight composition. Background d7 = normal intensity (else shadow).
 * Output d7-d6: 00 shadow, 01 normal, 10 highlight. Palette 3 colours 14/15 of a
 * visible sprite are operators: 15 shadows, 14 highlights what is underneath. */
static uint8_t sprite_over_sh(int bg, int spr, int bi) {
    int bf = bg & 0x3F, sf = spr & 0x3F, sp = spr & 0x40;
    int si = sp | bi;
    if ((sf & 0x3E) == 0x3E)                                  /* palette 3, colour 14/15 */
        return (uint8_t)((sf & 1) ? bf : ((bg & 0x80) ? (bf | 0x80) : (bf | 0x40)));
    if (sf == 0x0E || sf == 0x1E || sf == 0x2E)               /* colour 14 is never shadowed */
        return (uint8_t)(sf | 0x40);
    return (uint8_t)(sf | si);
}

static uint8_t make_bgobj_sh(int bg, int spr) {
    int bf = bg & 0x3F, bp = bg & 0x40, bo = bg & 0x0F;
    int bi = (bg & 0x80) >> 1;
    int s  = spr & 0x0F, sp = spr & 0x40;
    int c;
    if (sp)
        c = s ? sprite_over_sh(bg, spr, bi) : (bf | bi);
    else if (bp && bo)
        c = bf | bi;
    else
        c = s ? sprite_over_sh(bg, spr, bi) : (bf | bi);
    if ((c & 0x0F) == 0) c &= 0xC0;
    return (uint8_t)c;
}

void ps_render_init(void) {
    for (int b = 0; b < 256; b++)
        for (int f = 0; f < 256; f++) {
            int i = (b << 8) | f;
            s_lut_bg[i]       = make_bg(b, f);
            s_lut_bgobj[i]    = make_bgobj(b, f);
            s_lut_bg_sh[i]    = make_bg_sh(b, f);
            s_lut_obj[i]      = make_obj(b, f);
            s_lut_bgobj_sh[i] = make_bgobj_sh(b, f);
        }
    s_spr_ovr = 0;
}

void ps_render_set_sprite_limits(bool on) { s_sprite_limits = on; }

/* ------------------------------------------------------------------ */
/* Colour                                                             */
/* ------------------------------------------------------------------ */

static const PsVdp *s_vdp_override;     /* palette source for one colour table (real water) */
static uint16_t s_pal0[64];             /* the frame's palette at line 0 */
static uint8_t s_wmask[256][1024];      /* modern water: 1 water, 2 sprite over it */
static uint32_t s_wbehind[256][1024];   /* the sharp scene behind the water */
static uint32_t s_wlight; static int s_wlight_lum = -1, s_wwidth;
static int s_water_log;
static double s_water_alpha = 0.65;
static int s_water_min = 16;          /* colours changed mid-frame that make a water line (PS_WATER_MIN) */
static long s_water_last = -1;
static long ps_frame_no(void) { extern long ps_frames; return ps_frames; }
/* comparison runs (PS_RENDERER=compare/gpgx) draw the original picture: the self-check
 * compares pixels with the reference renderer */
/* comparison runs (PS_RENDERER=compare/gpgx) keep the original picture; play.sh sets native */
static bool comparing(void) { const char *r = getenv("PS_RENDERER"); return r && *r && strcmp(r, "native"); }
/* Modern water is per title (ps_render_allow_water from the title's main): screen-door
 * dithering is also used for skies and decorations elsewhere (SH2's sky) */
static bool s_water_allowed;
void ps_render_allow_water(void) { s_water_allowed = true; }
bool ps_render_water_allowed(void) { return s_water_allowed; }
void ps_render_set_water(bool on) { s_water = comparing() || !s_water_allowed ? 0 : on; }

uint32_t ps_render_color(int index, int intensity) {
    const PsVdp *d = s_vdp_override ? s_vdp_override : &g_ps_vdp;
    uint16_t c = d->cram[index & 0x3F];
    if (!(d->reg[0] & 0x04)) c &= 0x49;           /* palette select off: LSB of each gun only */
    int r = c & 7, g = (c >> 3) & 7, b = (c >> 6) & 7;
    if (!(d->reg[12] & 0x08)) intensity = 1;
    switch (intensity) {
    case 0:  break;                                /* shadow: half */
    case 2:  r += 7; g += 7; b += 7; break;        /* highlight: half + half */
    default: r <<= 1; g <<= 1; b <<= 1; break;     /* normal */
    }
    return 0xFF000000u | ((uint32_t)(r * 0x11) << 16) | ((uint32_t)(g * 0x11) << 8) | (uint32_t)(b * 0x11);
}

/* ------------------------------------------------------------------ */
/* Pattern and name-table fetch                                       */
/* ------------------------------------------------------------------ */

void ps_render_tile_row(uint32_t tile, int row, bool hflip, bool vflip, uint8_t out[8]) {
    const uint8_t *p = &g_ps_vdp.vram[((tile & 0x7FF) << 5) + ((vflip ? 7 - row : row) << 2)];
    for (int i = 0; i < 4; i++) {
        uint8_t hi = p[i] >> 4, lo = p[i] & 0x0F;
        if (hflip) { out[7 - 2 * i] = hi; out[6 - 2 * i] = lo; }
        else       { out[2 * i] = hi;     out[2 * i + 1] = lo; }
    }
}

static inline uint16_t vram_word(uint32_t a) {
    a &= 0xFFFE;
    return (uint16_t)((g_ps_vdp.vram[a] << 8) | g_ps_vdp.vram[a + 1]);
}

/* Plane geometry from register 16, in 2-cell column pairs and pixel rows.
 * Invalid size codes behave as the hardware's mirrored layouts do. */
static const uint8_t  k_row_shift[4] = { 6, 7, 0, 8 };
static const uint8_t  k_col_mask[4]  = { 0x0F, 0x1F, 0x0F, 0x3F };
static const uint16_t k_row_mask[4]  = { 0x0FF, 0x1FF, 0x2FF, 0x3FF };

static uint32_t plane_base(int plane) {
    return plane == 0 ? (uint32_t)(g_ps_vdp.reg[2] & 0x38) << 10
                      : (uint32_t)(g_ps_vdp.reg[4] & 0x07) << 13;
}

uint16_t ps_render_plane_entry(int plane, int cx, int cy) {
    uint8_t r16 = g_ps_vdp.reg[16];
    int colmask = k_col_mask[r16 & 3], rowmask = k_row_mask[(r16 >> 4) & 3];
    int width_cells = (colmask + 1) * 2;
    int y = (cy * 8) & rowmask;
    uint32_t row = ((uint32_t)(y >> 3) << k_row_shift[r16 & 3]) & 0x1FC0;
    uint32_t col = (uint32_t)(((cx % width_cells) + width_cells) % width_cells);
    return vram_word(plane_base(plane) + row + col * 2);
}

/* Draw one name-table entry's row into a layer buffer at `dst`. */
/* Real transparency: tiles drawn as a "screen door" (thin opaque streaks or dots over
 * transparent pixels, which a CRT blurred into see-through water) are marked as they are
 * drawn, and blended at the end of the line (see PS_WATER in render_line). */
static uint8_t *s_sd_buf;                /* the line buffer being drawn (marks are relative to it) */
static uint8_t s_sd_mark[LINE_MAX];      /* pixel of a screen-door tile, this line */
static bool tile_screen_door(int tile) {
    static uint32_t s_stamp[2048]; static uint8_t s_val[2048];
    extern long ps_frames;
    uint32_t now = (uint32_t)ps_frames + 1;
    if (s_stamp[tile] == now) return s_val[tile];
    int rows = 0, opaque = 0;
    for (int r = 0; r < 8; r++) {
        uint8_t px[8]; ps_render_tile_row(tile, r, false, false, px);
        int t = 0;
        for (int i = 0; i < 8; i++) { opaque += (px[i] & 15) != 0; if (i && ((px[i] & 15) != 0) != ((px[i - 1] & 15) != 0)) t++; }
        rows += t >= 3;
    }
    s_stamp[tile] = now;
    return s_val[tile] = rows >= 5 && opaque >= 13 && opaque <= 45;
}

static inline void draw_entry(uint8_t *dst, uint16_t e, int row) {
    uint8_t px[8];
    ps_render_tile_row(e & 0x7FF, row, (e & 0x0800) != 0, (e & 0x1000) != 0, px);
    uint8_t atex = (uint8_t)((e >> 9) & 0x70);
    for (int i = 0; i < 8; i++) dst[i] = px[i] | atex;
    if (s_water > 0 && s_sd_buf && tile_screen_door(e & 0x7FF)) {
        long o = dst - s_sd_buf;
        for (int i = 0; i < 8; i++) if (o + i >= 0 && o + i < LINE_MAX) s_sd_mark[o + i] = 1;
    }
}

/* Draw a 2-cell column pair of a scrolling plane: `pair` indexes the plane's
 * columns in units of 2 cells, `vs` is the vertical scroll for this column. */
static void draw_pair(uint8_t *dst, int plane, int line, int pair, int vs) {
    uint8_t r16 = g_ps_vdp.reg[16];
    int colmask = k_col_mask[r16 & 3], rowmask = k_row_mask[(r16 >> 4) & 3];
    int v = (line + vs) & rowmask;
    uint32_t base = plane_base(plane) + ((((uint32_t)v >> 3) << k_row_shift[r16 & 3]) & 0x1FC0);
    uint32_t a = base + (uint32_t)(pair & colmask) * 4;
    draw_entry(dst, vram_word(a), v & 7);
    draw_entry(dst + 8, vram_word(a + 2), v & 7);
}

/* ------------------------------------------------------------------ */
/* Background layers                                                  */
/* ------------------------------------------------------------------ */

typedef struct { int left, right, enable; } Clip;

/* Split of a line between plane A and the window (columns of 16 px). */
static void window_clip(Clip clip[2], int width_cols) {
    uint8_t r17 = g_ps_vdp.reg[17];
    int hp = r17 & 0x1F, hf = (r17 >> 7) & 1;
    int a = hf, w = hf ^ 1;
    memset(clip, 0, sizeof(Clip) * 2);
    if (hp) {
        if (hp > width_cols) {
            clip[w].left = 0; clip[w].right = width_cols; clip[w].enable = 1;
            clip[a].enable = 0;
        } else {
            clip[w].left = 0; clip[a].right = width_cols;
            clip[a].left = clip[w].right = hp;
            clip[0].enable = clip[1].enable = 1;
        }
    } else {
        clip[a].left = 0; clip[a].right = width_cols; clip[a].enable = 1;
        clip[w].enable = 0;
    }
}

/* Scrolling plane over screen columns [c0, c1) of 16 px, plus the partially
 * visible column on the left when the fine scroll is not 0. */
static void draw_plane(uint8_t *buf, int plane, int line, int c0, int c1, bool window_bug) {
    const PsVdp *d = &g_ps_vdp;
    static const uint8_t k_hmask[4] = { 0x00, 0x07, 0xF8, 0xFF };
    uint32_t hscb = ((uint32_t)d->reg[13] << 10) & 0xFC00;
    uint32_t ha = hscb + ((uint32_t)(line & k_hmask[d->reg[11] & 3]) << 2) + (plane ? 2 : 0);
    int hs = vram_word(ha) & 0x3FF;
    int colmask = k_col_mask[d->reg[16] & 3];
    int shift = hs & 0x0F;
    int index = colmask + 1 + c0 - ((hs >> 4) & colmask);
    bool vcol = (d->reg[11] & 0x04) != 0;
    int vs_full = d->vsram[plane ? 1 : 0];

    if (shift) {
        /* Left-most partial column: in 2-cell mode it uses a fixed value
         * (H40: both planes share VSRAM 38 & 39; H32: 0). */
        int vs = vs_full;
        if (vcol) vs = (d->reg[12] & 1) ? (d->vsram[38] & d->vsram[39]) : 0;
        int pair = (window_bug && c0) ? index : index - 1;
        draw_pair(buf + ORG - 16 + shift + c0 * 16, plane, line, pair, vs);
    }
    for (int c = c0; c < c1; c++, index++) {
        int cc = c < 0 ? 0 : c > 19 ? 19 : c;          /* widescreen: edge columns' scroll */
        int vs = vcol ? d->vsram[(2 * cc + plane) % 40] : vs_full;
        draw_pair(buf + ORG + shift + c * 16, plane, line, index, vs);
    }
}

static void draw_window(uint8_t *buf, int line, int c0, int c1) {
    const PsVdp *d = &g_ps_vdp;
    bool h40 = d->reg[12] & 1;
    uint32_t ntwb = ((uint32_t)d->reg[3] << 10) & (h40 ? 0xF000 : 0xF800);
    uint32_t row = ntwb | ((uint32_t)(line >> 3) << (6 + (h40 ? 1 : 0)));
    for (int c = c0; c < c1; c++) {
        uint32_t a = row + (uint32_t)c * 4;
        draw_entry(buf + ORG + c * 16, vram_word(a), line & 7);
        draw_entry(buf + ORG + c * 16 + 8, vram_word(a + 2), line & 7);
    }
}

/* ------------------------------------------------------------------ */
/* Sprites                                                            */
/* ------------------------------------------------------------------ */

typedef struct { uint16_t attr; int xpos; int yoff; uint8_t size; } Obj;

static inline uint16_t sat_word(uint32_t byte_off) {
    return (uint16_t)((g_ps_vdp.sat[byte_off & 0x3FF] << 8) | g_ps_vdp.sat[(byte_off + 1) & 0x3FF]);
}

/* Sprites on `line`, in link order. Y, size and link come from the internal
 * sprite cache; attributes and X from VRAM, as on the hardware. */
static int collect_sprites(int line, Obj *out) {
    const PsVdp *d = &g_ps_vdp;
    int width = (d->reg[12] & 1) ? 320 : 256;
    bool h40 = d->reg[12] & 1;
    uint32_t satb = ((uint32_t)d->reg[5] << 9) & (h40 ? 0xFC00 : 0xFE00);
    int max_on_line = s_sprite_limits ? (width >> 4) : 80;
    int total = width >> 2;
    int target = line + 0x80;
    int link = 0, count = 0;
    do {
        uint32_t s = (uint32_t)link * 8;
        int ypos = sat_word(s) & 0x1FF;
        if (target >= ypos) {
            uint8_t size = (uint8_t)(sat_word(s + 2) >> 8);
            int height = 8 + ((size & 3) << 3);
            int yoff = target - ypos;
            if (yoff < height) {
                if (count == max_on_line) { g_ps_render_flags |= 0x40; break; }   /* too many on the line */
                out[count].attr = vram_word(satb + s + 4);
                {   /* widescreen, full x (PsWideGame.wide_x): 11-bit signed, else the hardware's 9 */
                    extern int ps_wide_full_x(void);
                    uint16_t xw = vram_word(satb + s + 6);
                    out[count].xpos = s_extra && ps_wide_full_x() ? ((xw & 0x7FF) ^ 0x400) - 0x400 : xw & 0x1FF;
                }
                out[count].yoff = yoff;
                out[count].size = size & 0x0F;
                count++;
            }
        }
        link = sat_word(s + 2) & 0x7F;
        if (link == 0 || link * 4 >= width) break;
    } while (--total);
    return count;
}

/* Draw sprites front-to-back into `lb` through `lut`. */
/* Sprite collision in the off-screen strips, as Genesis Plus GX's line buffer behaves: its
 * 32-px borders are never cleared, only plane B overwrites [shift-16, width+shift) of them
 * each line (fine scroll), so sprite pixels left there by earlier lines can still collide.
 * Kept apart from the drawn picture so widescreen margins do not change game logic. */
static uint8_t s_col_border[2][32];          /* [0]: x = -32..-1, [1]: x = width..width+31 */
static void col_border_line(int line, int width) {
    const PsVdp *d = &g_ps_vdp;
    static const uint8_t k_hmask[4] = { 0x00, 0x07, 0xF8, 0xFF };
    uint32_t ha = (((uint32_t)d->reg[13] << 10) & 0xFC00) + ((uint32_t)(line & k_hmask[d->reg[11] & 3]) << 2) + 2;
    int shift = vram_word(ha) & 0x0F;
    if (shift) for (int x = shift - 16; x < 0; x++) s_col_border[0][x + 32] = 0;
    for (int x = width; x < width + shift; x++) s_col_border[1][x - width] = 0;
}
static inline bool col_hit(uint8_t *dst, int sx, int width) {    /* collision test + mark */
    if (sx >= 0 && sx < width) return (*dst & 0x80) != 0;
    uint8_t *b = sx < 0 ? &s_col_border[0][sx + 32] : &s_col_border[1][sx - width];
    bool hit = *b; *b = 1; return hit;
}

/* Boss arena wall made of sprites (a pillar): its sprites (tile range from the title) inside
 * the source block are drawn again, repeated past the wall into the side area. */
static void draw_wall_sprites(uint8_t *lb, int line, const uint8_t *lut) {
    int t0, t1;
    if (!s_extra || !ps_wide_wall_tiles(&t0, &t1)) return;
    Obj objs[80];
    int count = collect_sprites(line, objs);
    for (int side = 0; side < 2; side++) {
        int wall, src, bw;
        if (!ps_wide_wall_source(side, &wall, &src, &bw)) continue;
        int lo = side ? wall : -s_extra, hi = side ? 320 + s_extra : wall;   /* side pixels */
        for (int i = count - 1; i >= 0; i--) {            /* back to front: the SAT order wins */
            const Obj *o = &objs[i];
            int tile0 = o->attr & 0x7FF, x = o->xpos - 0x80;
            int w = 8 + ((o->size & 0x0C) << 1), wt = w >> 3, ht = (o->size & 3) + 1;
            if (tile0 < t0 || tile0 > t1 || x < src || x + w > src + bw) continue;
            bool hf = o->attr & 0x0800, vf = o->attr & 0x1000;
            uint8_t atex = (uint8_t)((o->attr >> 9) & 0x70);
            int vrow = o->yoff >> 3;
            if (vf) vrow = (ht - 1) - vrow;
            for (int k = 1; k < 64; k++) {
                int dx = side ? wall + (k - 1) * bw - src : wall - k * bw - src;
                if (x + dx >= hi || x + dx + w <= lo) { if (side ? x + dx >= hi : x + dx + w <= lo) break; continue; }
                for (int c = 0; c < wt; c++) {
                    uint32_t tile = (o->attr + (uint32_t)(vrow + (hf ? (wt - 1) - c : c) * ht)) & 0x07FF;
                    uint8_t px[8];
                    ps_render_tile_row(tile, o->yoff & 7, hf, vf, px);
                    for (int p = 0; p < 8; p++) {
                        int sx = x + dx + c * 8 + p;
                        if (px[p] && sx >= lo && sx < hi) lb[ORG + sx] = lut[(lb[ORG + sx] << 8) | px[p] | atex];
                    }
                }
            }
        }
    }
}

static void draw_sprites(uint8_t *lb, int line, const uint8_t *lut) {
    const PsVdp *d = &g_ps_vdp;
    int width = (d->reg[12] & 1) ? 320 : 256;
    int max_pixels = s_sprite_limits ? width : 80 * 32;
    Obj objs[80];
    int count = collect_sprites(line, objs);
    int pixelcount = 0, masked = 0;
    /* widescreen: on window lines (a status bar) the side areas show the bar only - objects
     * placed past the 4:3 edges stop at it, as the 4:3 picture never shows them there */
    int wl = (d->reg[18] & 0x1F) << 3, bar = s_extra && ((d->reg[18] >> 7) & 1) == (line >= wl);

    for (int i = 0; i < count; i++) {
        const Obj *o = &objs[i];
        if (o->xpos) s_spr_ovr = 1;
        else if (s_spr_ovr) masked = 1;

        int x = o->xpos - 0x80;
        int w = 8 + ((o->size & 0x0C) << 1);
        int wt = w >> 3, ht = (o->size & 3) + 1;
        pixelcount += w;

        if (s_extra && (x < 0 || x + w > width) && x + w > -s_extra && x < width + s_extra) g_ps_side_sprites++;
        bool gpgx_drawn = x + w > 0 && x < width;     /* sprites the hardware line covers */
        if (x + w > -s_extra && x < width + s_extra && !masked) {
            int draw_w = w;
            if (pixelcount > max_pixels) draw_w -= pixelcount - max_pixels;
            int tiles = draw_w >> 3;
            bool hf = o->attr & 0x0800, vf = o->attr & 0x1000;
            uint8_t atex = (uint8_t)((o->attr >> 9) & 0x70);
            int vrow = o->yoff >> 3;
            if (vf) vrow = (ht - 1) - vrow;
            for (int c = 0; c < tiles; c++) {
                int vcol = hf ? (wt - 1) - c : c;
                uint32_t tile = (o->attr + (uint32_t)(vrow + vcol * ht)) & 0x07FF;
                uint8_t px[8];
                ps_render_tile_row(tile, o->yoff & 7, hf, vf, px);
                uint8_t *dst = lb + ORG + x + c * 8;
                for (int p = 0; p < 8; p++)
                    if (px[p] && !(bar && (x + c * 8 + p < 0 || x + c * 8 + p >= width))) {
                        int sx = x + c * 8 + p;                  /* flags as on the hardware's line */
                        if (gpgx_drawn && sx >= -32 && sx < width + 32 && col_hit(&dst[p], sx, width)) g_ps_render_flags |= 0x20;
                        dst[p] = lut[(dst[p] << 8) | px[p] | atex];
                    }
            }
        }
        if (s_sprite_limits && pixelcount >= max_pixels) {
            s_spr_ovr = (pixelcount >= width);
            return;
        }
    }
    s_spr_ovr = 0;
}

/* ------------------------------------------------------------------ */
/* Line composition                                                   */
/* ------------------------------------------------------------------ */

/* Widescreen: plane A pixels for screen x in [x0, x1) outside the original screen, from
 * world cells (cached when seen before, else the live plane). */
static void draw_plane_a_wide(uint8_t *buf, int line, int x0, int x1) {
    const PsVdp *d = &g_ps_vdp;
    static const uint8_t k_hmask[4] = { 0x00, 0x07, 0xF8, 0xFF };
    int camx, camy; ps_wide_camera(&camx, &camy);
    uint32_t hscb = ((uint32_t)d->reg[13] << 10) & 0xFC00;
    int hs = vram_word(hscb + ((uint32_t)(line & k_hmask[d->reg[11] & 3]) << 2)) & 0x3FF;
    int vs = d->vsram[0] & 0x3FF;
    /* this line's offsets, taken next to the game camera (plane wraps at 512 x 256) */
    int ox = camx + ((((-hs) - camx) % 512 + 512 + 256) % 512 - 256);
    int oy = camy + (((vs - camy) % 256 + 256 + 128) % 256 - 128);
    int wy = line + oy, row = wy & 7;
    int cy = (wy - row) / 8;
    for (int x = x0; x < x1; ) {
        int wx = x + ox, fine = wx & 7;
        int cx = (wx - fine) / 8;
        uint8_t px[8] = {0};
        uint16_t e;
        bool door = false;
        if (ps_wide_entry(cx, cy, ps_render_plane_entry, &e)) {
            uint8_t *keep = s_sd_buf; s_sd_buf = NULL;     /* marks go to the line, below */
            draw_entry(px, e, row);
            s_sd_buf = keep;
            door = s_water > 0 && buf == s_sd_buf && tile_screen_door(e & 0x7FF);
        }
        for (int i = fine; i < 8 && x < x1; i++, x++) { buf[ORG + x] = px[i]; if (door) s_sd_mark[ORG + x] = 1; }
    }
}

/* Widescreen background: plane B's side areas from the game's background layout (bg_cell),
 * each line at its own scroll (parallax bands), unwrapped next to the background camera. Each
 * frame the 4:3 part is checked against the live plane (PS_WIDE_STATS reports it; cells that
 * differ: 0 where the game draws from its layout). */
long g_ps_bg_cells, g_ps_bg_differ;
static long s_bg_frame = -1, s_bg_cells, s_bg_miss; static bool s_bg_off;
static void draw_plane_b_wide(uint8_t *buf, int line, int x0, int x1, int width) {
    const PsVdp *d = &g_ps_vdp;
    {   /* self-check: when the previous frame's background cells disagreed with the live plane
         * (the game drawing plane B from elsewhere: Angel Island's intro), use the live plane */
        extern long ps_frames;
        if (ps_frames != s_bg_frame) {
            s_bg_off = s_bg_miss > 8 && s_bg_miss * 20 > s_bg_cells;
            s_bg_frame = ps_frames; s_bg_cells = s_bg_miss = 0;
        }
    }
    static const uint8_t k_hmask[4] = { 0x00, 0x07, 0xF8, 0xFF };
    int bx, by; ps_wide_bg_camera(&bx, &by);
    uint32_t hscb = ((uint32_t)d->reg[13] << 10) & 0xFC00;
    int hs = vram_word(hscb + ((uint32_t)(line & k_hmask[d->reg[11] & 3]) << 2) + 2) & 0x3FF;
    int vs = d->vsram[1] & 0x3FF;
    int dx = (((-hs) - bx) % 512 + 512 + 256) % 512 - 256;
    if (dx < -4 || dx > 4) return;     /* a band scrolling on its own (water, clouds): the game uses
                                          the plane as a 512-px repeating texture - keep it wrapped */
    int ox = bx + dx;
    int oy = by + (((vs - by) % 256 + 256 + 128) % 256 - 128);
    int wy = line + oy, row = wy & 7, cy = (wy - row) / 8;
    for (int x = s_bg_off ? x1 : x0; x < x1; ) {
        int wx = x + ox, fine = wx & 7, cx = (wx - fine) / 8;
        uint8_t px[8] = {0};
        uint16_t e;
        if (ps_wide_bg_cell(cx, cy, &e)) { uint8_t *keep = s_sd_buf; s_sd_buf = NULL; draw_entry(px, e, row); s_sd_buf = keep; }
        else for (int i = 0; i < 8; i++) px[i] = buf[ORG + ((x - fine + i) % width + width) % width];   /* outside: as drawn */
        for (int i = fine; i < 8 && x < x1; i++, x++) buf[ORG + x] = px[i];
    }
    if (row == 0 && x0 < 0)                        /* once per cell row */
        for (int x = 0; x < width; x += 8) {
            int wx = x + ox, cx = (wx - (wx & 7)) / 8;
            uint16_t e;
            if (!ps_wide_bg_cell(cx, cy, &e)) continue;
            g_ps_bg_cells++; s_bg_cells++;
            uint16_t live = ps_render_plane_entry(1, (x - hs) >> 3, (line + vs) >> 3);
            if ((e & 0x7FF) != (live & 0x7FF)) {
                g_ps_bg_differ++; s_bg_miss++;
                static int shown; extern long ps_frames;
                if (getenv("PS_WIDE_BGLOG") && shown++ < 40) fprintf(stderr, "bg f%ld line %d x %d wx %d cx %d cy %d hs %d bx %d: map %04X plane %04X\n", ps_frames, line, x, wx, cx, cy, hs, bx, e, live);
            }
        }
}

/* Widescreen status bar: the left/right 16-px slices that repeat on every window line of
 * this frame (-1: none), found once per frame from the window name table. */
static int s_bar_line = -1, s_bar_lx = -1, s_bar_rx = -1;
static void bar_slices(int width, int wl, int wdown) {
    static uint8_t rows[256][LINE_MAX];
    int n = 0, h = ps_vdp_height();
    for (int l = 0; l < h && n < 256; l++)
        if (wdown == (l >= wl)) { memset(rows[n], 0, sizeof rows[n]); draw_window(rows[n++], l, 0, width >> 4); }
    s_bar_line = 1; s_bar_lx = s_bar_rx = -1;
    for (int x = 0; x + 32 <= width && s_bar_lx < 0; x += 8) {
        int ok = n > 0;
        for (int i = 0; i < n && ok; i++) ok = !memcmp(rows[i] + ORG + x, rows[i] + ORG + x + 16, 16);
        if (ok) s_bar_lx = x;
    }
    for (int x = width - 32; x >= 0 && s_bar_rx < 0; x -= 8) {
        int ok = n > 0;
        for (int i = 0; i < n && ok; i++) ok = !memcmp(rows[i] + ORG + x, rows[i] + ORG + x + 16, 16);
        if (ok) s_bar_rx = x + 16;
    }
}

static void render_line(int line, uint32_t *out) {
    const PsVdp *d = &g_ps_vdp;
    int width = ps_vdp_width();
    int E = s_extra;                       /* out[0] is screen x = -E */
    uint8_t lb[LINE_MAX], la[LINE_MAX], ls[LINE_MAX];

    if (!ps_vdp_display_enabled()) {
        uint32_t bd = ps_render_color(d->reg[7] & 0x3F, 1);
        for (int x = 0; x < width + 2 * E; x++) out[x] = bd;
        return;
    }

    bool sh = (d->reg[12] & 0x08) != 0;
    int cols = width >> 4;
    memset(lb, 0, sizeof(lb));
    memset(la, 0, sizeof(la));

    memset(s_sd_mark, 0, sizeof s_sd_mark);
    s_sd_buf = NULL;                       /* screen-door marks: plane A tiles (drawn over B) */
    /* Plane B (live, wraps like the hardware beyond the original screen) */
    int ec = (E + 15) / 16;
    draw_plane(lb, 1, line, -ec, cols + ec, false);
    if (E && ps_wide_bg()) {                       /* the background beyond the 512-px plane */
        draw_plane_b_wide(lb, line, -E, 0, width);
        draw_plane_b_wide(lb, line, width, width + E, width);
    }
    s_sd_buf = la;

    /* Plane A and window */
    int wl = (d->reg[18] & 0x1F) << 3;
    int wdown = (d->reg[18] >> 7) & 1;
    Clip clip[2];
    if (wdown == (line >= wl)) {
        draw_window(la, line, 0, cols);
        if (E) {
            /* widescreen: a full-width window (status bar) continues into the side areas by
             * tiling a 16-px slice that repeats on every line of the bar (plain bar, not
             * text: a per-line search tiled the power pills and score digits); else its edges */
            extern bool ps_wide_bar(int b[4]);
            int bs[4];
            uint8_t *w0 = la + ORG;
            if (ps_wide_bar(bs)) {             /* the game's split: ends move out, gaps filled */
                static uint8_t src[LINE_MAX];
                memcpy(src, w0, (size_t)width);
                for (int x = -E; x < width + E; x++) {
                    int sx = x < bs[0] - E ? x + E : x < bs[0] ? bs[1]
                           : x < bs[2] ? x : x < bs[2] + E ? bs[3] : x - E;
                    w0[x] = src[sx < 0 ? 0 : sx >= width ? width - 1 : sx];
                }
                goto window_done;
            }
            if (s_bar_line < 0) bar_slices(width, wl, wdown);
            int lx = s_bar_lx, rx = s_bar_rx;
            for (int x = -E; x < 0; x++)
                w0[x] = lx >= 0 ? w0[lx + ((x % 16) + 16) % 16] : w0[((x % 8) + 8) % 8];
            for (int x = width; x < width + E; x++)
                w0[x] = rx >= 0 ? w0[rx + (x - width) % 16] : w0[width - 8 + (x - width) % 8];
        }
        window_done:;
    } else {
        if (E) {                                   /* widescreen sides of plane A */
            /* lines whose vertical scroll differs from the frame's first line are raster
             * effects (HBlank-written VSRAM: COI's upside-down rooms): the world cache
             * cannot follow them, so their sides come from the live plane, scroll included */
            static uint32_t s_vs0; static long s_logged = -1;
            uint32_t vs = (uint32_t)d->vsram[0] << 16 | d->vsram[1];
            if (line == 0) s_vs0 = vs;
            if (vs != s_vs0) {
                int ecl = (E + 15) / 16;
                draw_plane(la, 0, line, -ecl, 0, false);
                draw_plane(la, 0, line, cols, cols + ecl, false);
                extern long ps_frames;
                if (getenv("PS_RASTER_LOG") && s_logged != ps_frames) { s_logged = ps_frames; fprintf(stderr, "raster sides f=%ld line %d\n", ps_frames, line); }
            } else {
                draw_plane_a_wide(la, line, -E, 0);
                draw_plane_a_wide(la, line, width, width + E);
            }
        }
        window_clip(clip, cols);
        if (clip[0].enable) draw_plane(la, 0, line, clip[0].left, clip[0].right, true);
        if (clip[1].enable) {
            /* the window overwrites the columns it covers, including any
             * spill from plane A's partial column */
            memset(la + ORG + clip[1].left * 16, 0, (size_t)(clip[1].right - clip[1].left) * 16);
            draw_window(la, line, clip[1].left, clip[1].right);
        }
    }

    s_sd_buf = NULL;
    static uint8_t pb[LINE_MAX], pa[LINE_MAX], merged[LINE_MAX];
    if (s_water) { memcpy(pb, lb, sizeof pb); memcpy(pa, la, sizeof pa); }
    /* Merge B and A */
    const uint8_t *lut_bg = sh ? s_lut_bg_sh : s_lut_bg;
    for (int x = -E; x < width + E; x++)
        lb[ORG + x] = lut_bg[(lb[ORG + x] << 8) | la[ORG + x]];
    if (E && wdown != (line >= wl))                /* boss arena: a block of the room past a wall */
        for (int side = 0; side < 2; side++) {
            int wx, sx, w;
            if (!ps_wide_wall_source(side, &wx, &sx, &w)) continue;
            if (side == 0) for (int x = (wx < 0 ? wx : 0) - 1; x >= -E; x--) lb[ORG + x] = lb[ORG + sx + ((x - wx) % w + w) % w];
            else for (int x = wx > width ? wx : width; x < width + E; x++) lb[ORG + x] = lb[ORG + sx + (x - wx) % w];
        }
    /* boss arena: wall art from an earlier room (drawn over the planes, under the sprites) */
    bool art = false;
    uint8_t pre[LINE_MAX];
    if (E && wdown != (line >= wl))
        for (int side = 0; side < 2; side++) {
            int ax, aw; const uint32_t *row;
            if (ps_wide_art_row(side, line, &ax, &aw, &row)) { art = true; memcpy(pre, lb, sizeof pre); break; }
        }

    if (s_water) memcpy(merged, lb, sizeof merged);
    /* Sprites */
    col_border_line(line, width);
    if (sh) {
        memset(ls, 0, sizeof(ls));
        draw_wall_sprites(ls, line, s_lut_obj);
        draw_sprites(ls, line, s_lut_obj);
        for (int x = -E; x < width + E; x++)
            lb[ORG + x] = s_lut_bgobj_sh[(lb[ORG + x] << 8) | ls[ORG + x]];
    } else {
        draw_wall_sprites(lb, line, s_lut_bgobj);
        draw_sprites(lb, line, s_lut_bgobj);
    }

    /* Left-most 8 pixels blanked to the backdrop */
    if ((d->reg[0] & 0x20) && !E) memset(lb + ORG, 0x40, 8);   /* hides scroll seams: not needed when widened */

    /* Colour */
    uint32_t pal[3][64];
    int border = d->reg[7] & 0x3F;
    for (int k = 0; k < 3; k++) {
        for (int i = 0; i < 64; i++) pal[k][i] = ps_render_color(i, k);
        pal[k][0] = ps_render_color(border, k);
    }
    /* Real water (PS_WATER, settings menu): games draw water by swapping the palette
     * mid-screen (the lines below the surface use an underwater palette). Instead, those
     * lines are drawn with the frame's above-water palette and the underwater colour is
     * laid over it at PS_WATER_ALPHA (0.5): the water is see-through. A swap is a mid-frame
     * change of at least 16 colours from the palette at line 0 (fades change between
     * frames). Picture only: the game's logic is untouched. */
    if (line == 0) memcpy(s_pal0, d->cram, sizeof s_pal0);
    else if (s_water) {
        int changed = 0;
        for (int i = 0; i < 64; i++) changed += d->cram[i] != s_pal0[i];
        if (changed >= s_water_min) {
            if (s_water_log && s_water_last != ps_frame_no()) { s_water_last = ps_frame_no(); fprintf(stderr, "water: frame %ld from line %d (%d colours)\n", s_water_last, line, changed); }
            PsVdp tmp = *d;                       /* above-water colours: the line-0 palette */
            memcpy(tmp.cram, s_pal0, sizeof s_pal0);
            const PsVdp *keep = s_vdp_override; s_vdp_override = &tmp;
            uint32_t above[3][64];
            for (int k = 0; k < 3; k++) {
                for (int i = 0; i < 64; i++) above[k][i] = ps_render_color(i, k);
                above[k][0] = ps_render_color(border, k);
            }
            s_vdp_override = keep;
            int a = (int)(s_water_alpha * 256);
            for (int k = 0; k < 3; k++) for (int i = 0; i < 64; i++) {
                uint32_t u = pal[k][i], o = above[k][i], r = 0xFF000000u;
                for (int sh2 = 0; sh2 < 24; sh2 += 8) {
                    int cu = (u >> sh2) & 0xFF, co = (o >> sh2) & 0xFF;
                    r |= (uint32_t)((co * (256 - a) + cu * a) >> 8) << sh2;
                }
                pal[k][i] = r;
            }
        }
    }
    for (int x = -E; x < width + E; x++) {
        uint8_t c = lb[ORG + x];
        int k = sh ? (c >> 6) : 1;
        if (k > 2) k = 2;
        int idx = c & 0x3F;
        out[x + E] = pal[k][(idx & 0x0F) ? idx : 0];
    }
    /* Modern water (PS_WATER), pass 1: record where the game draws a waterfall as a
     * "screen door" (plane A tiles marked by draw_entry) and the sharp colour behind it;
     * ps_render_water_frame() draws the water over the whole frame at its end. */
    if (s_water && line < 256) {
        int n = width + 2 * E;
        for (int x = 0; x < n && x < 1024; x++) {
            int o = ORG - E + x;
            uint8_t m = 0;
            if (lb[o] != merged[o]) m = 2;                              /* a sprite: keep it */
            else if (s_sd_mark[o]) {
                m = 1;
                s_wbehind[line][x] = pal[1][(pb[o] & 15) ? (pb[o] & 0x3F) : 0];
                if (pa[o] & 15) {                                       /* the game's light water colour */
                    uint32_t c = pal[1][pa[o] & 0x3F];
                    int l = (int)((c >> 16) & 0xFF) * 3 + (int)((c >> 8) & 0xFF) * 6 + (int)(c & 0xFF);
                    if (l > s_wlight_lum) { s_wlight_lum = l; s_wlight = c; }
                }
            }
            s_wmask[line][x] = m;
        }
        /* a waterfall is wide: screen-door runs under 3 tiles on a line are other dithering
         * (COI's spider threads in the web level), not water */
        for (int x = 0; x < n && x < 1024; ) {
            if (s_wmask[line][x] != 1) { x++; continue; }
            int e = x; while (e < n && e < 1024 && s_wmask[line][e] == 1) e++;
            if (e - x < 24) memset(&s_wmask[line][x], 0, (size_t)(e - x));
            x = e;
        }
        s_wwidth = n;
    }
    for (int side = 0; art && side < 2; side++) {   /* side areas only, where no sprite drew */
        int ax, aw; const uint32_t *row;
        if (!ps_wide_art_row(side, line, &ax, &aw, &row)) continue;
        int lo = side ? width : -E, hi = side ? width + E : 0;
        for (int i = 0; i < aw; i++) {
            int x = ax + i;
            if (x >= lo && x < hi && (row[i] >> 24) && lb[ORG + x] == pre[ORG + x]) out[x + E] = row[i];
        }
    }
}

/* Modern water, pass 2 (end of frame): the waterfall columns (water on at least 1/5 of
 * the lines) become one band, holes closed between the first and last water line of each
 * column, edges feathered; a procedural sheet is composited over the sharp scene: soft
 * streaks varying smoothly across columns and falling continuously, foam at the foot. */
static float vnoise(float x) {                 /* smooth 1D value noise */
    int i = (int)floorf(x); float f = x - (float)i;
    uint32_t a = (uint32_t)i * 2654435761u, b = (uint32_t)(i + 1) * 2654435761u;
    float va = (float)(a >> 24) / 255.0f, vb = (float)(b >> 24) / 255.0f;
    f = f * f * (3 - 2 * f);
    return va + (vb - va) * f;
}
static float hash2(int x, int y) {
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return (float)((h ^ (h >> 16)) & 0xFFFF) / 65535.0f;
}
static float vnoise2(float x, float y) {         /* smooth 2D value noise */
    int ix = (int)floorf(x), iy = (int)floorf(y);
    float fx = x - (float)ix, fy = y - (float)iy;
    fx = fx * fx * (3 - 2 * fx); fy = fy * fy * (3 - 2 * fy);
    float a = hash2(ix, iy), b = hash2(ix + 1, iy), c = hash2(ix, iy + 1), d = hash2(ix + 1, iy + 1);
    return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fy;
}
/* colour ramp of the water: deep, mid, cyan, white */
static void water_ramp(float v, float *r, float *g, float *b) {
    static const float k[4][3] = { { 10, 42, 72 }, { 38, 104, 156 }, { 150, 210, 238 }, { 250, 254, 255 } };
    static const float at[4] = { 0.0f, 0.42f, 0.78f, 1.0f };
    if (v < 0) v = 0;
    if (v > 1) v = 1;
    int i = v < at[1] ? 0 : v < at[2] ? 1 : 2;
    float f = (v - at[i]) / (at[i + 1] - at[i]);
    *r = k[i][0] + (k[i + 1][0] - k[i][0]) * f; *g = k[i][1] + (k[i + 1][1] - k[i][1]) * f; *b = k[i][2] + (k[i + 1][2] - k[i][2]) * f;
}
static void blend_px(uint32_t *px, float r, float g, float b, float A, uint32_t behind) {
    float br = (float)((behind >> 16) & 0xFF), bg = (float)((behind >> 8) & 0xFF), bb = (float)(behind & 0xFF);
    int R = (int)(r * A + br * (1 - A)), G = (int)(g * A + bg * (1 - A)), B = (int)(b * A + bb * (1 - A));
    *px = 0xFF000000u | (uint32_t)(R > 255 ? 255 : R) << 16 | (uint32_t)(G > 255 ? 255 : G) << 8 | (uint32_t)(B > 255 ? 255 : B);
}
void ps_render_water_frame(uint32_t *buf, int pitch, int height) {
    if (!s_water || s_wlight_lum < 0) { s_wlight_lum = -1; return; }
    extern long ps_frames;
    int n = s_wwidth, h = height < 256 ? height : 256;
    static int top[1024], bot[1024], cnt[1024];
    static uint8_t band[1024];
    for (int x = 0; x < n; x++) {
        cnt[x] = 0; top[x] = -1; bot[x] = -1;
        for (int y = 0; y < h; y++) if (s_wmask[y][x] == 1) { cnt[x]++; if (top[x] < 0) top[x] = y; bot[x] = y; }
        band[x] = cnt[x] >= h / 5;
    }
    /* close one- and two-column gaps inside the band */
    for (int x = 1; x < n - 2; x++) if (!band[x] && band[x - 1] && (band[x + 1] || band[x + 2])) { band[x] = 1; top[x] = top[x - 1]; bot[x] = bot[x - 1]; }
    float t = (float)ps_frames;
    for (int x = -6; x < n + 6; x++) {
        /* inside the band, or spray up to 6 px outside it */
        int src = x, dist = 0;
        if (x < 0 || x >= n || !band[x]) {
            int best = 99;
            for (int d = 1; d <= 6; d++) {
                if (x - d >= 0 && x - d < n && band[x - d]) { best = d; src = x - d; break; }
                if (x + d >= 0 && x + d < n && band[x + d]) { best = d; src = x + d; break; }
            }
            if (best > 6 || x < 0 || x >= n) continue;
            dist = best;
        }
        int yt = top[src], yb = bot[src];
        for (int y = yt; y <= yb; y++) {
            if (y < 0 || y >= h || s_wmask[y][x] == 2) continue;             /* sprites stay in front */
            /* foam: rows where the game's water stops (the fall hits something) */
            float foam = 0;
            for (int d = -3; d <= 3; d++) {
                int yy = y + d;
                if (yy > 0 && yy < h && s_wmask[yy - 1][src] == 1 && s_wmask[yy][src] != 1) foam += (float)(4 - (d < 0 ? -d : d)) / 4.0f;
            }
            if (foam > 1) foam = 1;
            float fy = (float)y - t * 3.2f;                           /* falling */
            float v = vnoise2((float)x * 0.8f, fy * 0.06f) * 0.5f + vnoise2((float)x * 2.1f + 31, fy * 0.13f) * 0.32f
                    + vnoise2((float)x * 3.7f + 7, fy * 0.3f) * 0.18f;
            v = (v - 0.28f) * 1.9f;                                    /* contrast: clumps of light streaks */
            float sparkle = hash2(x, (int)fy) > 0.955f ? 0.45f : 0;
            v += sparkle + foam * 0.9f;
            float A = 0.9f;
            if (dist) {                                                /* spray: sparse droplets, denser at the foam */
                float p = (1.0f - (float)dist / 7.0f) * (0.12f + 0.25f * v) + foam * 0.5f;
                if (hash2(x * 7 + 3, (int)(fy * 0.5f)) > p) continue;
                v = 0.8f + 0.2f * hash2(x, y); A = 0.55f + 0.3f * foam;
            }
            float r, g, b; water_ramp(v, &r, &g, &b);
            uint32_t *px = buf + (long)y * pitch + x;
            uint32_t behind = (x >= 0 && x < n && s_wmask[y][x] == 1) ? s_wbehind[y][x] : *px;
            blend_px(px, r, g, b, A, behind);
        }
    }
    s_wlight_lum = -1;
}

void ps_render_lines(int y0, int y1, uint32_t *dst, int pitch) {
    if (s_water < 0) {
        const char *w = getenv("PS_WATER");
        s_water = comparing() || !s_water_allowed ? 0 : w ? *w != '0' : 1;   /* on unless PS_WATER=0; the menu sets it */
        s_water_log = getenv("PS_WATER_LOG") != NULL;
        if (s_water_log) s_water = 1;
        if (getenv("PS_WATER_ALPHA")) s_water_alpha = atof(getenv("PS_WATER_ALPHA"));
        if (getenv("PS_WATER_MIN")) s_water_min = atoi(getenv("PS_WATER_MIN"));
    }
    s_extra = ps_wide_extra();
    if (y0 == 0) s_bar_line = -1;                  /* new frame: find the bar slices again */
    for (int y = y0; y < y1; y++) render_line(y, dst + (long)y * pitch);
}

void ps_render_frame(uint32_t *dst, int pitch) {
    ps_render_lines(0, ps_vdp_height(), dst, pitch);
}

/* ------------------------------------------------------------ sprite extraction
 * PS_SPRITES_DIR=dir: every distinct sprite on screen (all its tiles, its palette line),
 * unflipped, as dir/<hash>.png (transparent = #FF00FF) + a line in dir/sprites.csv.
 * hash = FNV-1a 64 of the pixel indices and the 16 palette colours: the key an HD
 * replacement pack uses. ROM-derived output: never committed. */
#include "ps_png.h"
#include <stdio.h>
#include <stdlib.h>
void ps_render_dump_sprites(const char *dir, long frame) {
    static uint64_t seen[1 << 16]; static int nseen;
    const PsVdp *d = &g_ps_vdp;
    bool h40 = d->reg[12] & 1;
    uint32_t satb = ((uint32_t)d->reg[5] << 9) & (h40 ? 0xFC00 : 0xFE00);
    int link = 0, guard = 0;
    do {
        uint32_t sa = (uint32_t)link * 8;
        uint8_t size = (uint8_t)(sat_word(sa + 2) >> 8) & 0x0F;
        uint16_t attr = vram_word(satb + sa + 4);
        int wt = ((size >> 2) & 3) + 1, ht = (size & 3) + 1, w = wt * 8, h = ht * 8;
        int pal = (attr >> 13) & 3;
        uint8_t idx[32 * 32];
        uint64_t hsh = 1469598103934665603ull;
        for (int y = 0; y < h; y++)
            for (int c = 0; c < wt; c++) {
                uint8_t px[8];
                ps_render_tile_row((attr + (uint32_t)((y >> 3) + c * ht)) & 0x7FF, y & 7, false, false, px);
                for (int p = 0; p < 8; p++) { idx[y * w + c * 8 + p] = px[p]; hsh = (hsh ^ px[p]) * 1099511628211ull; }
            }
        uint32_t col[16];
        for (int i = 0; i < 16; i++) { col[i] = ps_render_color(pal * 16 + i, 1); hsh = (hsh ^ col[i]) * 1099511628211ull; }
        bool any = false;
        for (int i = 0; i < w * h; i++) if (idx[i]) { any = true; break; }
        bool dup = !any;
        for (int i = 0; i < nseen && !dup; i++) dup = seen[i] == hsh;
        if (!dup && nseen < (1 << 16)) {
            seen[nseen++] = hsh;
            char path[1024]; snprintf(path, sizeof path, "%s/%016llx.png", dir, (unsigned long long)hsh);
            FILE *t = fopen(path, "rb");
            if (t) fclose(t);
            else {
                uint32_t img[32 * 32];
                for (int i = 0; i < w * h; i++) img[i] = idx[i] ? col[idx[i]] : 0xFF00FF;
                ps_png_write(path, (const uint8_t *)img, w, h);
                snprintf(path, sizeof path, "%s/sprites.csv", dir);
                FILE *f = fopen(path, "a");
                if (f) { fprintf(f, "%016llx,%dx%d,tile=%03X,pal=%d,frame=%ld\n", (unsigned long long)hsh, w, h, attr & 0x7FF, pal, frame); fclose(f); }
            }
        }
        link = sat_word(sa + 2) & 0x7F;
    } while (link && ++guard < 80);
}

/* ps_state.c: render state carried between lines/frames that the game can observe (sprite
 * flags) or that the next frame's picture depends on */
void ps_state_blob(void *p, unsigned long n);
void ps_render_state(void) {
#define B(v) ps_state_blob(&(v), sizeof (v))
    B(s_spr_ovr); B(g_ps_render_flags); B(s_pal0);
#undef B
}
