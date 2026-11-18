/* a stand-in title for the one-executable build's check (CI, no ROM): its names are the same as the other
 * stand-in's - main, value, shared_value, the tables - and each must still run its own */
#include <stdio.h>
int value(void);
int main(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("title tb: %d\n", value());
    return value();
}
