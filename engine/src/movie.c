/*
 * Input-movie playback and RAM watches, for coverage runs and sync checks.
 *
 *   GENRECOMP_MOVIE=path        Gens .gmv, or a text file with one line per frame:
 *                               "P1 [P2]" hex masks in GEN_BTN_* bits.
 *   GENRECOMP_MOVIE_OFFSET=N    shift playback by N frames (positive = later).
 *   GENRECOMP_RECORD=path       record the pads, one "P1 P2" hex line per frame,
 *                               in the text format GENRECOMP_MOVIE plays back.
 *   GENRECOMP_WATCH=ADDR[,ADDR] print these 16-bit work-RAM words (hex, e.g.
 *                               FFF300,FFFF44) every GENRECOMP_WATCH_EVERY
 *                               frames (default 600) and whenever they change.
 *   GENRECOMP_POKE=C=V:A=W[;...]  once, the first frame the 16-bit word at C
 *                               equals V, write W to A (all hex). Used to send a
 *                               coverage run to a chosen level.
 *
 * Gens GMV: 64-byte header ("Gens Movie TEST" + version), then 3 bytes per
 * frame, active low: byte0 = P1 Up,Down,Left,Right,A,B,C,Start (bits 0-7),
 * byte1 = P2 same, byte2 = P1 X,Y,Z,Mode (bits 0-3) / P2 (bits 4-7).
 */

#include "genrecomp/input.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Read GPGX's work RAM directly (native-endian words, as bus.c does): going
 * through the bus from the input hook disturbs the interpreter-only tracer. */
extern unsigned char work_ram[0x10000];
static uint16_t ram16(uint32_t addr) { return *(uint16_t *)(work_ram + (addr & 0xFFFE)); }

static uint16_t *s_movie[2];
static long s_movie_len = -1;      /* -1 = not loaded yet, 0 = no movie */
static long s_offset;

static uint16_t gmv_main(uint8_t b) {
    uint8_t p = (uint8_t)~b;
    uint16_t m = 0;
    if (p & 0x01) m |= GEN_BTN_UP;
    if (p & 0x02) m |= GEN_BTN_DOWN;
    if (p & 0x04) m |= GEN_BTN_LEFT;
    if (p & 0x08) m |= GEN_BTN_RIGHT;
    if (p & 0x10) m |= GEN_BTN_A;
    if (p & 0x20) m |= GEN_BTN_B;
    if (p & 0x40) m |= GEN_BTN_C;
    if (p & 0x80) m |= GEN_BTN_START;
    return m;
}

static uint16_t gmv_extra(uint8_t nib) {
    uint8_t p = (uint8_t)(~nib & 0x0F);
    uint16_t m = 0;
    if (p & 0x01) m |= GEN_BTN_X;
    if (p & 0x02) m |= GEN_BTN_Y;
    if (p & 0x04) m |= GEN_BTN_Z;
    if (p & 0x08) m |= GEN_BTN_MODE;
    return m;
}

static void movie_load(void) {
    s_movie_len = 0;
    const char *path = getenv("GENRECOMP_MOVIE");
    if (!path || !*path) return;
    const char *off = getenv("GENRECOMP_MOVIE_OFFSET");
    s_offset = off ? atol(off) : 0;
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "movie: cannot open %s\n", path); return; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc((size_t)size + 1);
    if (!data || fread(data, 1, (size_t)size, f) != (size_t)size) { fclose(f); free(data); return; }
    fclose(f);
    data[size] = 0;

    if (size >= 64 && !memcmp(data, "Gens Movie TEST", 15)) {
        long n = (size - 64) / 3;
        s_movie[0] = calloc((size_t)n, sizeof(uint16_t));
        s_movie[1] = calloc((size_t)n, sizeof(uint16_t));
        for (long i = 0; i < n; i++) {
            const uint8_t *r = data + 64 + i * 3;
            s_movie[0][i] = gmv_main(r[0]) | gmv_extra(r[2] & 0x0F);
            s_movie[1][i] = gmv_main(r[1]) | gmv_extra(r[2] >> 4);
        }
        s_movie_len = n;
        fprintf(stderr, "movie: %s (Gens GMV v%c, rerecords %u, pads %c/%c, %ld frames)\n",
                path, data[15], data[16] | data[17] << 8 | data[18] << 16 | (unsigned)data[19] << 24,
                data[20], data[21], n);
    } else {
        long cap = 1024, n = 0;
        s_movie[0] = malloc(cap * sizeof(uint16_t));
        s_movie[1] = malloc(cap * sizeof(uint16_t));
        for (char *line = strtok((char *)data, "\n"); line; line = strtok(NULL, "\n")) {
            if (*line == '#') continue;
            unsigned a = 0, b = 0;
            if (sscanf(line, "%x %x", &a, &b) < 1) continue;
            if (n == cap) {
                cap *= 2;
                s_movie[0] = realloc(s_movie[0], cap * sizeof(uint16_t));
                s_movie[1] = realloc(s_movie[1], cap * sizeof(uint16_t));
            }
            s_movie[0][n] = (uint16_t)a;
            s_movie[1][n] = (uint16_t)b;
            n++;
        }
        s_movie_len = n;
        fprintf(stderr, "movie: %s (text, %ld frames)\n", path, n);
    }
    free(data);
}

