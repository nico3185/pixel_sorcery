/*
 * Genesis controller input — keyboard/gamepad mapping.
 */

#include "genrecomp/input.h"
#include "genrecomp/io.h"
#include "genrecomp/platform.h"

#include <SDL.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* movie.c: input-movie playback and RAM watches */
uint16_t movie_buttons(long frame, int port);
void movie_watch(long frame);
void movie_record(long frame, uint16_t p1, uint16_t p2);

static uint16_t s_pad_state[2];

/* Pad button mapping (settings menu CONTROLLER SETUP): for each of A B C START X Y Z MODE, the
 * raw control of this controller that presses it - a joystick button (0..99) or an axis pushed
 * past half way (100 + axis: triggers such as ZR on some pads). Stored per controller GUID in
 * PixelSorcery/input.ini. Without one, face buttons by position (left/bottom/right = A/B/C). */
static const uint16_t k_map_btn[8] = { GEN_BTN_A, GEN_BTN_B, GEN_BTN_C, GEN_BTN_START, GEN_BTN_X, GEN_BTN_Y, GEN_BTN_Z, GEN_BTN_MODE };
/* the defaults by position: left / bottom / right face = A / B / C, Start, left shoulder / top face / right
 * shoulder = X / Y / Z; REWIND / FORWARD default to the left / right triggers (input_hot_held) */
static const int k_default[8] = { SDL_CONTROLLER_BUTTON_X, SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_START,
                                  SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_CONTROLLER_BUTTON_Y, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, -1 };
static int s_map[10] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };   /* A B C START X Y Z MODE, then REWIND, FAST FWD */
static bool s_mapped;
static SDL_GameController *s_pad;
static char s_guid[64];

