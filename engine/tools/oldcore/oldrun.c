/*
 * oldrun: play an input movie on Genesis Plus GX as BizHawk 1.11 shipped it, and record the
 * pads per controller poll, which is what the port and the oracle replay (PS_POLL_REPLAY).
 *
 * BizHawk 1.11's Genplus-gx predates GPGX's December 2014 frame timing rewrite (ec554b4,
 * "frame emulation now starts with vertical blanking"): on today's core a BizHawk TAS
 * presses its buttons in the wrong frames and drifts at the first load. Built by
 * build.sh against GPGX 8744c39 (the commit before), it plays such movies in sync.
 * Recording per poll removes frame timing from the result: the polls replay on the
 * current core and on the port the same way.
 *
 *   oldrun ROM MOVIE.txt POLLS|- FRAMES [WATCH_ADDR ...] [-ppm DIR:EVERY]
 *
 * MOVIE: "P1 P2" hex per frame in GEN_BTN bits (bk2_to_movie.py). POLLS: "P1 P2" hex per
 * TH 0->1 write to port A (ps_poll.c's format). Each WATCH_ADDR (hex, 68K RAM word) is
 * printed with frame and poll whenever one changes. Six-button pads like BizHawk's
 * default (OLDRUN_PAD3=1 for three), region auto-detect (OLDRUN_REGION=1..4 to force).
 */
#include "shared.h"

t_config config;

static uint16_t *s_mv;
static long s_mvn, s_frame, s_polls;
static FILE *s_out;
static unsigned s_th = 0x40;
static uint16_t s_pad;

extern void (*io_pad_write_hook)(int port, unsigned int data, unsigned int ctrl);

static uint16_t to_gpgx(uint16_t b) {
    /* GEN_BTN_*: Up 1 Down 2 Left 4 Right 8 B 10 C 20 A 40 Start 80 X 100 Y 200 Z 400 Mode 800 */
    static const uint16_t map[12] = { INPUT_UP, INPUT_DOWN, INPUT_LEFT, INPUT_RIGHT, INPUT_B, INPUT_C,
                                      INPUT_A, INPUT_START, INPUT_X, INPUT_Y, INPUT_Z, INPUT_MODE };
    uint16_t r = 0;
    for (int i = 0; i < 12; i++)
        if (b & (1 << i)) r |= map[i];
    return r;
}

void oldrun_input_update(void) {
    input.pad[0] = to_gpgx(s_pad);
    input.pad[1] = 0;
}

/* a poll, as ps_poll.c counts it: TH driven 0 -> 1 on port A */
static void pad_write(int port, unsigned int data, unsigned int ctrl) {
    if (port) return;
    unsigned th = (ctrl & 0x40) ? (data & 0x40) : 0x40;
    if (th && !s_th) {
        s_polls++;
        if (s_out) fprintf(s_out, "%04X 0000\n", s_pad);
    }
    s_th = th;
}

int load_archive(char *filename, unsigned char *buffer, int maxsize, char *extension) {
    FILE *f = fopen(filename, "rb");
    if (!f) return 0;
    int n = (int)fread(buffer, 1, (size_t)maxsize, f);
    fclose(f);
    if (extension) {
        const char *dot = strrchr(filename, '.');
        strncpy(extension, dot ? dot + 1 : "bin", 3);
        extension[3] = 0;
    }
    return n;
}

static void ppm(const char *dir, long frame) {
    char path[1024];
    snprintf(path, sizeof path, "%s/f%06ld.ppm", dir, frame);
    FILE *o = fopen(path, "wb");
    if (!o) return;
    int w = bitmap.viewport.w, h = bitmap.viewport.h;
    fprintf(o, "P6\n%d %d\n255\n", w, h);
    for (int y = 0; y < h; y++) {
        const uint32_t *row = (const uint32_t *)(bitmap.data + (y + bitmap.viewport.y) * bitmap.pitch) + bitmap.viewport.x;
        for (int x = 0; x < w; x++) {
            uint8 rgb[3] = { (uint8)(row[x] >> 16), (uint8)(row[x] >> 8), (uint8)row[x] };
            fwrite(rgb, 1, 3, o);
        }
    }
    fclose(o);
}

