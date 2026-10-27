/*
 * genrecomp core lifecycle — init, frame, shutdown.
 *
 * This is the glue that ties Genesis Plus GX hardware, the recomp
 * CPU state, and the SDL2 platform layer together.
 */

#include "genrecomp/genrecomp.h"
#include "genrecomp/ps_vdp.h"
#include "genrecomp/ps_render.h"
#include "ps_png.h"

/* GenPlusGX headers */
#include "ps_hw.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Defined in osd.c */
extern void genrecomp_config_default(void);
extern void ps_60hz(const char *path);

/* Pixel buffer for VDP output (720x576x4 — GenPlusGX's max render size) */
#define GPGX_BITMAP_WIDTH  720
#define GPGX_BITMAP_HEIGHT 576
#ifndef PS_NATIVE
static uint32_t s_bitmap_data[GPGX_BITMAP_WIDTH * GPGX_BITMAP_HEIGHT];
/* ROM word patch (menu ROM toggles), GPGX's byte-swapped ROM: returns the old word */
uint16_t ps_hw_rom_patch16(uint32_t addr, uint16_t value) {
    addr &= ~1u;
    uint16_t old = (uint16_t)(cart.rom[addr ^ 1] << 8 | cart.rom[(addr + 1) ^ 1]);
    cart.rom[addr ^ 1] = (uint8_t)(value >> 8); cart.rom[(addr + 1) ^ 1] = (uint8_t)value;
    return old;
}
#endif

#define AUDIO_SAMPLE_RATE  44100

static bool s_initialized = false;

long ps_poll_index(void);
bool genrecomp_init(const char *window_title, int scale) {
    (void)ps_poll_index();   /* links ps_poll.c (its constructor installs the poll hook) */
    if (s_initialized) return true;

    /* Set up GenPlusGX config defaults */
#ifndef PS_NATIVE
    genrecomp_config_default();

    /* Initialize bitmap structure — GenPlusGX renders into this */
    memset(&bitmap, 0, sizeof(bitmap));
    bitmap.width  = GPGX_BITMAP_WIDTH;
    bitmap.height = GPGX_BITMAP_HEIGHT;
    bitmap.pitch  = GPGX_BITMAP_WIDTH * 4;  /* 32bpp RGBX */
    bitmap.data   = (uint8 *)s_bitmap_data;
#endif

    /* Initialize function dispatch table */
    func_table_init();

    /* Initialize SDL2 platform */
    if (!platform_init(window_title, scale)) {
        fprintf(stderr, "genrecomp: failed to initialize platform\n");
        return false;
    }

    s_initialized = true;
#ifdef PS_NATIVE
    printf("genrecomp: initialized (native hardware layer, no emulator core)\n");
#else
    printf("genrecomp: initialized (Genesis Plus GX backend)\n");
#endif
    return true;
}

bool genrecomp_load_rom(const char *path) {
    if (!s_initialized) return false;
#ifdef PS_NATIVE
    if (!ps_hw_load_rom(path)) {
        fprintf(stderr, "genrecomp: cannot load ROM %s\n", path);
        return false;
    }
    ps_hw_reset();
#else

    /* GenPlusGX's load_rom expects a mutable char* */
    char pathbuf[MAXPATHLEN];
    strncpy(pathbuf, path, sizeof(pathbuf) - 1);
    pathbuf[sizeof(pathbuf) - 1] = '\0';

    /* Load ROM via GenPlusGX (calls our load_archive in osd.c) */
    ps_60hz(pathbuf);
    if (!load_rom(pathbuf)) {
        fprintf(stderr, "genrecomp: GenPlusGX rejected the ROM: %s\n", path);
        return false;
    }

    /* Initialize audio subsystem */
    audio_init(AUDIO_SAMPLE_RATE, 0);

    /* Two gamepads. Must precede system_init(): its io_init() binds the
     * port handlers from input.system, and without it both ports stay on
     * dummy handlers that never report a button. */
    input.system[0] = SYSTEM_GAMEPAD;
    input.system[1] = SYSTEM_GAMEPAD;

    /* Initialize all hardware subsystems */
    system_init();

    /* Hard reset everything */
    system_reset();

    /* Re-point bitmap after reset */
    bitmap.data = (uint8 *)s_bitmap_data;
#endif

    /* PixelSorcery native video (PS_RENDERER selects the backend) */
    bus_video_init();
    ps_render_init();
    {
        const char *lim = getenv("PS_SPRITE_LIMIT");
        if (lim && *lim == '0') ps_render_set_sprite_limits(false);
    }
    printf("genrecomp: video backend %s\n", bus_video_mode() == BUS_VIDEO_NATIVE ? "native (PixelSorcery)"
                                        : bus_video_mode() == BUS_VIDEO_GPGX ? "gpgx" : "compare (native vs gpgx)");

    /* Initialize recomp CPU state from the vector table
     * (now that ROM is loaded and memory map is set up) */
    recomp_m68k_reset();
    { void bus_power_on(void); bus_power_on(); }   /* hardware power-on timing (after reset) */


#ifndef PS_NATIVE
    printf("genrecomp: ROM loaded — \"%s\"\n", rominfo.international);
    printf("genrecomp: System: %s | Region: %s\n",
           rominfo.consoletype, rominfo.country);
#endif
    printf("genrecomp: PC=$%06X SP=$%08X\n",
           g_m68k.pc, g_m68k.ssp);

    return true;
}

