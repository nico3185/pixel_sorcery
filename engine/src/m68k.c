/*
 * Recomp CPU state — the M68K registers your recompiled code uses.
 *
 * This is completely separate from Genesis Plus GX's M68K emulation.
 * In a recomp, YOUR native C code IS the CPU. This struct just holds
 * the register file so recompiled instructions can read/write D0-D7,
 * A0-A7, flags, etc.
 */

#include "genrecomp/m68k.h"
#include "genrecomp/bus.h"
#include "genrecomp/func_table.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static uint32_t s_iframe;   /* exception frame being handled (interrupt, TRAP, ...): its RTE returns */
static int s_iframe_depth;   /* C call depth where its handler was called */

M68kContext g_m68k;

void recomp_m68k_reset(void) {
    memset(&g_m68k, 0, sizeof(g_m68k));

    /* Power-on defaults: supervisor mode, all interrupts masked */
    g_m68k.flag_S = true;
    g_m68k.int_mask = 7;

    /* Read initial SSP and PC from vector table (ROM addresses 0x000000-0x000007) */
    g_m68k.ssp = bus_read32(0x000000);
    g_m68k.pc  = bus_read32(0x000004);
    g_m68k.a[7] = g_m68k.ssp;

    g_m68k.cycles = 0;
    g_m68k.target_cycles = 0;
}

void recomp_m68k_exception(uint8_t vector) {
    /*
     * M68K exception processing:
     * 1. Switch to supervisor mode
     * 2. Push PC and SR on supervisor stack
     * 3. Load new PC from vector table
     * 4. Dispatch to handler via func_table_call
     *
     * For TRAP instructions (vectors 32-47), the handler is called
     * directly. The handler saves/restores registers and ends with RTE,
     * which pops the saved PC and SR from the stack.
     */
    uint16_t old_sr = m68k_get_sr();
    /* exception processing time: TRAP/illegal 34 cycles, CHK and divide-by-zero 38 */
    g_cyc += (vector == 5 || vector == 6 ? 38 : 34) * 7;

    /* Enter supervisor mode */
    if (!g_m68k.flag_S) {
        g_m68k.usp = g_m68k.a[7];
        g_m68k.a[7] = g_m68k.ssp;
        g_m68k.flag_S = true;
    }

    /* Push PC (long) then SR (word) onto supervisor stack */
    g_m68k.a[7] -= 4;
    bus_write32(g_m68k.a[7], g_m68k.pc);
    g_m68k.a[7] -= 2;
    bus_write16(g_m68k.a[7], old_sr);

    /* Load new PC from vector table */
    uint32_t handler_addr = bus_read32((uint32_t)vector * 4);
    g_m68k.pc = handler_addr;

    /* Update SSP */
    g_m68k.ssp = g_m68k.a[7];

    /* Dispatch to the handler function.
     * The recompiled handler ends with "return; // RTE" which just returns
     * here. The original RTE would pop SR and PC from the supervisor stack,
     * but since we called via C function call, we need to clean up the
     * exception frame we pushed above. */
    uint32_t outer = s_iframe; int outer_d = s_iframe_depth;
    s_iframe = g_m68k.a[7]; s_iframe_depth = func_table_depth();
    func_table_call(handler_addr);
    func_table_unwind_done(s_iframe_depth);
    s_iframe = outer; s_iframe_depth = outer_d;

    /* Pop the exception frame: SR (word) then PC (long) */
    uint16_t restored_sr = bus_read16(g_m68k.a[7]);
    g_m68k.a[7] += 2;
    g_m68k.pc = bus_read32(g_m68k.a[7]);
    g_m68k.a[7] += 4;
    g_m68k.ssp = g_m68k.a[7];
    m68k_set_sr(restored_sr);             /* back to user mode: A7 = USP again */
}

uint8_t g_m68k_irq_pending;  /* highest requested level not yet taken */
static uint8_t s_irq_bits;    /* every requested level not yet taken (the VDP holds each line) */

static void irq_update_highest(void) {
    g_m68k_irq_pending = 0;
    for (int l = 7; l > 0; l--) if (s_irq_bits & (1u << l)) { g_m68k_irq_pending = (uint8_t)l; break; }
}

/* The VDP withdraws a held-back request when its enable bit is cleared (IE0/IE1). */
void recomp_m68k_irq_drop(uint8_t level) {
    s_irq_bits &= (uint8_t)~(1u << level);
    irq_update_highest();
}

