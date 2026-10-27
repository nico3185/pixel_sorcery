/*
 * SDL2 platform layer — windowing, framebuffer presentation, audio output.
 */

#include "genrecomp/platform.h"
#include "genrecomp/bus.h"

#include <SDL.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#ifdef __APPLE__
#include <objc/message.h>
#include <objc/runtime.h>
#endif
#ifdef _MSC_VER
/* A crash here otherwise just says "Segmentation fault" with no stack: dump one, with
 * symbols, on the way down. CaptureStackBackTrace walks the CURRENT stack (this handler's,
 * called from the OS's exception dispatcher) — not the faulted thread's, which is why this
 * uses StackWalk64 seeded from the exception's own CONTEXT (the faulted thread's registers)
 * instead; that's the one that actually shows the caller of the bad indirect call/jump. */
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
static LONG WINAPI ps_crash_handler(EXCEPTION_POINTERS *ep) {
    fprintf(stderr, "\n*** CRASH: exception 0x%08lX at %p ***\n",
            ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress);
    HANDLE process = GetCurrentProcess(), thread = GetCurrentThread();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
    SymInitialize(process, NULL, TRUE);
    CONTEXT ctx = *ep->ContextRecord;
    STACKFRAME64 frame = { 0 };
    frame.AddrPC.Offset = ctx.Rip; frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = ctx.Rbp; frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = ctx.Rsp; frame.AddrStack.Mode = AddrModeFlat;
    for (int i = 0; i < 64; i++) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &ctx,
                          NULL, SymFunctionTableAccess64, SymGetModuleBase64, NULL))
            break;
        DWORD64 addr = frame.AddrPC.Offset;
        if (!addr) { fprintf(stderr, "  #%d (null address — the bad call/jump itself)\n", i); continue; }
        DWORD64 disp = 0;
        char buf[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 256;
        DWORD line_disp; IMAGEHLP_LINE64 line = { sizeof(line) };
        bool got_sym = SymFromAddr(process, addr, &disp, sym);
        bool got_line = got_sym && SymGetLineFromAddr64(process, addr, &line_disp, &line);
        fprintf(stderr, "  #%d %p %s+0x%llx", i, (void *)addr, got_sym ? sym->Name : "?", disp);
        if (got_line) fprintf(stderr, " %s:%lu", line.FileName, line.LineNumber);
        fprintf(stderr, "\n");
    }
    fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

static SDL_Window   *s_window   = NULL;
static SDL_Renderer *s_renderer = NULL;
static SDL_Texture  *s_texture  = NULL;
static SDL_AudioDeviceID s_audio_dev = 0;
static uint64_t s_frame_start = 0;
static bool s_initialized = false;
static int s_volume = 100;             /* settings menu volume (percent) */

/* Low-latency pacing (PS_FRAME_DELAY=auto|off|<ms>): after vsync, sleep for most of the
 * frame, then poll input, run the frame and present just in time for the next vsync. The
 * delay adapts to the measured frame cost. PS_LATENCY_LOG=1 reports poll->present time. */
static int    s_delay_mode = -1;     /* -1 auto, 0 off, >0 fixed ms */
static double s_sync_ms;              /* time spent pacing since the last present (PS_SLOW_LOG) */
static double s_cost_ms = 4.0;       /* smoothed poll->present-call cost (max-biased) */
static uint64_t s_poll_t, s_vsync_t;
/* Vsync-locked pacing: when presenting really waits for the display (measured: the present
 * call blocks), the display is the clock. Each frame is shown for s_repeat refreshes (1 at
 * 60 Hz, 2 at 120 Hz) and nothing else sleeps, so no frame is ever shown twice; the game
 * runs at 60.00 Hz instead of the Genesis' 59.92 (0.13%, unnoticeable) and audio follows
 * through platform_queue_audio's resampler. Otherwise (present does not block, as in some
 * windowed setups, or a display that is not a multiple of 60 Hz) frame_sync's deadline paces
 * at 59.92 Hz, vsync still on: no tearing either way. PS_VSYNC=0: no vsync at all. */