static void ini_path(char *out, size_t n) {
    const char *home = getenv("HOME");
    snprintf(out, n, "%s/Library/Application Support/PixelSorcery/input.ini", home ? home : ".");
}
static void map_load(void) {
    s_mapped = false;
    char path[1024]; ini_path(path, sizeof path);
    FILE *f = fopen(path, "r"); if (!f) return;
    char line[256], g[64]; int m[10];
    while (fgets(line, sizeof line, f)) {
        m[8] = m[9] = -1;                                   /* lines from before REWIND / FAST FWD: the shoulders */
        if (sscanf(line, "%63s %d %d %d %d %d %d %d %d %d %d", g, &m[0], &m[1], &m[2], &m[3], &m[4], &m[5], &m[6], &m[7], &m[8], &m[9]) >= 9 && !strcmp(g, s_guid)) {
            memcpy(s_map, m, sizeof s_map); s_mapped = true;
        }
    }
    fclose(f);
}
static bool raw_down(SDL_Joystick *j, int c) {
    if (c < 0) return false;
    if (c < 100) return SDL_JoystickGetButton(j, c);
    return SDL_JoystickGetAxis(j, c - 100) > 16000;
}
/* the pad's REWIND (0) or FORWARD (1) button held: as set in CONTROLLER SETUP, else the left / right trigger */
bool input_hot_held(int which) {
    if (!s_pad) return false;
    int c = s_mapped ? s_map[8 + which] : -1;
    if (c >= 0) return raw_down(SDL_GameControllerGetJoystick(s_pad), c);
    return SDL_GameControllerGetAxis(s_pad, which ? SDL_CONTROLLER_AXIS_TRIGGERRIGHT : SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000;
}
/* a gamepad button held (SDL_GameControllerButton), for the player's fast forward (platform_sdl.c) */
bool input_pad_button(int b) { return s_pad && SDL_GameControllerGetButton(s_pad, (SDL_GameControllerButton)b); }
bool input_pad_name(char *out, size_t n) {
    if (!s_pad) return false;
    snprintf(out, n, "%s", SDL_GameControllerName(s_pad)); return true;
}
/* The raw control held now (button, else an axis past half way - a trigger resting at -32768 counts once
 * pushed past it, as its press does in play), -1 when none. */
int input_raw_pressed(void) {
    if (!s_pad) return -1;
    SDL_GameControllerUpdate();
    SDL_Joystick *j = SDL_GameControllerGetJoystick(s_pad);
    for (int b = 0; b < SDL_JoystickNumButtons(j) && b < 100; b++) if (SDL_JoystickGetButton(j, b)) return b;
    for (int a = 0; a < SDL_JoystickNumAxes(j); a++) {
        if (SDL_JoystickGetAxis(j, a) > 16000) return 100 + a;   /* a stick pushed, or a trigger (resting at -32768) past half way */
    }
    return -1;
}
/* CONTROLLER SETUP (ps_menu.c): the current mapping (-1 = the default for that function), and a raw
 * control's name as the pad's layout calls it ("A", "LEFTSHOULDER", "RIGHTTRIGGER"), else its number */
void input_get_mapping(int *m) { for (int i = 0; i < 10; i++) m[i] = s_mapped ? s_map[i] : -1; }
void input_raw_name(int c, char *out, size_t n) {
    if (c < 0) { snprintf(out, n, "DEFAULT"); return; }
    if (c >= 1000) {                                      /* 1000 + function: that function's default, by name */
        int f = c - 1000;
        const char *d = f == 8 ? "LEFTTRIGGER" : f == 9 ? "RIGHTTRIGGER" :
                        f < 8 && k_default[f] >= 0 ? SDL_GameControllerGetStringForButton((SDL_GameControllerButton)k_default[f]) : "NONE";
        snprintf(out, n, "%s (DEFAULT)", d); goto up;
    }
    if (s_pad) {
        for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; b++) {
            SDL_GameControllerButtonBind k = SDL_GameControllerGetBindForButton(s_pad, (SDL_GameControllerButton)b);
            if (c < 100 && k.bindType == SDL_CONTROLLER_BINDTYPE_BUTTON && k.value.button == c) {
                snprintf(out, n, "%s", SDL_GameControllerGetStringForButton((SDL_GameControllerButton)b)); goto up; }
        }
        for (int a = 0; a < SDL_CONTROLLER_AXIS_MAX; a++) {
            SDL_GameControllerButtonBind k = SDL_GameControllerGetBindForAxis(s_pad, (SDL_GameControllerAxis)a);
            if (c >= 100 && k.bindType == SDL_CONTROLLER_BINDTYPE_AXIS && k.value.axis == c - 100) {
                snprintf(out, n, "%s", SDL_GameControllerGetStringForAxis((SDL_GameControllerAxis)a)); goto up; }
        }
    }
    snprintf(out, n, c < 100 ? "BUTTON %d" : "AXIS %d", c < 100 ? c : c - 100);
up: for (char *p = out; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
}
void input_set_mapping(const int *m) {
    if (!s_pad) return;
    memcpy(s_map, m, sizeof s_map); s_mapped = true;
    char path[1024]; ini_path(path, sizeof path);
    char keep[64][256]; int nk = 0;
    FILE *f = fopen(path, "r");
    if (f) { char line[256], g[64]; while (nk < 64 && fgets(line, sizeof line, f)) if (sscanf(line, "%63s", g) == 1 && strcmp(g, s_guid)) snprintf(keep[nk++], 256, "%s", line); fclose(f); }
    if (!(f = fopen(path, "w"))) return;
    for (int i = 0; i < nk; i++) fputs(keep[i], f);
    fprintf(f, "%s %d %d %d %d %d %d %d %d %d %d  # %s: A B C START X Y Z MODE REWIND FASTFWD\n", s_guid, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], SDL_GameControllerName(s_pad));
    fclose(f);
}

