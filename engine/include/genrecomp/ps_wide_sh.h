/* ps_wide_sh.h -- widescreen hooks shared by the SH titles (one engine family each:
 * SH1; SH2; SH3 / SHK / SH3K). Include once from a
 * title's main.c and pass PS_WIDE_SONIC1/2/3(name) to ps_wide_set_game().
 *
 * RAM (from the community disassemblies, checked here against plane A's scroll):
 *   game mode $FFF600: $08 demo, $0C level ($88/$8C while the level loads: not widened)
 *   zone and act $FFFE10 (bytes)
 *   camera: SH1 $FFFF10/$FFFF14 (the copy VBlank draws from), SH2 $FFEE60/$FFEE64,
 *   SH3 engine $FFEE78/$FFEE7C; all snapped to plane A's scroll on screen (the RAM copy
 *   can be a frame away from the picture)
 * Level map in RAM, plane A = foreground:
 *   SH1: layout $FFA400, rows of $80 bytes (FG $40 then BG $40, 8 rows), a byte per
 *     256x256 chunk (low 7 bits, 0 = empty: block 0, chunk n at $FF0000 + (n-1)*$200: 16x16 words);
 *     chunk word: block (low 10 bits), x flip $0800, y flip $1000; blocks at $FFB000, 8 bytes
 *     (four name-table words TL TR BL BR).
 *   SH2: layout $FF8000, rows of $100 bytes (FG $80 then BG $80, 16 rows), a byte per
 *     128x128 chunk (chunk n at $FF0000 + n*$80: 8x8 words); chunk word: block (low 10
 *     bits), x flip $0400, y flip $0800; blocks at $FF9000, 8 bytes.
 *   SH3 engine: layout header $FF8000 (FG width, BG width, FG height, BG height, words)
 *     then row pointers from $FF8008 (FG, BG word pairs: RAM offsets of the row's bytes);
 *     chunks and blocks as SH2.
 * Sprites: the sprite builders' culls widen to the side areas and keep the full sprite x
 * (wide_x; hooks in each title's recomp.json).
 * A block flipped in a chunk swaps its tiles and flips each (name-table bits $0800 x,
 * $1000 y). */
#ifndef PS_WIDE_SONIC_H
#define PS_WIDE_SONIC_H
#include <genrecomp/bus.h>
#include <genrecomp/ps_wide.h>
#include <genrecomp/ps_vdp.h>

/* level or demo, not while loading, and not SH2's split screen (interlace mode 2) */
static bool sh_wide_active(void) {
    uint8_t m = bus_ram_read8(0xF600);
    return (m == 0x08 || m == 0x0C) && (g_ps_vdp.reg[12] & 0x06) != 0x06;
}
/* the game's own pause (Game_paused $FFF63A, in a level): the settings menu opens on C */
static bool sh_paused(void) { uint8_t m = bus_ram_read8(0xF600); return (m == 0x08 || m == 0x0C) && bus_ram_read16(0xF63A) != 0; }
static uint32_t sh_wide_map(void) { return bus_ram_read16(0xFE10); }
/* PS_WIDE_STATS: objects that appear inside the wide view (pop-in), counted once per frame from
 * the object slots (base, stride, count, x offset) - the measure for the object load windows */
#include <stdio.h>
#include <stdlib.h>
static long s_sh_popin, s_sh_spawn;
static void sh_popin_report(void) { fprintf(stderr, "wide: objects spawned %ld, appearing at the wide view's edges %ld\n", s_sh_spawn, s_sh_popin); }
static void sh_popin_scan(uint16_t base, int stride, int count, int xoff, int camx) {
    static int on = -1; static uint8_t prev[256]; static long last = -1; extern long ps_frames;
    if (on < 0) { on = getenv("PS_WIDE_STATS") != NULL; if (on) atexit(sh_popin_report); }
    if (!on || ps_frames == last || count > 256) return;
    last = ps_frames;
    int ps_wide_max_extra(void);
    int e = sh_wide_active() ? ps_wide_max_extra() : 0;
    if (!e) { for (int i = 0; i < count && i < 256; i++) { uint16_t o = (uint16_t)(base + i * stride); prev[i] = (bus_ram_read16(o) | bus_ram_read16((uint16_t)(o + 2))) != 0; } return; }
    for (int i = 0; i < count; i++) {
        uint16_t o = (uint16_t)(base + i * stride);
        uint8_t id = (bus_ram_read16(o) | bus_ram_read16((uint16_t)(o + 2))) != 0;   /* id byte or code pointer */
        if (id && !prev[i]) {
            int x = (int16_t)bus_ram_read16((uint16_t)(o + xoff)) - camx;
            s_sh_spawn++;
            if ((x >= 320 + e - 96 && x < 320 + e) || (x > -e && x <= -e + 96)) {   /* at the view's edges */
                s_sh_popin++;
                fprintf(stderr, "wide: pop-in f%ld slot $%04X object $%08X x %+d\n", ps_frames, o, (unsigned)(bus_ram_read16(o) << 16 | bus_ram_read16((uint16_t)(o + 2))), x);
            }
        }
        prev[i] = id;
    }
}
/* PS_WIDE_STATS: rings the ring manager keeps out of its window although they sit in the wide
 * view (SH2 engine: layout $FFE800, 6-byte entries {state, x, y} sorted by x, window
 * [$FFF712, $FFF714)) - frames x rings, the measure for the ring window hooks */