static bool s_metal;                   /* Metal presenter (present_metal.m), paced by CVDisplayLink */
/* Renderer preference (macOS): PS_PRESENTER=metal|opengl, else the settings menu's choice,
 * kept for every title in .../PixelSorcery/display.ini (read before the window opens,
 * which is before a title's own settings load). Default: OpenGL. */
static void display_ini(char *out, size_t n) {
    const char *home = getenv("HOME");
    snprintf(out, n, "%s/Library/Application Support/PixelSorcery/display.ini", home ? home : ".");
}
int platform_renderer_pref(void) {          /* 0 OpenGL, 1 Metal */
    const char *p = getenv("PS_PRESENTER");
    if (p && *p) return !strcmp(p, "metal");
    char path[1100]; display_ini(path, sizeof path);
    FILE *f = fopen(path, "r");
    int metal = 0;
    if (f) { char l[64]; while (fgets(l, sizeof l, f)) if (!strncmp(l, "renderer=metal", 14)) metal = 1; fclose(f); }
    return metal;
}
void platform_set_renderer_pref(int metal) {   /* takes effect at the next start */
    char path[1100]; display_ini(path, sizeof path);
    FILE *f = fopen(path, "w");
    if (f) { fprintf(f, "renderer=%s\n", metal ? "metal" : "opengl"); fclose(f); }
}
int platform_renderer_active(void) { return s_metal; }
static bool s_metal_wanted(void) {
#ifdef __APPLE__
    return platform_renderer_pref() == 1;
#else
    return false;
#endif
}
static bool s_vsync = true;

#ifdef __APPLE__
bool metal_present_init(SDL_Window *win);
void metal_present_frame(const uint8_t *xrgb, int w, int h, double poll_ms_ago);
double metal_latency_ms(void);
void metal_queue_stats(double *avg_inflight, int *max_inflight, double *commit_to_screen_ms);
long metal_drained(void);
uint64_t metal_wait_vblank(uint64_t n);
uint64_t metal_vblank_count(void);
double metal_refresh_hz(void);
void metal_present_shutdown(void);
#endif
static int  s_repeat = 1;              /* refreshes per game frame, 0: not a multiple of 60 Hz */
static bool s_locked;                  /* the display paces the frames */
static double s_present_max;           /* longest present call over the current window (ms) */
static double s_lat_sum; static long s_lat_n;
static double s_aq_sum; static long s_aq_n; static uint32_t s_aq_min = 0xFFFFFFFF;
static double ms_since(uint64_t t) { return (double)(SDL_GetPerformanceCounter() - t) * 1000.0 / (double)SDL_GetPerformanceFrequency(); }

/* Headless/record options (see platform_parse_args). Headless exists so a
 * recomp can run where no window may open (over RDP, in CI): frames go to
 * ffmpeg instead of a screen, and scripted presses stand in for a player. */
static bool s_headless = false;
static const char *s_record_path = NULL;
static FILE *s_record = NULL;
static long s_max_frames = 0;   /* 0 = unlimited */
static long s_frame_no = 0;
#define MAX_PRESSES 4096   /* long scripted runs: a jump every second for minutes */
static struct { long start, len; uint16_t buttons; } s_presses[MAX_PRESSES];
static int s_press_count = 0;
static const char *s_ram_dir = NULL;  /* --ram-dump DIR:EVERY */
static long s_ram_every = 60;