void recomp_input_update(void) {
    static const uint8_t no_keys[SDL_NUM_SCANCODES];
    const uint8_t *keys = platform_is_headless() ? no_keys : SDL_GetKeyboardState(NULL);
    uint16_t buttons = 0;

    /* Default keyboard mapping (Player 1) */
    if (keys[SDL_SCANCODE_UP])     buttons |= GEN_BTN_UP;
    if (keys[SDL_SCANCODE_DOWN])   buttons |= GEN_BTN_DOWN;
    if (keys[SDL_SCANCODE_LEFT])   buttons |= GEN_BTN_LEFT;
    if (keys[SDL_SCANCODE_RIGHT])  buttons |= GEN_BTN_RIGHT;
    if (keys[SDL_SCANCODE_Z])      buttons |= GEN_BTN_A;
    if (keys[SDL_SCANCODE_X])      buttons |= GEN_BTN_B;
    if (keys[SDL_SCANCODE_C])      buttons |= GEN_BTN_C;
    if (keys[SDL_SCANCODE_RETURN]) buttons |= GEN_BTN_START;
    if (keys[SDL_SCANCODE_A])      buttons |= GEN_BTN_X;
    if (keys[SDL_SCANCODE_S])      buttons |= GEN_BTN_Y;
    if (keys[SDL_SCANCODE_D])      buttons |= GEN_BTN_Z;

    /* Gamepad (hot-plugged): face buttons left/bottom/right = A/B/C, top = Y,
     * shoulders = X/Z, Start = Start, d-pad or left stick = d-pad. */
    SDL_GameController *pad = s_pad;
    if (pad && !SDL_GameControllerGetAttached(pad)) { SDL_GameControllerClose(pad); pad = s_pad = NULL; }
    if (!pad && !platform_is_headless())
        for (int i = 0; i < SDL_NumJoysticks() && !pad; i++)
            if (SDL_IsGameController(i) && (pad = s_pad = SDL_GameControllerOpen(i))) {
                SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(SDL_GameControllerGetJoystick(pad)), s_guid, sizeof s_guid);
                map_load();
                fprintf(stderr, "input: gamepad \"%s\"%s\n", SDL_GameControllerName(pad), s_mapped ? " (button mapping from input.ini)" : "");
            }
    if (pad) {                                      /* d-pad as usual; each function its set button, else its default */
        static const SDL_GameControllerButton dirs[4] = { SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_DOWN,
                                                          SDL_CONTROLLER_BUTTON_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT };
        for (int i = 0; i < 4; i++) if (SDL_GameControllerGetButton(pad, dirs[i])) buttons |= (uint16_t)(1u << i);
        SDL_Joystick *j = SDL_GameControllerGetJoystick(pad);
        for (int i = 0; i < 8; i++) {
            int c = s_mapped ? s_map[i] : -1;
            if (c >= 0 ? raw_down(j, c) : (k_default[i] >= 0 && SDL_GameControllerGetButton(pad, (SDL_GameControllerButton)k_default[i])))
                buttons |= k_map_btn[i];
        }
    }
    if (pad) {
        int ax = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX);
        int ay = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);
        if (ax < -16000) buttons |= GEN_BTN_LEFT;  if (ax > 16000) buttons |= GEN_BTN_RIGHT;
        if (ay < -16000) buttons |= GEN_BTN_UP;    if (ay > 16000) buttons |= GEN_BTN_DOWN;
    }
    buttons |= platform_scripted_buttons();
    { extern int g_ps_boot_hidden;                  /* booting unseen: Start through the BIOS menu */
      if (g_ps_boot_hidden) buttons = (g_ps_boot_hidden == 2 && platform_frame_number() % 16 < 4) ? GEN_BTN_START : 0; }
    {   /* game paused: C opens the settings menu (not passed to the game) */
        extern bool ps_menu_paused(void); extern void ps_menu_request(void);
        static uint16_t prev_c;
        if (ps_menu_paused()) {
            if ((buttons & GEN_BTN_C) && !prev_c) ps_menu_request();
            prev_c = buttons & GEN_BTN_C;
            buttons &= (uint16_t)~GEN_BTN_C;
        } else prev_c = buttons & GEN_BTN_C;
    }
    long frame = platform_frame_number();
    s_pad_state[0] = buttons | movie_buttons(frame, 0);
    s_pad_state[1] = movie_buttons(frame, 1);
    movie_watch(frame);
    movie_record(frame, s_pad_state[0], s_pad_state[1]);

    /* Feed into I/O system */
    io_set_pad_state(0, s_pad_state[0]);
    io_set_pad_state(1, s_pad_state[1]);
}

uint16_t recomp_input_read_pad(int port) {
    if (port < 0 || port > 1) return 0;
    return s_pad_state[port];
}
