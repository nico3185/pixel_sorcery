#include <stdint.h>
int shared_value = 2;
int value(void) { return shared_value; }
const char *ps_cheat_names[] = { "CHEAT tb", 0 };
int ps_cheat_count = 1;
