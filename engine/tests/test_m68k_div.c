/*
 * DIVS edge cases, run on every OS by ctest (no Genesis Plus GX needed).
 * $80000000 / -1 has no int32_t quotient: on x86 a 32-bit C division traps (SIGFPE) and the game
 * crashed, while on Apple silicon it quietly gave a value the overflow check then caught. A 68000
 * sets V and leaves the register unchanged. The other cases pin the rest of the overflow edge and
 * the sign of the remainder.
 */

#include <genrecomp/m68k.h>
#include <stdio.h>

M68kContext g_m68k;
unsigned int g_cyc, g_cyc_event, g_refresh;

static int s_fail;

static void divs(uint32_t d, uint16_t s, uint32_t want, bool want_v)
{
    uint32_t r = d;
    g_m68k.flag_V = !want_v;
    g_m68k.flag_C = true;
    M68K_DIVS(r, s);
    if (r != want || g_m68k.flag_V != want_v || g_m68k.flag_C) {
        printf("FAIL DIVS $%08X / $%04X: $%08X V=%d C=%d, want $%08X V=%d C=0\n",
               (unsigned)d, (unsigned)s, (unsigned)r, g_m68k.flag_V, g_m68k.flag_C,
               (unsigned)want, want_v);
        s_fail = 1;
    }
}

int main(void)
{
    divs(0x80000000u, 0xFFFF, 0x80000000u, true);   /* INT32_MIN / -1: overflow (trapped on x86) */
    divs(0x80000000u, 0x0001, 0x80000000u, true);   /* overflow */
    divs(0xFFFF8000u, 0xFFFF, 0xFFFF8000u, true);   /* -32768 / -1 = 32768: overflow */
    divs(0x00010000u, 0x0002, 0x00010000u, true);   /* 65536 / 2 = 32768: overflow */
    divs(0x00008000u, 0xFFFF, 0x00008000u, false);  /* 32768 / -1 = -32768: fits */
    divs(0x0000FFFEu, 0x0002, 0x00007FFFu, false);  /* 65534 / 2 = 32767: fits */
    divs(0xFFFFFFF9u, 0x0002, 0xFFFFFFFDu, false);  /* -7 / 2 = -3 rem -1 */
    divs(0x00000007u, 0xFFFE, 0x0001FFFDu, false);  /* 7 / -2 = -3 rem 1 */
    if (!s_fail) printf("DIVS: all cases OK\n");
    return s_fail;
}
