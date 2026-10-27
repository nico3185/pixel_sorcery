/*
 * Controller state into Genesis Plus GX's input system. GPGX's input.pad[]
 * uses the same bit layout as GEN_BTN_* (input.h), so no translation.
 */

#include "genrecomp/io.h"
#include "ps_hw.h"
#ifndef MAX_DEVICES
#define MAX_DEVICES 8
#endif

void io_set_pad_state(int port, uint16_t buttons) {
    if (port >= 0 && port < 2) {
        input.pad[PS_PAD_INDEX(port)] = buttons;   /* port 1 = GPGX device 4 */
    }
}