uint32_t g_m68k_ipc, g_m68k_redirect;
uint8_t g_m68k_ipc_kind;
bool recomp_m68k_in_exception(void) { return s_iframe != 0; }
void recomp_m68k_reset_frames(void) { s_iframe = 0; }
/* ps_state.c: the CPU and its interrupt bookkeeping */
void ps_state_blob(void *p, unsigned long n);
void recomp_m68k_state(void) {
    ps_state_blob(&g_m68k, sizeof g_m68k);
    ps_state_blob(&g_m68k_irq_pending, sizeof g_m68k_irq_pending);
    ps_state_blob(&s_irq_bits, sizeof s_irq_bits);
    ps_state_blob(&g_m68k_ipc, sizeof g_m68k_ipc);
    ps_state_blob(&g_m68k_redirect, sizeof g_m68k_redirect);
    ps_state_blob(&g_m68k_ipc_kind, sizeof g_m68k_ipc_kind);
}
uint32_t g_m68k_rte_target;                 /* last RTE used as a jump */
/* RTE: at the interrupt's own frame it ends the handler (recomp_m68k_interrupt restores the
 * context); anywhere else it is a jump through a stacked SR/PC the code pushed itself
 * (SH3 resumes its decompressor that way): pop them, true with the PC. */
bool m68k_rte(uint32_t *pc) {
    uint32_t sp = g_m68k.a[7];
    if (s_iframe && sp == s_iframe) { func_table_unwind_to(s_iframe_depth); return false; }
    uint16_t sr = bus_read16(sp);
    *pc = g_m68k_rte_target = bus_read32(sp + 2) & 0xFFFFFF;
    g_m68k.a[7] = sp + 6;
    m68k_set_sr(sr);
    return true;
}
void recomp_m68k_interrupt(uint8_t level) {
    /* Masked (SR I2-I0 >= level, level 7 excepted): remember it; the bus
     * clock takes it as soon as the mask drops, like the hardware does. */
    s_irq_bits |= (uint8_t)(1u << level);
    irq_update_highest();
    if (level <= g_m68k.int_mask && level != 7) return;
    s_irq_bits &= (uint8_t)~(1u << level);
    irq_update_highest();

    /* The hardware stacks PC and SR and RTE restores them; the handler
     * saves any registers it uses. Recompiled code is interrupted in the
     * middle of a bus access, possibly between setting flags and branching
     * on them, so everything is restored here, not only what RTE would. */
    /* interrupt acknowledge + exception processing: 50-59 cycles by E-clock phase */
    static const unsigned char irq_cyc[10] = {50, 59, 58, 57, 56, 55, 54, 53, 52, 51};
    { unsigned bus_frame_cycles(void); g_cyc += irq_cyc[(bus_frame_cycles() / 7) % 10] * 7; }
    uint32_t pc = g_m68k_ipc;
    uint8_t kind = g_m68k_ipc_kind;
    uint16_t sr = m68k_get_sr();
    M68kContext saved = g_m68k;
    g_m68k.flag_S = true;
    g_m68k.int_mask = level;
    /* the exception frame, as the hardware stacks it: SR, then the return PC (on the
     * supervisor stack; the whole context, USP included, is restored afterwards) */
    uint32_t frame = (saved.flag_S ? saved.a[7] : saved.ssp) - 6;
    g_m68k.a[7] = frame;
    bus_write16(frame, sr); bus_write32(frame + 2, pc);
    { void bus_irq_ack(uint8_t level); bus_irq_ack(level); }   /* VDP: pending flag cleared on acknowledge */
    uint32_t outer = s_iframe; int outer_d = s_iframe_depth;
    s_iframe = frame; s_iframe_depth = func_table_depth();
    func_table_call(bus_read32((uint32_t)(24 + level) * 4));
    func_table_unwind_done(s_iframe_depth);
    s_iframe = outer; s_iframe_depth = outer_d;
    uint32_t ret = bus_read32(frame + 2) & 0xFFFFFF;
    uint32_t a7_after = g_m68k.a[7];
    g_m68k = saved;
    /* the handler rewrote the return address (it ended with RTE at its own frame) */
    if (pc && a7_after == frame && ret != pc) g_m68k_redirect = ret;
    /* RTE lowered the mask: an interrupt that became pending meanwhile is taken at once */
    g_m68k_ipc = pc && g_m68k_redirect ? g_m68k_redirect : pc;   /* where a following interrupt returns */
    g_m68k_ipc_kind = kind;
    if (g_m68k_irq_pending > g_m68k.int_mask)
        recomp_m68k_interrupt(g_m68k_irq_pending);
}
