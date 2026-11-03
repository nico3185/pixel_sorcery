/* MinGW default (ps_defaults archive member, pulled only when no title defines these): no
 * recompiled Z80 driver. See ps_audio.c. */
#include "genrecomp/ps_z80rt.h"
void (*ps_z80_game_driver)(PsZ80 *) = 0;
const unsigned char *ps_z80_game_image = 0;
unsigned int ps_z80_game_image_len = 0;