static long s_sh_ringmiss, s_sh_ringside;
static void sh_ringmiss_report(void) { fprintf(stderr, "wide: ring-frames in the side areas %ld, missing (outside the ring window) %ld\n", s_sh_ringside, s_sh_ringmiss); }
static void sh2_ring_scan(void) {
    static int on = -1; static long last = -1; extern long ps_frames;
    if (on < 0) { on = getenv("PS_WIDE_STATS") != NULL; if (on) atexit(sh_ringmiss_report); }
    int ps_wide_max_extra(void);
    int e = sh_wide_active() ? ps_wide_max_extra() : 0;
    if (!on || !e || ps_frames == last) return;
    last = ps_frames;
    int cx = (int16_t)bus_ram_read16(0xEE00), cy = (int16_t)bus_ram_read16(0xEE04);
    uint16_t lo = bus_ram_read16(0xF712), hi = bus_ram_read16(0xF714);
    for (uint16_t a = 0xE802; a < 0xEE00; a += 6) {
        int x = (int16_t)bus_ram_read16(a) - cx, y = (int16_t)bus_ram_read16((uint16_t)(a + 2)) - cy;
        if ((uint16_t)(x + cx) == 0xFFFF) break;                         /* end of the layout */
        if (!bus_ram_read16((uint16_t)(a - 2)) && (x < -8 || x >= 328) && x >= -e - 8 && x < 320 + e + 8 && y >= -8 && y < 232) s_sh_ringside++;
        if (bus_ram_read16((uint16_t)(a - 2)) || a < lo || a >= hi) {
            if (!bus_ram_read16((uint16_t)(a - 2)) && x >= -e - 8 && x < 320 + e + 8 && y >= -8 && y < 232)
                s_sh_ringmiss++;
        }
    }
}
/* SH3 engine: the level's ring list in ROM, 4-byte entries {x, y} sorted by x, window
 * [$FFEE42, $FFEE46) (ROM pointers, collected rings' state only inside it): the rings just
 * outside the window on either side that sit in the wide view */
static void sh3_ring_scan(void) {
    static int on = -1; static long last = -1; extern long ps_frames;
    if (on < 0) { on = getenv("PS_WIDE_STATS") != NULL; if (on) atexit(sh_ringmiss_report); }
    int ps_wide_max_extra(void);
    int e = sh_wide_active() ? ps_wide_max_extra() : 0;
    if (!on || !e || ps_frames == last) return;
    last = ps_frames;
    int cx = (int16_t)bus_ram_read16(0xEE78), cy = (int16_t)bus_ram_read16(0xEE7C);
    uint32_t lo = (uint32_t)bus_ram_read16(0xEE42) << 16 | bus_ram_read16(0xEE44);
    uint32_t hi = (uint32_t)bus_ram_read16(0xEE46) << 16 | bus_ram_read16(0xEE48);
    if (lo >= 0x400000 || hi >= 0x400000 || hi < lo) return;
    for (int side = 0; side < 2; side++)
        for (int i = 0; i < 64; i++) {
            uint32_t a = side ? hi + i * 4u : lo - (i + 1) * 4u;
            uint16_t rx = bus_read16(a);
            if (rx == 0xFFFF || (!side && rx == 0)) break;            /* the list's end markers */
            int x = (int16_t)rx - cx, y = (bus_read16(a + 2) & 0x0FFF) - cy;
            if (side ? x >= 320 + e + 8 : x < -e - 8) break;
            if (y >= -8 && y < 232) { s_sh_ringmiss++; s_sh_ringside++; }
        }
    for (uint32_t a = lo; a < hi && a < lo + 0x400; a += 4) {        /* in the window: the side areas */
        int x = (int16_t)bus_read16(a) - cx, y = (bus_read16(a + 2) & 0x0FFF) - cy;
        if ((x < -8 || x >= 328) && x >= -e - 8 && x < 320 + e + 8 && y >= -8 && y < 232) s_sh_ringside++;
    }
}
static void sh1_wide_camera(int *x, int *y) {
    *x = (int16_t)bus_ram_read16(0xFF10); *y = (int16_t)bus_ram_read16(0xFF14);
    ps_wide_snap_camera(x, y);
    sh_popin_scan(0xD800, 0x40, 96, 8, *x);           /* level objects: slots $FFD800-$FFEFFF */
}
static void sh2_wide_camera(int *x, int *y) {
    *x = (int16_t)bus_ram_read16(0xEE60); *y = (int16_t)bus_ram_read16(0xEE64);
    ps_wide_snap_camera(x, y);
    sh_popin_scan(0xB000, 0x40, 128, 8, *x);          /* objects: slots $FFB000-$FFCFFF */
    sh2_ring_scan();
}
static void sh3_wide_camera(int *x, int *y) {
    *x = (int16_t)bus_ram_read16(0xEE78); *y = (int16_t)bus_ram_read16(0xEE7C);
    ps_wide_snap_camera(x, y);
    sh_popin_scan(0xB000, 0x4A, 110, 0x10, *x);       /* objects: $4A-byte slots from $FFB000 */
    sh3_ring_scan();
}