bool genrecomp_begin_frame(void) {
    /* Poll SDL events */
    if (!platform_poll_events()) {
        return false;
    }

    /* Update input and feed into GenPlusGX's input system */
    recomp_input_update();
    { extern void ps_menu_load(void), ps_menu_frame_pokes(void); ps_menu_load(); ps_menu_frame_pokes(); }


    return true;
}

/* ================================================================
 * Frame output
 *
 * gpgx_render_frame(): Genesis Plus GX draws all scanlines from its VDP
 * state after the recompiled frame code ran (the oracle path).
 * ps_render_frame(): PixelSorcery's native renderer (ps_render.c).
 * In compare mode both run and every frame is diffed pixel by pixel.
 * ================================================================ */
static uint8_t s_present_buf[GEN_RENDER_MAX_WIDTH * GEN_RENDER_HEIGHT * 4];
static int s_present_w = GEN_RENDER_WIDTH;
static uint8_t s_oracle_buf[GEN_RENDER_WIDTH * GEN_RENDER_HEIGHT * 4];

#ifndef PS_NATIVE
/* Renders into `dst` (GEN_RENDER_WIDTH x GEN_RENDER_HEIGHT, XRGB8888).
 * Returns false when GPGX has no valid viewport yet. */
static bool gpgx_render_frame(uint8_t *dst) {
    int active_lines = bitmap.viewport.h;

    /* GPGX defers an H32/H40 switch made mid-display to the next frame
     * boundary inside system_frame_gen(), which never runs here. We draw
     * the whole frame at once, so take the width the registers say now. */
    if (reg[1] & 0x04) {
        bitmap.viewport.w = (reg[12] & 0x01) ? 320 : 256;
        /* Same for the height: GPGX starts in the Master System's 192-line
         * mode and only switches to 224/240 lines at a frame boundary, so
         * without this the bottom 32 lines (COI's HUD) were never drawn. */
        bitmap.viewport.h = (reg[1] & 0x08) ? 240 : 224;
        active_lines = bitmap.viewport.h;
    }

    if (active_lines <= 0 || active_lines > 240 || !bitmap.data) return false;

    uint16_t saved_v = v_counter;
    int vp_w = bitmap.viewport.w;
    if ((reg[1] & 0x44) && vp_w > 0 && vp_w <= 320) {
        /* Sprites for line 0 are parsed at the end of the previous frame
         * (system_frame_gen); do it here too or line 0 shows stale sprites. */
        parse_satb(-1);
        if (getenv("PS_GPGX_FULLCACHE")) {   /* experiment: rebuild every pattern */
            for (int n = 0; n < 0x800; n++) {
                if (!bg_name_dirty[n]) bg_name_list[bg_list_index++] = (uint16)n;
                bg_name_dirty[n] = 0xFF;
            }
        }
        for (int line = 0; line < active_lines; line++) {
            v_counter = line;
            render_line(line);
        }
    }
    v_counter = saved_v;

    int vp_h = bitmap.viewport.h;
    if (vp_w <= 0 || vp_h <= 0) return false;

    uint8_t *fb_start = bitmap.data + (bitmap.viewport.y * bitmap.pitch) + (bitmap.viewport.x * 4);
    int copy_w = (vp_w > GEN_RENDER_WIDTH) ? GEN_RENDER_WIDTH : vp_w;
    int copy_h = (vp_h > GEN_RENDER_HEIGHT) ? GEN_RENDER_HEIGHT : vp_h;

    /* H32 is narrower than the buffer: don't leave the last H40 frame's
     * right-hand columns on screen */
    if (copy_w < GEN_RENDER_WIDTH) memset(dst, 0, GEN_RENDER_WIDTH * GEN_RENDER_HEIGHT * 4);
    for (int y = 0; y < copy_h; y++)
        memcpy(dst + y * GEN_RENDER_WIDTH * 4, fb_start + y * bitmap.pitch, copy_w * 4);
    return true;
}

