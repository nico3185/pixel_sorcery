/*
 * PixelSorcery native renderer: draws the Genesis display from ps_vdp state.
 *
 * Mode 5 (Genesis) only: planes A/B (32/64/128-cell sizes), window plane,
 * sprites (link list, sizes, flips, priority, per-line limits and masking),
 * full / per-cell / per-line horizontal scroll, full / 2-cell vertical scroll,
 * H32/H40, shadow/highlight, backdrop, display enable, left-column blank.
 *
 * Output is XRGB8888. Layers are composed per line through small priority
 * tables, so the same pipeline can later draw wider lines (16:9 look-ahead)
 * or swap tile fetches for HD replacements (ps_render_tile_row is the single
 * place pattern pixels are read).
 */
#ifndef PS_RENDER_H
#define PS_RENDER_H

#include <stdint.h>
#include <stdbool.h>

void ps_render_init(void);

/* Draw lines [y0, y1) of the active display into `dst` (row stride `pitch`
 * pixels). Lines are ps_vdp_width() pixels wide. */
void ps_render_lines(int y0, int y1, uint32_t *dst, int pitch);

/* Whole active display (ps_vdp_width() x ps_vdp_height()). */
void ps_render_frame(uint32_t *dst, int pitch);

/* Per-line sprite limits (20 sprites / 320 px per line in H40). On by default:
 * that is what the hardware does and some games rely on it for effects. */
void ps_render_set_sprite_limits(bool on);

/* Sprite colours past the palette (native only, owner: "palette expansion" for the Mac/PC builds): sprite
 * pixels from tiles [tile_lo, tile_hi) drawn with palette line 0 take, for colour slot pairs[i][0],
 * pairs[i][2] where the line's colour there is pairs[i][1] (CRAM words; another value - a fade - scales the
 * slot's first pair the same way). gate() says when it applies (NULL: always); off while comparing with GPGX.
 * SHK1: his own colours, nothing else sharing Sonic's slots changes. */
void ps_render_set_sprite_colours(int tile_lo, int tile_hi, const uint16_t (*pairs)[3], int n, bool (*gate)(void));
/* more tiles with the same colours, while gate() (up to 4; e.g. a face pasted into shared VRAM) */
void ps_render_add_sprite_tiles(int tile_lo, int tile_hi, bool (*gate)(void));

/* Pattern fetch: 8 pixel colour indices (0-15) of one row of `tile`,
 * honouring flips. Single entry point for tile data (HD replacement hook). */
void ps_render_tile_row(uint32_t tile, int row, bool hflip, bool vflip, uint8_t out[8]);

/* Name-table entry of plane 0 (A) or 1 (B) at world cell (cx, cy), wrapped to
 * the plane size. Helper for world-space (look-ahead) rendering. */
uint16_t ps_render_plane_entry(int plane, int cx, int cy);

/* Colour of CRAM index (0-63) at intensity 0 shadow / 1 normal / 2 highlight. */
uint32_t ps_render_color(int index, int intensity);

extern uint16_t g_ps_render_flags;   /* sprite overflow ($40) / collision ($20) since last read */
#endif /* PS_RENDER_H */
