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
static int s_map[8];
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
    char line[256], g[64]; int m[8];
    while (fgets(line, sizeof line, f))
        if (sscanf(line, "%63s %d %d %d %d %d %d %d %d", g, &m[0], &m[1], &m[2], &m[3], &m[4], &m[5], &m[6], &m[7]) == 9 && !strcmp(g, s_guid)) {
            memcpy(s_map, m, sizeof s_map); s_mapped = true;
        }
    fclose(f);
}
static bool raw_down(SDL_Joystick *j, int c) {
    if (c < 0) return false;
    if (c < 100) return SDL_JoystickGetButton(j, c);
    return SDL_JoystickGetAxis(j, c - 100) > 16000;
}
bool input_pad_name(char *out, size_t n) {
    if (!s_pad) return false;
    snprintf(out, n, "%s", SDL_GameControllerName(s_pad)); return true;
}
/* The raw control held now (button, else an axis past half way; axes resting high, like some
 * triggers' -32768..32767, count only when pushed from rest), -1 when none. */
int input_raw_pressed(void) {
    if (!s_pad) return -1;
    SDL_GameControllerUpdate();
    SDL_Joystick *j = SDL_GameControllerGetJoystick(s_pad);
    for (int b = 0; b < SDL_JoystickNumButtons(j) && b < 100; b++) if (SDL_JoystickGetButton(j, b)) return b;
    for (int a = 0; a < SDL_JoystickNumAxes(j); a++) {
        int16_t rest = 0;
        if (SDL_JoystickGetAxisInitialState(j, a, &rest) && rest < -16000) continue;
        if (SDL_JoystickGetAxis(j, a) > 16000) return 100 + a;
    }
    return -1;
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
    fprintf(f, "%s %d %d %d %d %d %d %d %d  # %s: A B C START X Y Z MODE\n", s_guid, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], SDL_GameControllerName(s_pad));
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
    if (pad && s_mapped) {                          /* d-pad and stick as usual, buttons as set */
        static const SDL_GameControllerButton dirs[4] = { SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_DOWN,
                                                          SDL_CONTROLLER_BUTTON_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT };
        for (int i = 0; i < 4; i++) if (SDL_GameControllerGetButton(pad, dirs[i])) buttons |= (uint16_t)(1u << i);
        SDL_Joystick *j = SDL_GameControllerGetJoystick(pad);
        for (int i = 0; i < 8; i++) if (raw_down(j, s_map[i])) buttons |= k_map_btn[i];
    } else if (pad) {
        static const struct { SDL_GameControllerButton b; uint16_t g; } map[] = {
            {SDL_CONTROLLER_BUTTON_DPAD_UP, GEN_BTN_UP}, {SDL_CONTROLLER_BUTTON_DPAD_DOWN, GEN_BTN_DOWN},
            {SDL_CONTROLLER_BUTTON_DPAD_LEFT, GEN_BTN_LEFT}, {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, GEN_BTN_RIGHT},
            {SDL_CONTROLLER_BUTTON_X, GEN_BTN_A}, {SDL_CONTROLLER_BUTTON_A, GEN_BTN_B},
            {SDL_CONTROLLER_BUTTON_B, GEN_BTN_C}, {SDL_CONTROLLER_BUTTON_START, GEN_BTN_START},
            {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, GEN_BTN_X}, {SDL_CONTROLLER_BUTTON_Y, GEN_BTN_Y},
            {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, GEN_BTN_Z},
        };
        for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
            if (SDL_GameControllerGetButton(pad, map[i].b)) buttons |= map[i].g;
    }
    if (pad) {
        int ax = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX);
        int ay = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);
        if (ax < -16000) buttons |= GEN_BTN_LEFT;  if (ax > 16000) buttons |= GEN_BTN_RIGHT;
        if (ay < -16000) buttons |= GEN_BTN_UP;    if (ay > 16000) buttons |= GEN_BTN_DOWN;
    }
    buttons |= platform_scripted_buttons();
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
