#ifndef GENRECOMP_PLATFORM_H
#define GENRECOMP_PLATFORM_H

/*
 * SDL2 platform layer — windowing, rendering, audio output, frame timing.
 *
 * You usually don't call these directly — genrecomp_init/end_frame
 * handle it for you. But they're here if you need finer control.
 */

#include <stdbool.h>
#include <stdint.h>

/* Genesis output resolution (320x224 NTSC, can be 256 wide in H32 mode) */
#define GEN_RENDER_WIDTH   320
#define GEN_RENDER_HEIGHT  224

/* Consume runtime flags from argv and compact the rest (argv[1] onward is
 * left for the title, e.g. a ROM path). Returns the new argc. Flags:
 *   --headless            no window or audio device, no frame pacing
 *   --record out.mp4      pipe every presented frame to ffmpeg
 *   --frames N            exit after N presented frames
 *   --press F:BTN[:LEN]   hold BTN (e.g. START, A+RIGHT) from frame F for LEN
 *                         frames (default 6); repeatable
 *   --ram-dump DIR[:N]    write RAM+VDP state to DIR/ram_<frame>.bin every
 *                         N frames (default 60); DIR must exist
 * Call before genrecomp_init. */
int platform_parse_args(int argc, char **argv);
bool platform_is_headless(void);
long platform_frame_number(void);
int platform_renderer_pref(void);           /* 0 OpenGL, 1 Metal (macOS; next start) */
void platform_set_renderer_pref(int metal);
int platform_renderer_active(void);
uint16_t platform_scripted_buttons(void);

/* Initialize SDL2 window + renderer + audio */
bool platform_init(const char *window_title, int scale);

/* Present a framebuffer (320x224 RGBX8888) */
void platform_present_frame(const uint8_t *framebuffer);
/* Widescreen: a frame `width` pixels wide (GEN_RENDER_WIDTH .. GEN_RENDER_MAX_WIDTH). */
void platform_present_frame_w(const uint8_t *framebuffer, int width);
#define GEN_RENDER_MAX_WIDTH (GEN_RENDER_WIDTH + 2 * 240)
void platform_show_frame(const uint32_t *pixels, int width);   /* display only (menu) */
void platform_set_frame_delay(bool on);
void platform_set_volume(int percent);
void platform_set_fullscreen(bool on);

/* Queue audio samples (interleaved stereo int16) */
void platform_queue_audio(const int16_t *samples, int sample_count);

/* Poll SDL events. Returns false if quit was requested. */
bool platform_poll_events(void);

/* Wait to maintain ~60 Hz frame timing */
void platform_frame_sync(void);

/* Clean shutdown */
void platform_shutdown(void);

#endif /* GENRECOMP_PLATFORM_H */
