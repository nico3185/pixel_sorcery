/* ps_hw.h — machine state shared by the engine.
 *
 * Native build (PS_NATIVE): PixelSorcery's own Mega Drive hardware layer (ps_hw.c):
 * cartridge, 68000 memory map, Z80 memory and bus arbitration, I/O chip and pads.
 * Nothing from Genesis Plus GX is compiled in. The names match the oracle core's so the
 * scheduler and runtime are the same source in both builds.
 *
 * Oracle build: Genesis Plus GX provides these (tracer, in-process compare modes).
 */
#ifndef PS_HW_H
#define PS_HW_H

#ifdef PS_NATIVE
#include <stdbool.h>
#include <stdint.h>

#define MCYCLES_PER_LINE 3420
#define ASSERT_LINE 1
#define CLEAR_LINE  0
/* RAM/ROM are stored as host-order 16-bit words (byte A lives at index A ^ 1) */
#define READ_BYTE(BASE, ADDR)       (BASE)[(ADDR) ^ 1]
#define WRITE_BYTE(BASE, ADDR, VAL) (BASE)[(ADDR) ^ 1] = (VAL)

typedef uint8_t  uint8;
typedef uint16_t uint16;
typedef uint32_t uint32;

extern uint8_t  work_ram[0x10000];
extern uint8_t  zram[0x2000];
extern uint16_t v_counter;
extern uint16_t lines_per_frame;
extern uint32_t mcycles_vdp;
extern uint16_t status;
extern uint8_t  zstate;            /* bit 0: Z80 running (reset released), bit 1: bus requested */

typedef struct { unsigned int cycles; int irq_state; } ps_z80_clock_t;
extern ps_z80_clock_t Z80;         /* Z80 clock (master clocks) and interrupt line */

typedef struct { uint16_t pad[8]; } ps_input_t;
extern ps_input_t input;

/* controller-port data writes (ps_poll.c: input record/replay, checkpoints) */
extern void (*io_pad_write_hook)(int port, unsigned int data, unsigned int ctrl);

bool     ps_hw_load_rom(const char *path);
const uint8_t *ps_hw_rom(uint32_t *size);
void     ps_hw_reset(void);
void     ps_hw_end_frame(unsigned int cycles);

/* 68000 accesses outside the VDP ports (handled by ps_vdp) */
uint8_t  ps_hw_read8(uint32_t addr);
uint16_t ps_hw_read16(uint32_t addr);
void     ps_hw_write8(uint32_t addr, uint8_t val);
void     ps_hw_write16(uint32_t addr, uint16_t val);

/* Z80 accesses above its RAM (YM2612, bank register, PSG, 68000 window) */
uint8_t  ps_hw_z80_read(uint16_t addr);
void     ps_hw_z80_write(uint16_t addr, uint8_t val);
void     z80_run(unsigned int cycles);

/* 68K-bus DMA end time, for a Z80 waiting on the 68000 bus (bus.c) */
extern unsigned int g_dma68k_end;

#else
#include "shared.h"
#endif

/* input.pad[] index of controller port 0/1. GPGX numbers devices per port with room for a
 * multitap (gamepad_2_read() reads input.pad[4]); the native layer reads input.pad[1]. */
#ifdef PS_NATIVE
#define PS_PAD_INDEX(port) (port)
#else
#define PS_PAD_INDEX(port) ((port) * 4)
#endif

#endif
