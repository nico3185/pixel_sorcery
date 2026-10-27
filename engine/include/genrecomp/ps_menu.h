/* ps_menu.h — in-game settings menu and runtime cheats (see ps_menu.c). */
#ifndef PS_MENU_H
#define PS_MENU_H
#include <stdbool.h>
#include <stdint.h>
void ps_menu_set_title(const char *title);                 /* settings folder name */
void ps_menu_add_toggle(const char *name, uint32_t addr, uint16_t value, bool word);  /* RAM code */
/* RAM toggle written only while gate() holds (e.g. a level is running), not every frame */
void ps_menu_set_toggle_gate(const char *name, bool (*gate)(void));
void ps_menu_load(void);
void ps_menu_request(void);
void ps_menu_run(uint8_t *frame, int width);               /* modal while open */
void ps_menu_frame_pokes(void);
int  ps_cheat(int n);
/* The title's pause state: while paused, C opens the menu and a hint is shown */
void ps_menu_set_pause_hook(bool (*paused)(void));
bool ps_menu_paused(void);
void ps_menu_pause_hint(uint8_t *frame, int width);
#endif