static uint16_t parse_buttons(const char *s) {
    static const struct { const char *name; uint16_t bit; } map[] = {
        {"UP",0x0001},{"DOWN",0x0002},{"LEFT",0x0004},{"RIGHT",0x0008},
        {"B",0x0010},{"C",0x0020},{"A",0x0040},{"START",0x0080},
        {"X",0x0100},{"Y",0x0200},{"Z",0x0400},{"MODE",0x0800}};
    uint16_t b = 0;
    char buf[128];
    strncpy(buf, s, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
    for (char *tok = strtok(buf, "+"); tok; tok = strtok(NULL, "+"))
        for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
            if (!strcmp(tok, map[i].name)) b |= map[i].bit;
    return b;
}

int platform_parse_args(int argc, char **argv) {
#ifdef _MSC_VER
    SetUnhandledExceptionFilter(ps_crash_handler);
#endif
    int out = 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--headless")) s_headless = true;
        else if (!strcmp(argv[i], "--record") && i + 1 < argc) s_record_path = argv[++i];
        else if (!strcmp(argv[i], "--ram-dump") && i + 1 < argc) {
            /* DIR[:EVERY] -- writes DIR/ram_NNNNNN.bin (64KB work RAM) */
            static char dir[512];
            strncpy(dir, argv[++i], sizeof(dir) - 1);
            char *colon = strrchr(dir, ':');
            if (colon && colon > dir + 1) { *colon = 0; s_ram_every = atol(colon + 1); }
            if (s_ram_every < 1) s_ram_every = 1;
            s_ram_dir = dir;
        }
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) s_max_frames = atol(argv[++i]);
        else if (!strcmp(argv[i], "--press") && i + 1 < argc) {
            /* FRAME:BUTTONS[:LEN], e.g. 600:START or 900:A+RIGHT:30 */
            char name[64] = {0};
            long f = 0, len = 6;
            if (sscanf(argv[++i], "%ld:%63[^:]:%ld", &f, name, &len) >= 2 &&
                s_press_count < MAX_PRESSES) {
                s_presses[s_press_count].start = f;
                s_presses[s_press_count].len = len;
                s_presses[s_press_count].buttons = parse_buttons(name);
                s_press_count++;
            }
        }
        else argv[out++] = argv[i];
    }
    argv[out] = NULL;
    return out;
}

bool platform_is_headless(void) { return s_headless; }
long platform_frame_number(void) { return s_frame_no; }

uint16_t platform_scripted_buttons(void) {
    uint16_t b = 0;
    for (int i = 0; i < s_press_count; i++)
        if (s_frame_no >= s_presses[i].start &&
            s_frame_no < s_presses[i].start + s_presses[i].len)
            b |= s_presses[i].buttons;
    return b;
}

