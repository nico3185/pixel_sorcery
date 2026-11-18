/* Ultimate SH, native (BACKLOG 0.7.5): one executable, every title it holds (titles.h, from
 * CMakeLists.txt's ULTIMATE_TITLES) and the game-select screen (tools/launcher, its disc look).
 *   ultimate_sh [--roms DIR] [--fullscreen]        the game-select screen (ultimate.txt beside the
 *                                                     executable, or in the app's Resources, or PS_ULTIMATE_CONFIG)
 *   ultimate_sh --game ID ROM [title's options]    run title ID (as its own executable would)
 *   ultimate_sh --list                             the titles held */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define PS_TITLE(id) int id##_entry(int, char **);
#include "titles.h"
#undef PS_TITLE
typedef struct { const char *id; int (*entry)(int, char **); } Title;
static const Title k_titles[] = {
#define PS_TITLE(id) { #id, id##_entry },
#include "titles.h"
#undef PS_TITLE
};
#define NTITLES ((int)(sizeof k_titles / sizeof k_titles[0]))
int launcher_main(int argc, char **argv);

static int exists(const char *p) { struct stat st; return stat(p, &st) == 0; }

int main(int argc, char *argv[]) {
    if (argc >= 2 && !strcmp(argv[1], "--list")) {
        for (int i = 0; i < NTITLES; i++) puts(k_titles[i].id);
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "--game")) {
        for (int i = 0; i < NTITLES; i++)
            if (!strcmp(argv[2], k_titles[i].id)) {
                argv[2] = argv[0];                     /* the title sees: PROGRAM ROM [options] */
                return k_titles[i].entry(argc - 2, argv + 2);
            }
        fprintf(stderr, "no title %s (--list)\n", argv[2]);
        return 2;
    }
    char cfg[1200] = "", dir[1100];
    snprintf(dir, sizeof dir, "%s", argv[0]);
    char *sl = strrchr(dir, '/'); if (!sl) sl = strrchr(dir, '\\');
    if (sl) *sl = 0; else snprintf(dir, sizeof dir, ".");
    if (getenv("PS_ULTIMATE_CONFIG")) snprintf(cfg, sizeof cfg, "%s", getenv("PS_ULTIMATE_CONFIG"));
    else {
        snprintf(cfg, sizeof cfg, "%s/ultimate.txt", dir);
        if (!exists(cfg)) snprintf(cfg, sizeof cfg, "%s/../Resources/ultimate.txt", dir);   /* macOS app */
    }
    if (!exists(cfg)) { fprintf(stderr, "ultimate_sh: no ultimate.txt beside the executable\n"); return 2; }
    char **av = calloc((size_t)argc + 3, sizeof *av);
    int n = 0;
    av[n++] = argv[0]; av[n++] = cfg; av[n++] = "@self";
    for (int i = 1; i < argc; i++) av[n++] = argv[i];
    return launcher_main(n, av);
}