#endif

#include "genrecomp/ps_wide.h"
static int s_line_h;                  /* lines of the frame being drawn line by line */
static bool s_lines_drawn;
static void native_frame_setup(uint8_t *dst) {
    int w = ps_vdp_width();
    ps_wide_begin_frame();                       /* widescreen only during gameplay, H40 */
    int e = ps_wide_extra();
    s_present_w = GEN_RENDER_WIDTH + 2 * e;
    if (w + 2 * e < s_present_w) memset(dst, 0, (size_t)s_present_w * GEN_RENDER_HEIGHT * 4);
    s_line_h = ps_vdp_height() > GEN_RENDER_HEIGHT ? GEN_RENDER_HEIGHT : ps_vdp_height();
}
static void native_render_frame(uint8_t *dst) {
    native_frame_setup(dst);
    ps_render_lines(0, s_line_h, (uint32_t *)dst, s_present_w);
    ps_wide_record(ps_render_plane_entry);       /* cells on the original screen -> world cache */
}

/* Native port: each active line is drawn at the start of its own scanline, with the VDP
 * state of that moment (raster effects: per-line scroll written by HBlank handlers). */
void genrecomp_render_line(int line) {
#ifdef PS_NATIVE
    if (line == 0) native_frame_setup(s_present_buf);
    if (line < s_line_h) {
        {   /* PS_LINE_DBG=frame: VDP registers as each line of that frame is drawn */
            static long f = -2; extern long ps_frames;
            if (f == -2) f = getenv("PS_LINE_DBG") ? atol(getenv("PS_LINE_DBG")) : -1;
            if (f >= 0 && ps_frames == f)
                fprintf(stderr, "line %3d reg0 %02X reg1 %02X reg7 %02X reg12 %02X cram0 %03X\n", line, g_ps_vdp.reg[0],
                        g_ps_vdp.reg[1], g_ps_vdp.reg[7], g_ps_vdp.reg[12], g_ps_vdp.cram[0]);
        }
        ps_render_lines(line, line + 1, (uint32_t *)s_present_buf, s_present_w);
        extern uint16_t g_ps_render_flags;
        status |= g_ps_render_flags & 0x60;      /* sprite overflow / collision, as the line is drawn */
        g_ps_render_flags = 0;
    }
    if (line == s_line_h - 1) {
        ps_wide_record(ps_render_plane_entry); s_lines_drawn = true;
        { void ps_render_water_frame(uint32_t *, int, int); ps_render_water_frame((uint32_t *)s_present_buf, s_present_w, s_line_h); }
    }
#else
    (void)line;
#endif
}

#ifndef PS_NATIVE
/* Compare mode: report differing pixels (RGB only) and dump PNGs on demand.
 * PS_DIFF_LOG=1 logs every frame; otherwise only frames that differ.
 * PS_DUMP_DIR=dir with PS_DUMP_EVERY=N writes native/gpgx PNG pairs. */