bool platform_init(const char *window_title, int scale) {
    if (s_initialized) return true;

    if (s_record_path) {
        char cmd[1024];
        /* GPGX renders XRGB8888 little-endian: bytes are B,G,R,X */
        snprintf(cmd, sizeof(cmd),
            "ffmpeg -loglevel error -y -f rawvideo -pix_fmt bgr0 -s %dx%d -r 60 -i - "
            "-vf scale=iw*3:ih*3:flags=neighbor -c:v libx264 -pix_fmt yuv420p \"%s\"",
            GEN_RENDER_WIDTH, GEN_RENDER_HEIGHT, s_record_path);
#ifdef _WIN32
        s_record = _popen(cmd, "wb");
#else
        s_record = popen(cmd, "w");
#endif
        if (!s_record) fprintf(stderr, "platform: could not start ffmpeg for --record\n");
    }

    if (s_headless) {
        /* No window, no audio device, no vsync: run as fast as the CPU allows */
        if (SDL_Init(0) < 0) {
            fprintf(stderr, "platform: SDL_Init failed: %s\n", SDL_GetError());
            return false;
        }
        s_initialized = true;
        return true;
    }

    /* Gamepads by button position (Xbox layout), not by label: on Nintendo pads the
     * labels are swapped, which scrambled A/B/C for the Genesis layout */
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0) {
        fprintf(stderr, "platform: SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }

    extern int ps_wide_max_extra(void);
    int w = (GEN_RENDER_WIDTH + 2 * ps_wide_max_extra()) * scale;
    int h = GEN_RENDER_HEIGHT * scale;

    s_window = SDL_CreateWindow(
        window_title,
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        w, h,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | (s_metal_wanted() ? SDL_WINDOW_METAL : 0)
    );
    if (!s_window) {
        fprintf(stderr, "platform: SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return false;
    }

    /* No text input: on macOS it enables press-and-hold (accent popup / key repeat
     * handling by the input method), which swallows held game buttons. */
    SDL_StopTextInput();

    /* Vsync (no tearing), with the pacing locked to it when it blocks (see s_locked): pacing
     * at 59.92 Hz on a timer while vsync also waited doubled 323 frames in 2400 in macOS
     * fullscreen on a 60 Hz MacBook display. */
    const char *vs = getenv("PS_VSYNC");
    s_vsync = !(vs && *vs == '0');
#ifdef __APPLE__
    if (s_metal_wanted()) {
        if (metal_present_init(s_window)) {
            s_metal = true;
            s_locked = true;
            goto renderer_done;
        }
        /* Metal chosen (settings menu) but unavailable here: OpenGL in a plain window, so the
         * game still starts */
        fprintf(stderr, "platform: Metal presenter failed (%s), using OpenGL\n", SDL_GetError());
        SDL_DestroyWindow(s_window);
        s_window = SDL_CreateWindow(window_title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h,
                                    SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
        if (!s_window) { fprintf(stderr, "platform: SDL_CreateWindow failed: %s\n", SDL_GetError()); SDL_Quit(); return false; }
        SDL_StopTextInput();
    }
    /* OpenGL: its vsync swap waits for one refresh; the Metal renderer's drawable queue
     * made presents wait two refreshes at times (measured in fullscreen: 1252 missed
     * refreshes in 2400 frames, against 24 with OpenGL). SDL_RENDER_DRIVER overrides. */
    if (!getenv("SDL_RENDER_DRIVER")) SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengl");
#endif
    s_renderer = SDL_CreateRenderer(s_window, -1,
        SDL_RENDERER_ACCELERATED | (s_vsync ? SDL_RENDERER_PRESENTVSYNC : 0));
    if (!s_renderer) {
        fprintf(stderr, "platform: SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(s_window);
        SDL_Quit();
        return false;
    }

#ifdef __APPLE__
    /* At most 2 drawables in flight: the default of 3 can hold a finished frame back one
     * more vsync. */
    void *layer = SDL_RenderGetMetalLayer(s_renderer);
    if (layer) ((void (*)(void *, SEL, unsigned long))objc_msgSend)(layer, sel_registerName("setMaximumDrawableCount:"), 2);
#endif
    {
        const char *d = getenv("PS_FRAME_DELAY");
        s_delay_mode = (!d || !strcmp(d, "auto")) ? -1 : !strcmp(d, "off") ? 0 : atoi(d);
    }

    /* Set logical size for aspect-ratio-correct scaling */
    SDL_RenderSetLogicalSize(s_renderer, GEN_RENDER_WIDTH, GEN_RENDER_HEIGHT);

    s_texture = SDL_CreateTexture(s_renderer,
        SDL_PIXELFORMAT_XRGB8888,   /* Genesis Plus GX 32bpp writes 0x00RRGGBB */
        SDL_TEXTUREACCESS_STREAMING,
        GEN_RENDER_MAX_WIDTH, GEN_RENDER_HEIGHT);
    if (!s_texture) {
        fprintf(stderr, "platform: SDL_CreateTexture failed: %s\n", SDL_GetError());
        SDL_DestroyRenderer(s_renderer);
        SDL_DestroyWindow(s_window);
        SDL_Quit();
        return false;
    }
#ifdef __APPLE__
renderer_done: ;
#endif

    /* Audio setup */
    SDL_AudioSpec want = {0};
    want.freq = 44100;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 512;          /* ~11.6 ms device buffer */
    want.callback = NULL; /* Using queue-based audio */

    SDL_AudioSpec have;
    s_audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (s_audio_dev > 0) {
        SDL_PauseAudioDevice(s_audio_dev, 0);
    }

    s_frame_start = SDL_GetPerformanceCounter();
    s_initialized = true;
    return true;
}

void platform_present_frame(const uint8_t *framebuffer) { platform_present_frame_w(framebuffer, GEN_RENDER_WIDTH); }

void platform_present_frame_w(const uint8_t *framebuffer, int width) {
    static int s_shown_w = GEN_RENDER_WIDTH;
    if (s_record && width == GEN_RENDER_WIDTH)
        fwrite(framebuffer, 4, GEN_RENDER_WIDTH * GEN_RENDER_HEIGHT, s_record);
    if (s_ram_dir && s_frame_no % s_ram_every == 0) {
        char path[600];
        snprintf(path, sizeof(path), "%s/ram_%06ld.bin", s_ram_dir, s_frame_no);
        FILE *f = fopen(path, "wb");
        if (f) { bus_dump_state(f); fclose(f); }
    }
    {   /* PS_SLOW_LOG: frames whose work (emulation + render, between presents, pacing
         * excluded) took over 8 ms, half a frame at 60 Hz: what a player feels as a hitch */
        static int on = -1; static uint64_t last;
        if (on < 0) on = getenv("PS_SLOW_LOG") != NULL;
        if (on) {
            uint64_t now = SDL_GetPerformanceCounter();
            double ms = last ? (double)(now - last) * 1000.0 / (double)SDL_GetPerformanceFrequency() - s_sync_ms : 0;
            if (ms > 8.0 && s_headless) fprintf(stderr, "slow: frame %ld %.1f ms\n", s_frame_no, ms);
            last = now; s_sync_ms = 0;
        }
    }
    s_frame_no++;
    if (s_max_frames && s_frame_no >= s_max_frames) {
        printf("platform: reached --frames %ld, exiting\n", s_max_frames);
        platform_shutdown();
        exit(0);
    }
    if (s_metal) {
#ifdef __APPLE__
        { extern int ps_poll_fast_forward(void); if (ps_poll_fast_forward() && s_frame_no % 30) return; }
        double cost = ms_since(s_poll_t);
        s_cost_ms = cost > s_cost_ms ? cost : s_cost_ms * 0.98 + cost * 0.02;
        metal_present_frame(framebuffer, width, GEN_RENDER_HEIGHT, cost);
        static uint64_t prev; static long gaps, frames;
        uint64_t now = SDL_GetPerformanceCounter();
        if (prev && getenv("PS_SLOW_LOG")) {
            double gap = (double)(now - prev) * 1000.0 / (double)SDL_GetPerformanceFrequency();
            frames++;
            if (gap > 25.0) { gaps++; fprintf(stderr, "hitch: frame %ld %.1f ms between presents (%ld in %ld frames)\n", s_frame_no, gap, gaps, frames); }
        }
        prev = now;
        if (getenv("PS_LATENCY_LOG")) {
            s_lat_sum += cost; s_lat_n++;
            if (s_lat_n == 300) {
                double qa, cs; int qm; metal_queue_stats(&qa, &qm, &cs);
                fprintf(stderr, "metal: in flight at present avg %.2f max %d, commit -> on screen %.1f ms, %ld frames skipped to drain\n", qa, qm, cs, metal_drained());
                fprintf(stderr, "latency: input poll -> on screen %.1f ms avg (metal, presented time; frame cost %.1f ms); audio queue avg %.1f ms, min %.1f ms\n",
                        metal_latency_ms(), s_cost_ms, s_aq_n ? s_aq_sum / s_aq_n / 44.1 : 0, s_aq_min / 44.1);
                s_aq_sum = 0; s_aq_n = 0; s_aq_min = 0xFFFFFFFF; s_lat_sum = 0; s_lat_n = 0;
            }
        }
#endif
        return;
    }
    if (!s_texture || !s_renderer) return;
    { extern int ps_poll_fast_forward(void);       /* resuming: show 1 frame in 30 */
      if (ps_poll_fast_forward() && s_frame_no % 30) return; }

    SDL_Rect src = { 0, 0, width, GEN_RENDER_HEIGHT };
    if (width != s_shown_w) {                         /* 4:3 <-> widescreen */
        SDL_RenderSetLogicalSize(s_renderer, width, GEN_RENDER_HEIGHT);
        s_shown_w = width;
    }
    SDL_UpdateTexture(s_texture, &src, framebuffer, width * 4);
    SDL_RenderClear(s_renderer);
    SDL_RenderCopy(s_renderer, s_texture, &src, NULL);
    double cost = ms_since(s_poll_t);
    s_cost_ms = cost > s_cost_ms ? cost : s_cost_ms * 0.98 + cost * 0.02;   /* rise fast, decay slow */
    if (cost > 8.0 && getenv("PS_SLOW_LOG"))      /* windowed: the frame's own work, vsync excluded */
        fprintf(stderr, "slow: frame %ld %.1f ms (poll to present)\n", s_frame_no, cost);
    {
        static long n; static int dpy_check;
        if (s_vsync && dpy_check-- <= 0) {     /* the display's rate, again every 2 s (window moved) */
            SDL_DisplayMode dm;
            dpy_check = 120;
            if (SDL_GetCurrentDisplayMode(SDL_GetWindowDisplayIndex(s_window), &dm) == 0 && dm.refresh_rate > 0) {
                int k = (int)(dm.refresh_rate / 60.0 + 0.5);
                s_repeat = k >= 1 && fabs(dm.refresh_rate / (60.0 * k) - 1) < 0.015 ? k : 0;
            }
        }
        uint64_t t0 = SDL_GetPerformanceCounter();
        for (int r = 0; r < (s_locked && s_repeat > 1 ? s_repeat : 1); r++) SDL_RenderPresent(s_renderer);
        double pm = ms_since(t0);
        if (pm > s_present_max) s_present_max = pm;
        static uint64_t win_t0;
        if (!win_t0) win_t0 = t0;
        if (++n % 60 == 0) {
            /* timer -> locked: a present waited for the display (> 4 ms, at least once a
             * second); locked -> timer: frames came faster than the display (the present
             * stopped blocking), averaged over the second */
            double avg = ms_since(win_t0) / 60.0, period = 1000.0 / 60.0 * (s_repeat ? s_repeat : 1);
            win_t0 = SDL_GetPerformanceCounter();
            bool lock = s_vsync && s_repeat && (s_locked ? avg > 0.8 * period : s_present_max > 4.0);
            if (lock != s_locked && getenv("PS_SLOW_LOG"))
                fprintf(stderr, "pacing: %s (present waits up to %.1f ms, %d refresh(es) per frame)\n",
                        lock ? "locked to the display" : "timer at 59.92 Hz", s_present_max, s_repeat);
            s_locked = lock;
            s_present_max = 0;
        }
    }
    {   /* PS_SLOW_LOG: a gap of more than 25 ms between two presents is a missed refresh */
        static uint64_t prev; static long gaps, frames;
        uint64_t now = SDL_GetPerformanceCounter();
        if (prev && getenv("PS_SLOW_LOG")) {
            double gap = (double)(now - prev) * 1000.0 / (double)SDL_GetPerformanceFrequency();
            frames++;
            if (gap > 25.0) { gaps++; fprintf(stderr, "hitch: frame %ld %.1f ms between presents (%ld in %ld frames)\n", s_frame_no, gap, gaps, frames); }
        }
        prev = now;
    }
    s_vsync_t = SDL_GetPerformanceCounter();
    if (getenv("PS_LATENCY_LOG")) {
        s_lat_sum += ms_since(s_poll_t); s_lat_n++;
        if (s_lat_n == 300) {
            fprintf(stderr, "latency: input poll -> present %.1f ms avg (frame cost %.1f ms); audio queue avg %.1f ms, min %.1f ms\n",
                    s_lat_sum / s_lat_n, s_cost_ms, s_aq_n ? s_aq_sum / s_aq_n / 44.1 : 0, s_aq_min / 44.1);
            s_aq_sum = 0; s_aq_n = 0; s_aq_min = 0xFFFFFFFF;
            s_lat_sum = 0; s_lat_n = 0;
        }
    }
}

void platform_queue_audio(const int16_t *samples, int sample_count) {
    extern int ps_poll_fast_forward(void);
    if (s_audio_dev > 0 && samples && !ps_poll_fast_forward()) {
        /* Keep the queue near 2 frames (~33 ms) by resampling: the display may run a hair
         * faster than the Genesis' 59.92 Hz (exactly 60 when the pacing is locked to it),
         * which would otherwise make audio lag further and further. The rate follows the
         * queue's distance from its target, within +-0.5% (inaudible), linear interpolation
         * carrying the phase across frames. */
        uint32_t queued = SDL_GetQueuedAudioSize(s_audio_dev) / 4, target = 44100 * 2 / 60;
        if (queued < 64) {               /* start, or ran dry: one frame of silent headroom */
            static int16_t silence[735 * 2];
            SDL_QueueAudio(s_audio_dev, silence, sizeof silence);
            queued += 735;
        }
        static double ratio = 1.0, phase;      /* input samples per output sample */
        double want = 1.0 + ((double)queued - target) / target * 0.01;
        if (want > 1.005) want = 1.005;
        if (want < 0.995) want = 0.995;
        ratio += (want - ratio) * 0.05;        /* glide: no audible pitch steps */
        static int16_t out[8192 * 2];
        static int16_t last[2];                /* previous frame's final sample (interpolation) */
        int m = 0;
        double vol = s_volume / 100.0;
        for (; m < 8192; m++) {
            double pos = phase + m * ratio;     /* in input samples; -1 is last[] */
            int i = (int)floor(pos);
            if (i + 1 >= sample_count) break;
            double f = pos - i;
            for (int c = 0; c < 2; c++) {
                double a0 = i < 0 ? last[c] : samples[i * 2 + c], a1 = samples[(i + 1) * 2 + c];
                out[m * 2 + c] = (int16_t)((a0 + (a1 - a0) * f) * vol);
            }
        }
        phase = phase + m * ratio - sample_count;  /* next frame starts this far past its sample 0 */
        if (phase < -1) phase = -1;
        last[0] = samples[(sample_count - 1) * 2]; last[1] = samples[(sample_count - 1) * 2 + 1];
        SDL_QueueAudio(s_audio_dev, out, (uint32_t)(m * 2 * sizeof(int16_t)));
        s_aq_sum += queued; s_aq_n++; if (queued < s_aq_min) s_aq_min = queued;
    }
}

void platform_set_frame_delay(bool on) { s_delay_mode = on ? -1 : 0; }
void platform_set_volume(int percent) { s_volume = percent; }
void platform_set_fullscreen(bool on) {
    if (s_window) SDL_SetWindowFullscreen(s_window, on ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}
void platform_show_frame(const uint32_t *pixels, int width) {
#ifdef __APPLE__
    if (s_metal) { metal_present_frame((const uint8_t *)pixels, width, GEN_RENDER_HEIGHT, 0); return; }
#endif
    if (!s_texture || !s_renderer) return;
    SDL_Rect src = { 0, 0, width, GEN_RENDER_HEIGHT };
    SDL_RenderSetLogicalSize(s_renderer, width, GEN_RENDER_HEIGHT);
    SDL_UpdateTexture(s_texture, &src, pixels, width * 4);
    SDL_RenderClear(s_renderer);
    SDL_RenderCopy(s_renderer, s_texture, &src, NULL);
    SDL_RenderPresent(s_renderer);
}

bool platform_poll_events(void) {
    SDL_Event ev;
    s_poll_t = SDL_GetPerformanceCounter();
    if (s_headless) return true;
    while (SDL_PollEvent(&ev)) {
        extern void ps_menu_request(void);
        if ((ev.type == SDL_KEYDOWN && (ev.key.keysym.sym == SDLK_F1 || ev.key.keysym.sym == SDLK_TAB)) ||
            (ev.type == SDL_CONTROLLERBUTTONDOWN && (ev.cbutton.button == SDL_CONTROLLER_BUTTON_BACK ||
                                                     ev.cbutton.button == SDL_CONTROLLER_BUTTON_GUIDE)))
            ps_menu_request();
        if (ev.type == SDL_QUIT) return false;
        if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE) return false;
    }
    return true;
}

static void frame_sync(void);
void platform_frame_sync(void) {
    uint64_t t0 = SDL_GetPerformanceCounter();
    frame_sync();
    s_sync_ms += (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency();
}

static void frame_sync(void) {
    extern int ps_poll_fast_forward(void);
    if (s_headless || ps_poll_fast_forward()) return;
    /* Deadline pacing at the Genesis NTSC rate (vsync is not relied on: it may not block).
     * Input for the next frame is polled as late as possible: `lead` before its deadline,
     * where lead covers the measured frame cost plus a margin. */
    static uint64_t s_next;
    uint64_t freq = SDL_GetPerformanceFrequency(), now = SDL_GetPerformanceCounter();
#ifdef __APPLE__
    if (s_metal) {   /* wait for the refresh that shows the frame just presented */
        static int chk;
        if (chk-- <= 0) { double hz = metal_refresh_hz(); int k = (int)(hz / 60.0 + 0.5); s_repeat = k >= 1 ? k : 1; chk = 120; }
        /* frame k is due on refresh due; if the loop fell more than a frame behind, resync */
        static uint64_t due;
        uint64_t now_v = metal_vblank_count();
        if (!due || now_v > due + (uint64_t)s_repeat) due = now_v + 1;
        metal_wait_vblank(due);
        due += (uint64_t)s_repeat;
        s_vsync_t = SDL_GetPerformanceCounter();
        /* no low-latency sleep on Metal: frames committed later in the refresh queued behind
         * the previous one (2 in flight, 44 ms to screen against 26 ms right away) */
        s_frame_start = s_vsync_t;
        return;
    }
#endif
    if (s_locked) {
        /* the present already waited for the display; low latency: sleep most of the next
         * refresh, then poll input and run the frame just in time for it */
        s_next = 0;
        if (s_delay_mode != 0) {
            double period_ms = 1000.0 / 60.0 * s_repeat;
            double lead_ms = s_delay_mode > 0 ? period_ms - s_delay_mode : s_cost_ms * 1.5 + 4.0;   /* 4 ms: macOS sleeps can wake late */
            uint64_t until = s_vsync_t + (uint64_t)((period_ms - lead_ms) * (double)freq / 1000.0);
            while (SDL_GetPerformanceCounter() + freq / 500 < until) SDL_Delay(1);
            while (SDL_GetPerformanceCounter() < until) ;
        }
        s_frame_start = SDL_GetPerformanceCounter();
        return;
    }
    uint64_t period = (uint64_t)((double)freq / 59.9227);
    if (!s_next || now > s_next + 2 * period) s_next = now;      /* first frame / stall: resync */
    s_next += period;
    double lead_ms = s_delay_mode == 0 ? 1000.0 / 59.9227
                   : s_delay_mode > 0 ? 1000.0 / 59.9227 - s_delay_mode
                   : s_cost_ms * 1.5 + 1.5;
    uint64_t lead = (uint64_t)(lead_ms * (double)freq / 1000.0);
    uint64_t until = s_next > lead ? s_next - lead : 0;
    while (SDL_GetPerformanceCounter() + freq / 500 < until) SDL_Delay(1);
    while (SDL_GetPerformanceCounter() < until) ;
    s_frame_start = SDL_GetPerformanceCounter();
}

void platform_shutdown(void) {
    if (s_record) {
#ifdef _WIN32
        _pclose(s_record);
#else
        pclose(s_record);
#endif
        s_record = NULL;
    }
    if (!s_initialized) return;

    if (s_audio_dev > 0) {
        SDL_CloseAudioDevice(s_audio_dev);
        s_audio_dev = 0;
    }
#ifdef __APPLE__
    if (s_metal) metal_present_shutdown();
#endif
    if (s_texture)  { SDL_DestroyTexture(s_texture);   s_texture  = NULL; }
    if (s_renderer) { SDL_DestroyRenderer(s_renderer); s_renderer = NULL; }
    if (s_window)   { SDL_DestroyWindow(s_window);     s_window   = NULL; }

    SDL_Quit();
    s_initialized = false;
}