/* the name-table word of 8x8 cell (tx, ty) inside block b, the block flipped by fx/fy */
static uint16_t sh_block_cell(uint16_t blocks, unsigned b, int tx, int ty, int fx, int fy) {
    int sx = fx ? 1 - tx : tx, sy = fy ? 1 - ty : ty;
    uint16_t e = bus_ram_read16((uint16_t)(blocks + b * 8u + (sy * 2 + sx) * 2));
    return e ^ (fx ? 0x0800 : 0) ^ (fy ? 0x1000 : 0);
}

static bool sh1_world_cell(int cx, int cy, uint16_t *e) {
    int x = cx * 8, y = cy * 8;
    if (x < 0 || y < 0 || x >= 0x40 * 256 || y >= 8 * 256) return false;
    unsigned n = bus_ram_read8((uint16_t)(0xA400 + (y >> 8) * 0x80 + (x >> 8))) & 0x7F;
    uint16_t w = n ? bus_ram_read16((uint16_t)((n - 1) * 0x200u + ((y >> 4) & 15) * 32 + ((x >> 4) & 15) * 2))
                   : 0;                              /* empty chunk: block 0 */
    *e = sh_block_cell(0xB000, w & 0x3FF, (x >> 3) & 1, (y >> 3) & 1, (w & 0x0800) != 0, (w & 0x1000) != 0);
    return true;
}

/* SH1's background: the second half of each layout row ($FFA440 + row * $80), same chunks
 * and blocks as the foreground; background camera $FFF708/$FFF70C */
static bool sh1_bg_cell(int cx, int cy, uint16_t *e) {
    int x = cx * 8, y = cy * 8;
    if (x < 0 || y < 0) return false;
    unsigned n = bus_ram_read8((uint16_t)(0xA440 + ((y >> 8) & 7) * 0x80 + ((x >> 8) & 0x3F))) & 0x7F;
    uint16_t w = n ? bus_ram_read16((uint16_t)((n - 1) * 0x200u + ((y >> 4) & 15) * 32 + ((x >> 4) & 15) * 2)) : 0;
    *e = sh_block_cell(0xB000, w & 0x3FF, (x >> 3) & 1, (y >> 3) & 1, (w & 0x0800) != 0, (w & 0x1000) != 0);
    return true;
}
static void sh1_bg_camera(int *x, int *y) { *x = (int16_t)bus_ram_read16(0xF708); *y = (int16_t)bus_ram_read16(0xF70C); }

static bool sh2_world_cell(int cx, int cy, uint16_t *e) {
    int x = cx * 8, y = cy * 8;
    if (x < 0 || y < 0 || x >= 0x80 * 128 || y >= 16 * 128) return false;
    unsigned n = bus_ram_read8((uint16_t)(0x8000 + (y >> 7) * 0x100 + (x >> 7)));
    uint16_t w = bus_ram_read16((uint16_t)(n * 0x80u + ((y >> 4) & 7) * 16 + ((x >> 4) & 7) * 2));
    *e = sh_block_cell(0x9000, w & 0x3FF, (x >> 3) & 1, (y >> 3) & 1, (w & 0x0400) != 0, (w & 0x0800) != 0);
    return true;
}