static void compare_frames(const uint8_t *native, const uint8_t *oracle) {
    static long s_frames, s_bad;
    long frame = platform_frame_number();
    const uint32_t *a = (const uint32_t *)native, *b = (const uint32_t *)oracle;
    int diff = 0, fx = -1, fy = -1;
    for (int y = 0; y < GEN_RENDER_HEIGHT; y++)
        for (int x = 0; x < GEN_RENDER_WIDTH; x++) {
            uint32_t p = a[y * GEN_RENDER_WIDTH + x] ^ b[y * GEN_RENDER_WIDTH + x];
            if (p & 0xFFFFFF) {
                if (!diff) { fx = x; fy = y; }
                diff++;
            }
        }
    s_frames++;
    if (diff) s_bad++;
    if (getenv("PS_REGS_AT") && frame == atol(getenv("PS_REGS_AT"))) {
        fprintf(stderr, "[compare] regs native:");
        for (int i = 0; i < 24; i++) fprintf(stderr, " %02X", g_ps_vdp.reg[i]);
        fprintf(stderr, "\n[compare] regs gpgx  :");
        for (int i = 0; i < 24; i++) fprintf(stderr, " %02X", reg[i]);
        fprintf(stderr, "\n");
        int vd = 0, cd = 0, sd = 0;
        for (int i = 0; i < 0x10000; i++) if (g_ps_vdp.vram[i] != vram[i ^ 1]) vd++;
        for (int i = 0; i < 64; i++) if (g_ps_vdp.cram[i] != ((uint16_t *)cram)[i]) cd++;
        for (int i = 0; i < 40; i++) if (g_ps_vdp.vsram[i] != (((uint16_t *)vsram)[i] & 0x7FF)) sd++;
        fprintf(stderr, "[compare] gpgx ntab=%04X ntbb=%04X ntwb=%04X satb=%04X hscb=%04X vp=%dx%d+%d,%d\n",
                ntab, ntbb, ntwb, satb, hscb, bitmap.viewport.w, bitmap.viewport.h, bitmap.viewport.x, bitmap.viewport.y);
        fprintf(stderr, "[compare] vram diff bytes %d, cram diff %d, vsram diff %d\n", vd, cd, sd);
    }
    if (diff || getenv("PS_DIFF_LOG"))
        fprintf(stderr, "[compare] frame %ld: %d px differ (first at %d,%d) native=%06X gpgx=%06X | %ld/%ld frames differ\n",
                frame, diff, fx, fy,
                diff ? a[fy * GEN_RENDER_WIDTH + fx] & 0xFFFFFF : 0,
                diff ? b[fy * GEN_RENDER_WIDTH + fx] & 0xFFFFFF : 0, s_bad, s_frames);
    const char *dir = getenv("PS_DUMP_DIR");
    if (dir) {
        const char *ev = getenv("PS_DUMP_EVERY");
        long every = ev ? atol(ev) : 60;
        if (every > 0 && (frame % every == 0 || (diff && getenv("PS_DUMP_DIFFS")))) {
            char path[1024];
            snprintf(path, sizeof path, "%s/f%06ld_native.png", dir, frame);
            ps_png_write(path, native, GEN_RENDER_WIDTH, GEN_RENDER_HEIGHT);
            snprintf(path, sizeof path, "%s/f%06ld_gpgx.png", dir, frame);
            ps_png_write(path, oracle, GEN_RENDER_WIDTH, GEN_RENDER_HEIGHT);
        }
    }
}

#endif

/* Draw the frame at the start of VBlank, when the hardware has finished the active
 * display (sprite flags become visible to the VBlank handler then). */
static bool s_frame_ok = true, s_frame_drawn;
void genrecomp_render_frame(void) {
    s_frame_ok = true;
#ifdef PS_NATIVE
    if (!s_lines_drawn) native_render_frame(s_present_buf);   /* first frame: no lines yet */
    s_lines_drawn = false;
    {   static const char *sd; static int init;
        if (!init) { init = 1; sd = getenv("PS_SPRITES_DIR"); }
        if (sd && *sd) { extern long ps_frames; void ps_render_dump_sprites(const char *, long);
                         if (ps_vdp_display_enabled()) ps_render_dump_sprites(sd, ps_frames); } }
#else
    int mode = bus_video_mode();
    if (mode == BUS_VIDEO_GPGX) {
        s_frame_ok = gpgx_render_frame(s_present_buf);
    } else {
        native_render_frame(s_present_buf);
        if (mode == BUS_VIDEO_COMPARE && gpgx_render_frame(s_oracle_buf))
            compare_frames(s_present_buf, s_oracle_buf);
    }
#endif
    s_frame_drawn = true;
}