long movie_length(void) {
    if (s_movie_len < 0) movie_load();
    return s_movie_len;
}

uint16_t movie_buttons(long frame, int port) {
    if (s_movie_len < 0) movie_load();
    long i = frame - s_offset;
    if (port < 0 || port > 1 || s_movie_len <= 0 || i < 0 || i >= s_movie_len) return 0;
    return s_movie[port][i];
}

static void ram_write16(uint32_t addr, uint16_t v) { *(uint16_t *)(work_ram + (addr & 0xFFFE)) = v; }

static void movie_pokes(long frame) {
    static int n = -1;
    static struct { uint32_t c, a; uint16_t v, w; int done; } p[8];
    if (n < 0) {
        n = 0;
        const char *e = getenv("GENRECOMP_POKE");
        for (const char *q = e; q && *q && n < 8; ) {
            unsigned c, v, a, w;
            if (sscanf(q, "%x=%x:%x=%x", &c, &v, &a, &w) == 4) {
                p[n].c = c; p[n].v = (uint16_t)v; p[n].a = a; p[n].w = (uint16_t)w; p[n].done = 0; n++;
            }
            q = strchr(q, ';');
            if (q) q++;
        }
    }
    for (int k = 0; k < n; k++)
        if (!p[k].done && ram16(p[k].c) == p[k].v) {
            ram_write16(p[k].a, p[k].w);
            p[k].done = 1;
            fprintf(stderr, "poke f=%ld %06X=%04X\n", frame, p[k].a, p[k].w);
        }
}

void movie_watch(long frame) {
    movie_pokes(frame);
    static int n = -1;
    static uint32_t addr[8];
    static uint16_t last[8];
    static long every = 600;
    if (n < 0) {
        n = 0;
        const char *w = getenv("GENRECOMP_WATCH");
        const char *e = getenv("GENRECOMP_WATCH_EVERY");
        if (e) every = atol(e) > 0 ? atol(e) : 600;
        for (const char *p = w; p && *p && n < 8; ) {
            addr[n++] = (uint32_t)strtoul(p, NULL, 16) & 0xFFFFFF;
            p = strchr(p, ',');
            if (p) p++;
        }
        for (int k = 0; k < n; k++) last[k] = 0xFFFF;
    }
    if (!n) return;
    int changed = 0;
    uint16_t v[8];
    for (int k = 0; k < n; k++) {
        v[k] = ram16(addr[k]);
        if (v[k] != last[k]) changed = 1;
    }
    if (!changed && frame % every) return;
    fprintf(stderr, "watch f=%ld", frame);
    for (int k = 0; k < n; k++) { fprintf(stderr, " %06X=%04X", addr[k], v[k]); last[k] = v[k]; }
    fprintf(stderr, "%s\n", changed ? " *" : "");
}

/* Input recording (GENRECOMP_RECORD): one line per frame, replayable as a movie. */
void movie_record(long frame, uint16_t p1, uint16_t p2) {
    static FILE *f;
    static int init;
    static long next;
    if (!init) {
        init = 1;
        const char *path = getenv("GENRECOMP_RECORD");
        if (path && *path) f = fopen(path, "w");
    }
    if (!f) return;
    for (; next < frame; next++) fprintf(f, "0000 0000\n");   /* fill any skipped frames */
    if (frame == next) { fprintf(f, "%04X %04X\n", p1, p2); next++; }
    if (frame % 600 == 0) fflush(f);
}