/* SH2's background: the second half of each layout row ($FF8080 + row * $100) */
static bool sh2_bg_cell(int cx, int cy, uint16_t *e) {
    int x = cx * 8, y = cy * 8;
    if (x < 0 || y < 0) return false;
    unsigned n = bus_ram_read8((uint16_t)(0x8080 + ((y >> 7) & 15) * 0x100 + ((x >> 7) & 0x7F)));
    uint16_t w = bus_ram_read16((uint16_t)(n * 0x80u + ((y >> 4) & 7) * 16 + ((x >> 4) & 7) * 2));
    *e = sh_block_cell(0x9000, w & 0x3FF, (x >> 3) & 1, (y >> 3) & 1, (w & 0x0400) != 0, (w & 0x0800) != 0);
    return true;
}
static void sh2_bg_camera(int *x, int *y) { *x = (int16_t)bus_ram_read16(0xEE08); *y = (int16_t)bus_ram_read16(0xEE0C); }   /* Camera_BG_X/Y_pos */

static bool sh3_world_cell(int cx, int cy, uint16_t *e) {
    int x = cx * 8, y = cy * 8;
    int fw = bus_ram_read16(0x8000), fh = bus_ram_read16(0x8004);
    if (x < 0 || y < 0 || fw <= 0 || fh <= 0 || fw > 0x100 || fh > 0x40) return false;
    int row = y >> 7, col = x >> 7;
    if (col >= fw) return false;
    row %= fh;                                       /* levels that wrap vertically */
    uint16_t rp = bus_ram_read16((uint16_t)(0x8008 + row * 4));
    if (rp < 0x8000) return false;
    unsigned n = bus_ram_read8((uint16_t)(rp + col));
    uint16_t w = bus_ram_read16((uint16_t)(n * 0x80u + ((y >> 4) & 7) * 16 + ((x >> 4) & 7) * 2));
    *e = sh_block_cell(0x9000, w & 0x3FF, (x >> 3) & 1, (y >> 3) & 1, (w & 0x0400) != 0, (w & 0x0800) != 0);
    return true;
}

/* SH3 engine's background: width/height in the layout header ($FF8002/$FF8006), its row
 * pointers in the second word of each row pair from $FF8008; wraps like the game's layout */
static bool sh3_bg_cell(int cx, int cy, uint16_t *e) {
    int x = cx * 8, y = cy * 8;
    int bw = bus_ram_read16(0x8002), bh = bus_ram_read16(0x8006);
    if (x < 0 || y < 0 || bw <= 0 || bh <= 0 || bw > 0x100 || bh > 0x40) return false;
    int col = (x >> 7) % bw, row = (y >> 7) % bh;
    uint16_t rp = bus_ram_read16((uint16_t)(0x8008 + row * 4 + 2));
    if (rp < 0x8000) return false;
    unsigned n = bus_ram_read8((uint16_t)(rp + col));
    uint16_t w = bus_ram_read16((uint16_t)(n * 0x80u + ((y >> 4) & 7) * 16 + ((x >> 4) & 7) * 2));
    *e = sh_block_cell(0x9000, w & 0x3FF, (x >> 3) & 1, (y >> 3) & 1, (w & 0x0400) != 0, (w & 0x0800) != 0);
    return true;
}
static void sh3_bg_camera(int *x, int *y) {
    static unsigned a; if (!a) { const char *e = getenv("PS_WIDE_BGCAM"); a = e ? (unsigned)strtoul(e, NULL, 16) : 0xEE8C; }
    *x = (int16_t)bus_ram_read16((uint16_t)a); *y = (int16_t)bus_ram_read16((uint16_t)(a + 4));
}

#define PS_WIDE_SONIC1(name) { name, sh_wide_active, sh_wide_map, sh1_wide_camera, NULL, NULL, 0, sh1_world_cell, {0}, true, sh1_bg_cell, sh1_bg_camera }
#define PS_WIDE_SONIC2(name) { name, sh_wide_active, sh_wide_map, sh2_wide_camera, NULL, NULL, 0, sh2_world_cell, {0}, true, sh2_bg_cell, sh2_bg_camera }
#define PS_WIDE_SONIC3(name) { name, sh_wide_active, sh_wide_map, sh3_wide_camera, NULL, NULL, 0, sh3_world_cell, {0}, true, sh3_bg_cell, sh3_bg_camera }
#endif