void genrecomp_end_frame(void) {
    if (!s_frame_drawn) genrecomp_render_frame();
    s_frame_drawn = false;
    bool ok = s_frame_ok;

    /* Signal VBlank */
    status |= 0x08; /* VBlank flag */

    /* Audio is mixed at the end of each simulated frame in bus.c */
    { extern void ps_menu_run(uint8_t *, int); ps_menu_run(s_present_buf, s_present_w); }
    { extern void ps_menu_pause_hint(uint8_t *, int); ps_menu_pause_hint(s_present_buf, s_present_w); }
    {   /* PS_FRAMES_DIR=dir [PS_FRAMES_FROM/TO]: dump presented frames (any width) as PNG
         * (PS_FRAMES_BMP=1: as BMP, f<n>.bmp) */
        static const char *dir; static long from = 0, to = -1, n, every = 1; static int init, bmp;
        if (!init) { init = 1; dir = getenv("PS_FRAMES_DIR"); bmp = getenv("PS_FRAMES_BMP") != NULL;
                     if (getenv("PS_FRAMES_FROM")) from = atol(getenv("PS_FRAMES_FROM"));
                     if (getenv("PS_FRAMES_TO")) to = atol(getenv("PS_FRAMES_TO"));
                     if (getenv("PS_FRAMES_EVERY")) every = atol(getenv("PS_FRAMES_EVERY")); }
        if (dir && n >= from && (to < 0 || n <= to) && (n - from) % every == 0) {
            char path[1024]; snprintf(path, sizeof path, "%s/f%06ld.%s", dir, n - from, bmp ? "bmp" : "png");
            extern void ps_menu_demo_overlay(uint8_t *, int, long);
            ps_menu_demo_overlay(s_present_buf, s_present_w, n - from);   /* PS_MENU_DEMO only */
            (bmp ? ps_bmp_write : ps_png_write)(path, s_present_buf, s_present_w, GEN_RENDER_HEIGHT);
        }
        n++;
    }
    if (ok) platform_present_frame_w(s_present_buf, s_present_w);
    platform_frame_sync();
}

void genrecomp_trigger_vblank(void) {
    /* Set VDP VBlank flag */
    status |= 0x08;
#ifndef PS_NATIVE
    vint_pending = 1;
#endif

    /* Trigger level 6 interrupt on the recomp CPU */
    /* (recompiled code checks this and calls the VBlank handler) */
}

const uint8_t *genrecomp_get_framebuffer(void) {
#ifdef PS_NATIVE
    return s_present_buf;
#else
    return bitmap.data;
#endif
}

void genrecomp_shutdown(void) {
    if (!s_initialized) return;

    platform_shutdown();
#ifndef PS_NATIVE
    audio_shutdown();
#endif

    s_initialized = false;
    printf("genrecomp: shutdown complete\n");
}

void genrecomp_vblank_irq(void) {
    /* The VDP raises level 6 at VBlank only if register 1's IE0 is set */
#ifdef PS_NATIVE
    bool ie0 = ps_vdp_vint_enabled();
#else
    bool ie0 = (bus_video_mode() == BUS_VIDEO_NATIVE) ? ps_vdp_vint_enabled() : (reg[1] & 0x20) != 0;
#endif
    /* VInt stays pending until the 68000 acknowledges it: enabling IE0 later fires it */
    { void bus_vint_pending(void); bus_vint_pending(); }
    if (ie0)
        recomp_m68k_interrupt(6);
}

/* ps_state.c: the frame being drawn (lines already drawn this frame, the picture) */
void ps_state_blob(void *p, unsigned long n);
void genrecomp_state(void) {
#define B(v) ps_state_blob(&(v), sizeof (v))
    B(s_present_w); B(s_line_h); B(s_lines_drawn); B(s_frame_ok); B(s_frame_drawn); B(s_present_buf);
#undef B
}
