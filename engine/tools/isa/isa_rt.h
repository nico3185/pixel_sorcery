/* isa_rt.h: one instruction test as isa_check.py writes it (see isa_rt.c) */
#include <stdint.h>

typedef struct {
    void (*fn)(void);          /* the instruction, as generate.py translates it */
    const char *name;          /* the test's name in the suite */
    uint32_t init[19], fin[19];  /* D0-D7, A0-A6, USP, SSP, SR, PC */
    uint32_t ram0, nram0, ram1, nram1;   /* initial / final bytes: pairs (address, value) in isa_ram */
    uint32_t len;              /* instruction length in bytes */
    uint32_t cycles;           /* 68000 clocks */
    int vector;                /* exception expected, -1 = none */
    uint32_t vec_sr;           /* SR the CPU stacked for it */
} IsaTest;

extern const IsaTest isa_tests[];
extern const int isa_ntests;
extern const uint32_t isa_ram[];