int main(int argc, char **argv) {
    if (argc < 5) {
        fprintf(stderr, "usage: oldrun ROM MOVIE.txt POLLS|- FRAMES [WATCH_ADDR ...] [-ppm DIR:EVERY]\n");
        return 1;
    }
    FILE *m = fopen(argv[2], "r");
    if (!m) { fprintf(stderr, "oldrun: cannot open %s\n", argv[2]); return 1; }
    char line[128];
    long cap = 1 << 17;
    s_mv = malloc((size_t)cap * sizeof *s_mv);
    while (fgets(line, sizeof line, m)) {
        unsigned p1;
        if (line[0] == '#' || sscanf(line, "%x", &p1) != 1) continue;
        if (s_mvn == cap) s_mv = realloc(s_mv, (size_t)(cap *= 2) * sizeof *s_mv);
        s_mv[s_mvn++] = (uint16_t)p1;
    }
    fclose(m);
    s_out = strcmp(argv[3], "-") ? fopen(argv[3], "w") : NULL;
    long frames = atol(argv[4]);
    unsigned watch[8];
    int nw = 0;
    char *ppm_dir = NULL;
    long ppm_every = 0;
    for (int i = 5; i < argc; i++) {
        if (!strcmp(argv[i], "-ppm") && i + 1 < argc) {
            ppm_dir = argv[++i];
            char *c = strchr(ppm_dir, ':');
            if (c) { *c = 0; ppm_every = atol(c + 1); }
        } else if (nw < 8) watch[nw++] = (unsigned)strtoul(argv[i], NULL, 16);
    }

    memset(&config, 0, sizeof config);
    config.psg_preamp = 150;
    config.fm_preamp = 100;
    config.hq_fm = 1;
    config.addr_error = 1;
    config.region_detect = getenv("OLDRUN_REGION") ? (uint8)atoi(getenv("OLDRUN_REGION")) : 0;
    for (int i = 0; i < MAX_INPUTS; i++) config.input[i].padtype = getenv("OLDRUN_PAD3") ? DEVICE_PAD3B : DEVICE_PAD6B;

    static uint8 fb[720 * 576 * 4];
    memset(&bitmap, 0, sizeof bitmap);
    bitmap.width = 720;
    bitmap.height = 576;
    bitmap.pitch = 720 * 4;
    bitmap.data = fb;
    if (!load_rom(argv[1])) { fprintf(stderr, "oldrun: cannot load %s\n", argv[1]); return 1; }
    audio_init(44100, 60.0);
    input.system[0] = input.system[1] = SYSTEM_GAMEPAD;
    system_init();
    system_reset();
    io_pad_write_hook = pad_write;

    static int16 snd[4096];
    uint16_t last[8];
    for (int k = 0; k < 8; k++) last[k] = 0xFFFF;
    for (s_frame = 0; s_frame < frames; s_frame++) {
        s_pad = s_frame < s_mvn ? s_mv[s_frame] : 0;
        system_frame_gen(0);
        audio_update(snd);
        int changed = 0;
        for (int k = 0; k < nw; k++)
            if (*(uint16_t *)(work_ram + (watch[k] & 0xFFFE)) != last[k]) changed = 1;
        if (changed) {
            printf("f=%ld poll=%ld", s_frame, s_polls);
            for (int k = 0; k < nw; k++) {
                last[k] = *(uint16_t *)(work_ram + (watch[k] & 0xFFFE));
                printf(" %06X=%04X", watch[k], last[k]);
            }
            printf("\n");
        }
        if (ppm_dir && ppm_every && s_frame % ppm_every == 0) ppm(ppm_dir, s_frame);
    }
    if (s_out) fclose(s_out);
    fprintf(stderr, "oldrun: %ld frames, %ld polls\n", s_frame, s_polls);
    return 0;
}
