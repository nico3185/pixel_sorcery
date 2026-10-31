/*
 * explore: guided exploration of a game on the reference interpreter (Go-Explore style),
 * to get input runs that reach every level for the side-by-side checks.
 *
 * Cells are the game's own position: a few RAM values (level, X/Y >> shift, ...). The
 * archive keeps a savestate for the first visit of each cell. Each iteration resumes a
 * cell (high score or rarely tried), plays a random burst (held directions, jumps,
 * attacks), and adds the cells it reaches. A burst that costs health or a life is cut
 * there: what stays is a clean route.
 *
 * When nothing new turns up for --stuck iterations, or the best score has not moved for
 * --plateau iterations, explore writes DIR/stuck_N.png (the
 * best cell's screen) and DIR/stuck_N.txt, logs "STUCK N" to DIR/events.log, and waits
 * for DIR/hint.txt (a person, or Claude, looks and answers). Hint lines:
 *   macro [CELL|best] INPUTS   play INPUTS from that cell, e.g. "RIGHT:30 C+RIGHT:12 B:6"
 *                              (the end state joins the archive even if no cell is new)
 *   score ADDR:SIZE:WEIGHT,..  replace the score (e.g. a level that goes left or up)
 *   cell ADDR:SIZE:SHIFT       one more cell variable (a room index found at the stuck
 *                              point): every cell is re-keyed from its state
 *   play [CELL|best] FILE      play inputs recorded by hand ("P1 P2" hex per frame, e.g. from
 *                              genrecomp_ref GENRECOMP_STATE=stuck_N.state GENRECOMP_PLAY_OUT=FILE)
 *   damage on|off              allow bursts that cost health (not lives)
 *   policy PCT T1 | T2 | ...   force an action policy: PCT% of the bursts are built from
 *                              these templates (INPUTS as for macro), each step's length
 *                              varied +-30%, after a random 0-30 frame wait, repeated 1-3x
 *                              (what the screenshot calls for: "C+RIGHT:24 RIGHT:8")
 *   go                         keep exploring as before
 *   done                       write the route and stop
 * --novelty (build with -DGENRECOMP_TRACE=ON: GPGX's per-instruction hook): new code is a reward
 * too. Every 68000 address the original executes is marked; a burst that runs code never run
 * before keeps its end state as a cell (even without a new position) credited with the count,
 * and a quarter of the picks favour such cells (the next level stays the main goal), per level: while a level (the first cell variable)
 * still yields new code its cells are explored; once it has yielded none for --saturate
 * iterations it counts as covered and the forward score (level, position) takes over, to
 * reach the next one. New code also counts as progress for --stuck.
 * DIR/status.log gets a line every 500 iterations. DIR/route.txt is the best cell's route from power-on, one "P1 P2" hex line per frame
 * (GENRECOMP_MOVIE format), rewritten whenever the best cell changes.
 *
 *   explore ROM --dir DIR --start F [--press F:BTN[:LEN] ...] --cell ADDR:SIZE:SHIFT ...
 *           --score ADDR:SIZE:WEIGHT,... [--lives ADDR:SIZE] [--health ADDR:SIZE]
 *           [--buttons C,B,A] [--stuck 3000] [--iters N] [--seed N] [--damage] [--fast] [--patch ADDR=WORD ...]
 * --fast: frames are not drawn during bursts (several times faster; check a route replays the
 * same with and without it, games that read the VDP sprite collision flag would differ) [--plateau N] [--policy "PCT T1 | T2"]
 * --patch: ROM words written at start (cheats, as sidebyside.json's oracle "cheats") [--prior FILE [--prior-mix PERCENT]]
 * --prior: an action prior learned from a TAS (action_prior.py): that share of the bursts
 * (default 50%) replays how the TAS player chains and holds combinations, instead of
 * uniformly random ones.
 *   explore ROM --try DIR/stuck_N.state --inputs "C+RIGHT:24 RIGHT:30" --shots OUT:10
 *           [--patch ...] [--cell ...]: play inputs from a saved state, screenshot every 10
 *           frames and print the cell variables (seconds, instead of a replay from power-on)
 *   explore ROM --movie route.txt --save-state FILE [--patch ...]: the state at the end of a
 *           route, to --try from or to play from (genrecomp_ref GENRECOMP_STATE)
 *   --state FILE --prefix-route ROUTE: start from a saved state (cheats --patch applied
 *           at once), ROUTE being the inputs that led there (kept for route.txt).
 *   --anchors PLAY [--anchor-every 120]: a person's play from the start, one checkpoint
 *           cell every 120 frames of it: exploration goes on from where they got to. Progress
 *           is then the checkpoint's order in the play, then the score gained since it (room
 *           numbers, X... are not in play order across rooms).
 * Coverage mode (engine/tools/cover_loop.sh): --cover-base FILE (a PS_PCCOVER bitmap: the
 * instructions recordings already ran) makes novelty = 68000 instructions nobody executed yet:
 * a frame that runs one adds a cell (with --cell variables too, as usual), the cell is picked
 * in proportion to the new code it and its bursts found. No waiting for hints. Options:
 *   --polls FILE [--anchor-every F]  replay a recording (per poll, as sidebyside.py does) from
 *                         power-on, a cell every F frames of it (default 600): exploration
 *                         branches off anywhere along a person's play
 *   --camera SHIFT        cell variables any title has: the camera (plane-A scroll X, Y) >> SHIFT
 *   --seconds N           stop after N seconds (wall clock)   --max-cells N (default 3000)
 *   --routes K            at the end, DIR/cover_NN.polls: the K routes (power-on, per poll,
 *                         PS_POLL_REPLAY format) that together reach the most new code,
 *                         DIR/cover.txt their gains, DIR/found.bin the new code (bitmap)
 * ADDRs are 68K RAM (hex). The --press script reaches the start (frame F): menus, passwords;
 * or --movie FILE (an earlier route.txt) plays up to F (default: its end).
 * A score term ADDR:SIZE:WEIGHT:BASE counts (BASE - value) * WEIGHT while the value is not 0:
 * a boss's hit points (damage dealt), 0 while the boss is not loaded.
 */

#include <genrecomp/genrecomp.h>
#include <genrecomp/platform.h>
#include <genrecomp/input.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
__declspec(dllimport) void __stdcall Sleep(unsigned long ms);   /* kernel32; no windows.h clashes */
#define sleep(s) Sleep((s) * 1000ul)
#define strtok_r strtok_s                                             /* same signature in MSVC */
#else
#include <unistd.h>
#endif

#include "shared.h"
#include "../../src/ps_png.h"

void io_set_pad_state(int port, uint16_t buttons);
unsigned long zs_bound(unsigned long n);
unsigned long zs_pack(void *dst, unsigned long cap, const void *src, unsigned long n);
unsigned long zs_unpack(void *dst, unsigned long cap, const void *src, unsigned long n);
extern unsigned char work_ram[0x10000];

typedef struct { unsigned addr, size; int arg, base; } var_t;   /* arg: shift (cells) or weight (score); base: see score_now */

static unsigned ram(unsigned addr, unsigned size) {
    unsigned a = addr & 0xFFFF;
    unsigned v = work_ram[a ^ 1];                       /* GPGX keeps 68K RAM word-swapped */
    if (size >= 2) v = v << 8 | work_ram[(a + 1) ^ 1];
    if (size == 4) v = v << 16 | (unsigned)work_ram[(a + 2) ^ 1] << 8 | work_ram[(a + 3) ^ 1];
    return v;
}

static var_t s_cell[8], s_score[8], s_lives, s_health;
static int s_ncell, s_nscore, s_allow_damage;

static int parse_vars(const char *s, var_t *v, int max) {
    int n = 0;
    while (s && *s && n < max) {
        unsigned a, sz; int arg = 0, base = 0;
        if (sscanf(s, "%x:%u:%d:%d", &a, &sz, &arg, &base) >= 2) { v[n].addr = a; v[n].size = sz; v[n].arg = arg; v[n].base = base; n++; }
        s = strchr(s, ',');
        if (s) s++;
    }
    return n;
}

static int s_cam = -1;    /* --camera SHIFT: the VDP's plane-A scroll (camera X, Y) >> SHIFT is a cell variable */
static uint64_t cell_key(void) {
    uint64_t h = 1469598103934665603ull;
    if (s_cam >= 0) {
        unsigned hs = *(uint16_t *)(vram + (((reg[13] & 0x3F) << 10) & 0xFFFE));
        unsigned vs = *(uint16_t *)(vsram);
        h = (h ^ ((0x10000 - hs) & 0x3FF) >> s_cam) * 1099511628211ull;
        h = (h ^ (vs & 0x3FF) >> s_cam) * 1099511628211ull;
    }
    for (int i = 0; i < s_ncell; i++) {
        unsigned v = ram(s_cell[i].addr, s_cell[i].size);
        v = s_cell[i].arg >= 0 ? v >> s_cell[i].arg : v;
        h = (h ^ v) * 1099511628211ull;
    }
    return h;
}

static long score_now(void) {
    long s = 0;
    for (int i = 0; i < s_nscore; i++) {
        unsigned v = ram(s_score[i].addr, s_score[i].size);
        if (s_score[i].size == 2) v = (unsigned)(int16_t)v;
        if (s_score[i].base) s += v ? (long)(s_score[i].base - (int)v) * s_score[i].arg : 0;   /* e.g. boss HP: damage dealt */
        else s += (long)(int)v * s_score[i].arg;
    }
    return s;
}

typedef struct {
    uint64_t key;
    int parent;              /* -1: the start */
    int n;                   /* frames from the parent's state */
    uint16_t *in;
    uint8_t *st; unsigned long stlen;
    long score, depth;       /* depth: frames from the start */
    int tried, found;
    int rank;                /* checkpoint of a person's play it descends from (--anchors), 0: none */
    long novel;              /* --novelty: instructions run for the first time on the way here */
    unsigned vals[8];
    uint32_t *pl; long pln;  /* coverage mode: the polls from the parent's state (P1 << 16 | P2) */
    unsigned th;             /* port A TH as last driven, at this state */
    long gain, fbits;        /* new instructions reached on arrival here / by bursts from here */
} cell_t;

/* ---- coverage mode (--cover-base) ---- */
static int s_cover;
static uint8_t *s_known;                 /* bit per even ROM address: ran in a recording or here */
static uint8_t *s_found;                 /* found here */
static long s_newbits;
static unsigned s_romsz;
static uint32_t *s_pl; static long s_pln, s_plcap, s_plmark;   /* polls since the last restore */
static unsigned s_pth = 0x40;
static uint32_t *s_rp; static long s_rpn, s_rpi; static uint32_t s_rcur;   /* --polls replay */
static long s_max_cells = 3000;
#ifdef HOOK_CPU
static void cov_hook(hook_type_t type, int width, unsigned int address, unsigned int value) {
    (void)width; (void)value;
    if (type != HOOK_M68K_E) return;
    unsigned pc = address & 0xFFFFFF;
    if (pc >= s_romsz) return;
    uint8_t m = (uint8_t)(1u << ((pc >> 1) & 7));
    if (!(s_known[pc >> 4] & m)) { s_known[pc >> 4] |= m; s_found[pc >> 4] |= m; s_newbits++; }
}
#endif
static void pl_push(uint32_t v) {
    if (s_pln == s_plcap) s_pl = realloc(s_pl, (size_t)(s_plcap = s_plcap ? s_plcap * 2 : 1 << 16) * sizeof *s_pl);
    s_pl[s_pln++] = v;
}
/* the poll events of engine/src/ps_poll.c (TH 0->1 on port A): record what the game read, or
 * feed a recording the way PS_POLL_REPLAY does, so a route replays exactly in sidebyside.py */
static void cov_pad_hook(int port, unsigned int data, unsigned int ctrl) {
    if (s_rp) { input.pad[0] = (uint16)(s_rcur >> 16); input.pad[4] = (uint16)s_rcur; }
    if (port != 0) return;
    unsigned th = (ctrl & 0x40) ? (data & 0x40) : 0x40;
    if (!th && s_pth && s_rp && s_rpn) {
        long n = s_rpi < s_rpn ? s_rpi : s_rpn - 1;
        s_rcur = s_rp[n]; input.pad[0] = (uint16)(s_rcur >> 16); input.pad[4] = (uint16)s_rcur;
    }
    if (th && !s_pth) {
        if (s_rp) { s_rcur = s_rpi < s_rpn ? s_rp[s_rpi] : 0; s_rpi++; input.pad[0] = (uint16)(s_rcur >> 16); input.pad[4] = (uint16)s_rcur; }
        pl_push((uint32_t)input.pad[0] << 16 | input.pad[4]);
    }
    s_pth = th;
}

static cell_t *s_cells;
static long s_anchor_raw[1 << 14];   /* score_now() at each checkpoint */
static int s_ranked;                 /* --anchors: progress is measured along the person's play */
static int s_ncells, s_cap;
static uint8_t *s_tmp;        /* raw state */
static uint8_t *s_ztmp;
static long s_frame;          /* emulated frame number (for --press during the prefix) */

static int16_t s_audio[4096];
static int s_skip_render;     /* --fast: no picture during bursts (screenshots render their frame) */
static void step(uint16_t pad) {
    io_set_pad_state(0, pad);
    io_set_pad_state(1, 0);
    system_frame_gen(s_skip_render);
    audio_update(s_audio);
    s_frame++;
}

static int *s_hash; static unsigned s_hcap;            /* open addressing: cell index + 1 */
static void hash_insert(uint64_t key, int idx) {
    unsigned h = (unsigned)(key ^ key >> 32) & (s_hcap - 1);
    while (s_hash[h]) h = (h + 1) & (s_hcap - 1);
    s_hash[h] = idx + 1;
}
static void hash_rebuild(void) {                       /* all cells, at most half full */
    free(s_hash);
    for (s_hcap = 1 << 16; s_hcap < (unsigned)s_ncells * 4; s_hcap *= 2) ;
    s_hash = calloc(s_hcap, sizeof *s_hash);
    for (int i = 0; i < s_ncells; i++) hash_insert(s_cells[i].key, i);
}
static int find_cell(uint64_t key) {
    if (!s_hash) return -1;
    for (unsigned h = (unsigned)(key ^ key >> 32) & (s_hcap - 1); s_hash[h]; h = (h + 1) & (s_hcap - 1))
        if (s_cells[s_hash[h] - 1].key == key) return s_hash[h] - 1;
    return -1;
}

/* --novelty: every executed 68000 address (2 bytes per bit), the count, the level's last find */
static int s_novelty; static long s_saturate = 3000;
static uint8_t s_seen[0x400000 / 16]; static long s_seen_n;
#define NLEVELS 4096
static long s_level_last[NLEVELS];      /* iteration of the level's last new code (+1; 0: never) */
static long s_iter;
#ifdef HOOK_CPU
#include "cpuhook.h"
static void novelty_hook(hook_type_t type, int width, unsigned int address, unsigned int value) {
    (void)width; (void)value;
    if (type != HOOK_M68K_E || address >= 0x400000) return;
    unsigned i = address >> 1;
    if (!(s_seen[i >> 3] & (1u << (i & 7)))) { s_seen[i >> 3] |= (uint8_t)(1u << (i & 7)); s_seen_n++; }
}
#endif
static unsigned level_of(unsigned v0) { return v0 % NLEVELS; }

static int add_cell(uint64_t key, int parent, const uint16_t *in, int n) {
    if (s_ncells == s_cap) s_cells = realloc(s_cells, (size_t)(s_cap = s_cap ? s_cap * 2 : 1024) * sizeof *s_cells);
    cell_t *c = &s_cells[s_ncells];
    memset(c, 0, sizeof *c);
    c->key = key; c->parent = parent; c->n = n;
    c->in = malloc((size_t)(n ? n : 1) * sizeof *in);
    if (n) memcpy(c->in, in, (size_t)n * sizeof *in);
    int len = state_save(s_tmp);
    c->stlen = zs_pack(s_ztmp, zs_bound(STATE_SIZE), s_tmp, (unsigned long)len);
    c->st = malloc(c->stlen);
    memcpy(c->st, s_ztmp, c->stlen);
    c->rank = parent >= 0 ? s_cells[parent].rank : 0;
    c->score = s_ranked ? (long)c->rank * 10000000 + score_now() - s_anchor_raw[c->rank] : score_now();
    c->depth = (parent >= 0 ? s_cells[parent].depth : 0) + n;
    if (s_cover) {
        c->pln = s_pln - s_plmark;
        c->pl = malloc((size_t)(c->pln ? c->pln : 1) * sizeof *c->pl);
        if (c->pln) memcpy(c->pl, s_pl + s_plmark, (size_t)c->pln * sizeof *c->pl);
        s_plmark = s_pln;
        c->th = s_pth;
    }
    for (int i = 0; i < s_ncell; i++) c->vals[i] = ram(s_cell[i].addr, s_cell[i].size);
    s_ncells++;
    if (!s_hash || (unsigned)s_ncells * 2 >= s_hcap) hash_rebuild();
    else hash_insert(key, s_ncells - 1);
    return s_ncells - 1;
}

static void restore(int i) {
    s_pln = s_plmark = 0; s_pth = s_cells[i].th;
    zs_unpack(s_tmp, STATE_SIZE, s_cells[i].st, s_cells[i].stlen);
    state_load(s_tmp);
}

static int best_cell(void) {
    int b = 0;
    for (int i = 1; i < s_ncells; i++)
        if (s_cells[i].score > s_cells[b].score || (s_cells[i].score == s_cells[b].score && s_cells[i].depth < s_cells[b].depth)) b = i;
    return b;
}

static char s_dir[1024];
static uint16_t *s_prefix; static long s_prefix_n;

static void write_route(int c) {
    char p[1100]; snprintf(p, sizeof p, "%s/route.txt", s_dir);
    FILE *f = fopen(p, "w");
    if (!f) return;
    fprintf(f, "# explore route to cell %d (score %ld)\n", c, s_cells[c].score);
    for (long i = 0; i < s_prefix_n; i++) fprintf(f, "%04X 0000\n", s_prefix[i]);
    int chain[65536], n = 0;
    for (int i = c; i >= 0 && n < 65536; i = s_cells[i].parent) chain[n++] = i;
    while (n--) for (int k = 0; k < s_cells[chain[n]].n; k++) fprintf(f, "%04X 0000\n", s_cells[chain[n]].in[k]);
    fclose(f);
}

static void event(const char *fmt, ...) {
    char p[1100]; snprintf(p, sizeof p, "%s/events.log", s_dir);
    FILE *f = fopen(p, "a");
    if (!f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static void screenshot(const char *path) {
    static uint8_t px[320 * 240 * 4];
    int w = bitmap.viewport.w < 320 ? bitmap.viewport.w : 320, h = bitmap.viewport.h < 240 ? bitmap.viewport.h : 240;
    for (int y = 0; y < h; y++)
        memcpy(px + y * 320 * 4, bitmap.data + (y + bitmap.viewport.y) * bitmap.pitch + bitmap.viewport.x * 4, (size_t)w * 4);
    ps_png_write(path, px, 320, h);
}

/* "RIGHT:30 C+RIGHT:12 B:6" -> frames */
static int parse_inputs(const char *s, uint16_t *out, int max) {
    static const struct { const char *n; uint16_t b; } names[] = {
        {"UP", GEN_BTN_UP}, {"DOWN", GEN_BTN_DOWN}, {"LEFT", GEN_BTN_LEFT}, {"RIGHT", GEN_BTN_RIGHT},
        {"A", GEN_BTN_A}, {"B", GEN_BTN_B}, {"C", GEN_BTN_C}, {"START", GEN_BTN_START}, {"NONE", 0} };
    int n = 0;
    char *buf = strdup(s);
    for (char *tok = strtok(buf, " \t\n"); tok; tok = strtok(NULL, " \t\n")) {
        char *colon = strchr(tok, ':');
        int len = colon ? atoi(colon + 1) : 1;
        if (colon) *colon = 0;
        uint16_t b = 0;
        for (char *part = strtok_r(tok, "+", &colon); part; part = strtok_r(NULL, "+", &colon))
            for (size_t k = 0; k < sizeof names / sizeof names[0]; k++) if (!strcmp(part, names[k].n)) b |= names[k].b;
        for (int k = 0; k < len && n < max; k++) out[n++] = b;
    }
    free(buf);
    return n;
}

static uint16_t s_btn[4]; static int s_nbtn;

/* a random burst: held directions with jumps/attacks, like a player trying things */
static int random_burst(uint16_t *out, int max) {
    int n = 0, total = 40 + rand() % 140;
    while (n < total && n < max) {
        static const uint16_t dirs[] = { GEN_BTN_RIGHT, GEN_BTN_RIGHT, GEN_BTN_LEFT, 0, GEN_BTN_UP, GEN_BTN_DOWN,
                                         GEN_BTN_RIGHT, GEN_BTN_LEFT };
        uint16_t d = dirs[rand() % 8];
        int len = 4 + rand() % 40;
        int press = s_nbtn && rand() % 3 ? rand() % len : -1, plen = 1 + rand() % 24;
        uint16_t b = s_nbtn ? s_btn[rand() % s_nbtn] : 0;
        for (int k = 0; k < len && n < max; k++) out[n++] = d | (press >= 0 && k >= press && k < press + plen ? b : 0);
    }
    return n;
}

/* action prior: Markov chain over held combinations, with their hold lengths */
#define PRI_MAX 128
static uint16_t s_pc[PRI_MAX]; static int s_npc;               /* combinations */
static long s_ptr[PRI_MAX][PRI_MAX], s_prow[PRI_MAX];           /* transition counts */
static struct { int len; long n; } *s_pdur[PRI_MAX]; static int s_npdur[PRI_MAX]; static long s_pdtot[PRI_MAX];
static int s_prior_mix = 50;

static int pri_index(uint16_t c) {
    for (int i = 0; i < s_npc; i++) if (s_pc[i] == c) return i;
    if (s_npc == PRI_MAX) return -1;
    s_pc[s_npc] = c;
    return s_npc++;
}

static int load_prior(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        unsigned a, b; long n; int d;
        if (sscanf(line, "T %x %x %ld", &a, &b, &n) == 3) {
            int i = pri_index((uint16_t)a), j = pri_index((uint16_t)b);
            if (i >= 0 && j >= 0) { s_ptr[i][j] += n; s_prow[i] += n; }
        } else if (sscanf(line, "D %x %d %ld", &a, &d, &n) == 3) {
            int i = pri_index((uint16_t)a);
            if (i < 0) continue;
            s_pdur[i] = realloc(s_pdur[i], (size_t)(s_npdur[i] + 1) * sizeof *s_pdur[i]);
            s_pdur[i][s_npdur[i]].len = d; s_pdur[i][s_npdur[i]].n = n; s_npdur[i]++;
            s_pdtot[i] += n;
        }
    }
    fclose(f);
    return s_npc;
}

static long rnd(long n) { return n > 0 ? (long)(((unsigned long)rand() << 16 ^ (unsigned long)rand()) % (unsigned long)n) : 0; }

static int prior_burst(uint16_t *out, int max) {
    int n = 0, total = 40 + rand() % 140, cur = rand() % s_npc;
    while (n < total && n < max) {
        if (s_prow[cur]) {                      /* next combination */
            long r = rnd(s_prow[cur]);
            for (int j = 0; j < s_npc; j++) if ((r -= s_ptr[cur][j]) < 0) { cur = j; break; }
        } else cur = rand() % s_npc;
        int len = 1;
        if (s_pdtot[cur]) {                     /* how long it is held */
            long r = rnd(s_pdtot[cur]);
            for (int k = 0; k < s_npdur[cur]; k++) if ((r -= s_pdur[cur][k].n) < 0) { len = s_pdur[cur][k].len; break; }
        }
        for (int k = 0; k < len && n < max; k++) out[n++] = s_pc[cur];
    }
    return n;
}

/* forced policy (hint): templates of held combinations */
#define POL_MAX 16
static struct { uint16_t b[64]; int len[64]; int n; } s_pol[POL_MAX];
static int s_npol, s_pol_pct;

static void set_policy(const char *spec) {
    s_npol = 0;
    s_pol_pct = atoi(spec);
    const char *q = strchr(spec, ' ');
    static uint16_t buf[1 << 17];
    while (q && s_npol < POL_MAX) {
        const char *bar = strchr(q, '|');
        char t[1024]; size_t L = bar ? (size_t)(bar - q) : strlen(q);
        if (L >= sizeof t) L = sizeof t - 1;
        memcpy(t, q, L); t[L] = 0;
        int n = parse_inputs(t, buf, 1 << 17);
        if (n) {                                  /* back to runs */
            int r = 0;
            for (int k = 0; k < n && r < 64; k++) {
                if (k && buf[k] == buf[k - 1]) { s_pol[s_npol].len[r - 1]++; continue; }
                s_pol[s_npol].b[r] = buf[k]; s_pol[s_npol].len[r] = 1; r++;
            }
            s_pol[s_npol].n = r;
            s_npol++;
        }
        q = bar ? bar + 1 : NULL;
    }
}

static int policy_burst(uint16_t *out, int max) {
    int n = 0, reps = 1 + rand() % 3;
    for (int w = rand() % 31; w > 0 && n < max; w--) out[n++] = 0;
    while (reps-- > 0) {
        int t = rand() % s_npol;
        for (int r = 0; r < s_pol[t].n; r++) {
            int len = s_pol[t].len[r] * (70 + rand() % 61) / 100;
            if (len < 1) len = 1;
            for (int k = 0; k < len && n < max; k++) out[n++] = s_pol[t].b[r];
        }
    }
    return n;
}

/* drop the archive's newest cells (from index `keep` on) */
static void drop_cells(int keep) {
    if (keep >= s_ncells) return;
    for (int i = keep; i < s_ncells; i++) { free(s_cells[i].in); free(s_cells[i].st); free(s_cells[i].pl); }
    s_ncells = keep;
    hash_rebuild();
}

/* play inputs from cell `from`; new cells join the archive. Stops on damage (unless
 * allowed) or a lost life; then, and after 60 idle frames of look-ahead, cells reached in
 * the 120 frames before a lost life are dropped: they are a fall already under way. */
static long s_seen_bits;
static int play(int from, const uint16_t *in, int n, int force_add) {
    restore(from);
    s_seen_bits = s_newbits;
    unsigned lives0 = s_lives.size ? ram(s_lives.addr, s_lives.size) : 0;
    unsigned hp0 = s_health.size ? ram(s_health.addr, s_health.size) : 0;
    int first = s_ncells, parent = from, start = 0, k, lost_at = -1;
    long seen0 = s_seen_n, seen_cell = s_seen_n;
    static int at[1 << 17];                     /* frame each new cell was reached at */
    for (k = 0; k < n; k++) {
        step(in[k]);
        if (s_lives.size && ram(s_lives.addr, s_lives.size) < lives0) { lost_at = k; break; }
        if (!s_allow_damage && s_health.size && ram(s_health.addr, s_health.size) < hp0) break;
        int keyed = s_ncell || s_cam >= 0;
        uint64_t key = keyed ? cell_key() : 0;
        long gain = s_newbits - s_seen_bits;
        if (gain || (keyed && find_cell(key) < 0 && (!s_cover || s_ncells < s_max_cells)) || (force_add && k == n - 1)) {
            at[s_ncells - first] = k;
            if (gain && !keyed) key = 0x8000000000000000ull | (uint64_t)s_ncells;
            parent = add_cell(key, parent, in + start, k + 1 - start);
            s_cells[parent].novel = s_seen_n - seen_cell; seen_cell = s_seen_n;
            s_cells[parent].gain = gain; s_seen_bits = s_newbits;
            start = k + 1;
        }
    }
    if (s_novelty && lost_at < 0 && s_seen_n > seen_cell && k == n) {   /* new code, no new place: keep it */
        at[s_ncells - first] = n - 1;
        parent = add_cell(cell_key() ^ (0x9E3779B97F4A7C15ull * (uint64_t)s_seen_n), parent, in + start, n - start);
        s_cells[parent].novel = s_seen_n - seen_cell;
    }
    if (s_novelty && s_seen_n > seen0) s_level_last[level_of(s_cells[from].vals[0])] = s_iter + 1;
    if (lost_at < 0 && s_lives.size && s_ncells > first)
        for (int j = 0; j < 60; j++) {
            step(0);
            if (ram(s_lives.addr, s_lives.size) < lives0) { lost_at = k + j; break; }
        }
    if (lost_at >= 0) {
        int keep = first;
        while (keep < s_ncells && at[keep - first] < lost_at - 120) keep++;
        drop_cells(keep);
    }
    return s_ncells - first;
}

/* the n best cells by score, best first */
static int top_cells(int *out, int n) {
    int k = 0;
    for (int i = 0; i < s_ncells; i++) {
        int j = k < n ? k++ : n - 1;
        if (j == n - 1 && k == n && i >= n && s_cells[i].score <= s_cells[out[n - 1]].score) continue;
        out[j] = i;
        for (; j > 0 && s_cells[out[j]].score > s_cells[out[j - 1]].score; j--) { int t = out[j]; out[j] = out[j - 1]; out[j - 1] = t; }
    }
    return k;
}

static int pick(void) {
    if (s_novelty && rand() % 4 == 0) {      /* a quarter of the picks: cells that found new code, in levels still yielding it (reaching the next level stays the main goal) */
        double tot = 0;
        for (int i = 0; i < s_ncells; i++) {
            long last = s_level_last[level_of(s_cells[i].vals[0])];
            if (s_cells[i].novel > 0 && last && s_iter - (last - 1) < s_saturate) tot += (double)s_cells[i].novel / (1.0 + s_cells[i].tried);
        }
        if (tot > 0) {
            double r = tot * rand() / RAND_MAX;
            for (int i = 0; i < s_ncells; i++) {
                long last = s_level_last[level_of(s_cells[i].vals[0])];
                if (s_cells[i].novel > 0 && last && s_iter - (last - 1) < s_saturate && (r -= (double)s_cells[i].novel / (1.0 + s_cells[i].tried)) <= 0) return i;
            }
        }
    }
    if (s_cover) {   /* in proportion to the new code found on arrival and from there since */
        double tot = 0;
        for (int i = 0; i < s_ncells; i++) tot += (1.0 + s_cells[i].gain + 2.0 * s_cells[i].fbits) / (1.0 + s_cells[i].tried);
        double r = tot * rand() / RAND_MAX;
        for (int i = 0; i < s_ncells; i++) if ((r -= (1.0 + s_cells[i].gain + 2.0 * s_cells[i].fbits) / (1.0 + s_cells[i].tried)) <= 0) return i;
        return s_ncells - 1;
    }
    /* half the time the best few cells (push the frontier), else rarely tried ones */
    if (rand() % 2) {
        int top[8], nt = top_cells(top, 8);
        return top[rand() % nt];
    }
    double tot = 0;
    for (int i = 0; i < s_ncells; i++) tot += 1.0 / (1.0 + s_cells[i].tried);
    double r = tot * rand() / RAND_MAX;
    for (int i = 0; i < s_ncells; i++) if ((r -= 1.0 / (1.0 + s_cells[i].tried)) <= 0) return i;
    return s_ncells - 1;
}

/* the K routes reaching the most new code (greedy: each next one counts only cells not yet
 * in an earlier one), DIR/cover_NN.polls, + DIR/cover.txt, DIR/found.bin */
static void write_cover_routes(int k) {
    char p[1100];
    uint8_t *used = calloc((size_t)s_ncells, 1);
    snprintf(p, sizeof p, "%s/cover.txt", s_dir);
    FILE *sum = fopen(p, "w");
    for (int r = 0; r < k; r++) {
        long bv = 0; int bc = -1;
        for (int i = 0; i < s_ncells; i++) {
            if (!s_cells[i].gain || used[i]) continue;
            long v = 0;
            for (int j = i; j >= 0; j = s_cells[j].parent) if (!used[j]) v += s_cells[j].gain;
            if (v > bv) { bv = v; bc = i; }
        }
        if (bc < 0) break;
        int chain[1 << 16], n = 0;
        for (int j = bc; j >= 0 && n < (1 << 16); j = s_cells[j].parent) { chain[n++] = j; used[j] = 1; }
        snprintf(p, sizeof p, "%s/cover_%02d.polls", s_dir, r);
        FILE *f = fopen(p, "w");
        long polls = 0;
        while (f && n--) for (long q = 0; q < s_cells[chain[n]].pln; q++, polls++)
            fprintf(f, "%04X %04X\n", s_cells[chain[n]].pl[q] >> 16, s_cells[chain[n]].pl[q] & 0xFFFF);
        if (f) fclose(f);
        if (sum) fprintf(sum, "cover_%02d.polls gain %ld polls %ld frames %ld\n", r, bv, polls, s_cells[bc].depth);
    }
    if (sum) fclose(sum);
    free(used);
    snprintf(p, sizeof p, "%s/found.bin", s_dir);
    FILE *f = fopen(p, "wb");
    if (f) { fwrite(s_found, 1, 0x400000 / 16, f); fclose(f); }
}

int main(int argc, char *argv[]) {
    const char *rom = NULL;
    long start = 0, stuck_after = 3000, max_iters = 0, plateau = 0;
    const char *movie = NULL;
    unsigned patch_a[8], patch_v[8]; int npatch = 0;
    const char *try_state = NULL, *try_inputs = "", *shots = NULL, *save_state = NULL;
    const char *start_state = NULL, *prefix_route = NULL, *anchors = NULL;
    long anchor_every = 120;
    unsigned seed = 1;
    const char *cover_base = NULL, *polls_file = NULL; long seconds = 0; int routes = 8;
    char *pargv[512]; int pargc = 0;
    pargv[pargc++] = argv[0]; pargv[pargc++] = "--headless";
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : "";
        if (!strcmp(a, "--dir")) { snprintf(s_dir, sizeof s_dir, "%s", v); i++; }
        else if (!strcmp(a, "--start")) { start = atol(v); i++; }
        else if (!strcmp(a, "--press") && pargc < 500) { pargv[pargc++] = argv[i]; pargv[pargc++] = argv[++i]; }
        else if (!strcmp(a, "--cell")) { s_ncell += parse_vars(v, s_cell + s_ncell, 8 - s_ncell); i++; }
        else if (!strcmp(a, "--score")) { s_nscore = parse_vars(v, s_score, 8); i++; }
        else if (!strcmp(a, "--lives")) { parse_vars(v, &s_lives, 1); i++; }
        else if (!strcmp(a, "--health")) { parse_vars(v, &s_health, 1); i++; }
        else if (!strcmp(a, "--stuck")) { stuck_after = atol(v); i++; }
        else if (!strcmp(a, "--iters")) { max_iters = atol(v); i++; }
        else if (!strcmp(a, "--plateau")) { plateau = atol(v); i++; }
        else if (!strcmp(a, "--novelty")) {
#ifdef HOOK_CPU
            s_novelty = 1; set_cpu_hook(novelty_hook);
#else
            fprintf(stderr, "--novelty needs the tracer build (-DGENRECOMP_TRACE=ON)\n"); return 2;
#endif
        }
        else if (!strcmp(a, "--saturate")) { s_saturate = atol(v); i++; }
        else if (!strcmp(a, "--policy")) { set_policy(v); i++; }
        else if (!strcmp(a, "--try")) { try_state = v; i++; }
        else if (!strcmp(a, "--save-state")) { save_state = v; i++; }
        else if (!strcmp(a, "--state")) { start_state = v; i++; }
        else if (!strcmp(a, "--prefix-route")) { prefix_route = v; i++; }
        else if (!strcmp(a, "--anchors")) { anchors = v; i++; }
        else if (!strcmp(a, "--anchor-every")) { anchor_every = atol(v); i++; }
        else if (!strcmp(a, "--inputs")) { try_inputs = v; i++; }
        else if (!strcmp(a, "--shots")) { shots = v; i++; }
        else if (!strcmp(a, "--movie")) { movie = v; i++; }
        else if (!strcmp(a, "--damage")) s_allow_damage = 1;
        else if (!strcmp(a, "--fast")) s_skip_render = 1;
        else if (!strcmp(a, "--patch") && npatch < 8) {      /* ROM word, e.g. a cheat: F0EA=600A */
            if (sscanf(v, "%x=%x", &patch_a[npatch], &patch_v[npatch]) == 2) npatch++;
            i++;
        }
        else if (!strcmp(a, "--cover-base")) { cover_base = v; s_cover = 1; i++; }
        else if (!strcmp(a, "--polls")) { polls_file = v; i++; }
        else if (!strcmp(a, "--seconds")) { seconds = atol(v); i++; }
        else if (!strcmp(a, "--max-cells")) { s_max_cells = atol(v); i++; }
        else if (!strcmp(a, "--camera")) { s_cam = atoi(v); i++; }
        else if (!strcmp(a, "--routes")) { routes = atoi(v); i++; }
        else if (!strcmp(a, "--seed")) { seed = (unsigned)atol(v); i++; }
        else if (!strcmp(a, "--prior")) { if (!load_prior(v)) { fprintf(stderr, "explore: no prior in %s\n", v); return 1; } i++; }
        else if (!strcmp(a, "--prior-mix")) { s_prior_mix = atoi(v); i++; }
        else if (!strcmp(a, "--buttons")) {
            for (const char *p = v; *p && s_nbtn < 4; p++)
                if (*p == 'A') s_btn[s_nbtn++] = GEN_BTN_A; else if (*p == 'B') s_btn[s_nbtn++] = GEN_BTN_B;
                else if (*p == 'C') s_btn[s_nbtn++] = GEN_BTN_C;
            i++;
        }
        else rom = a;
    }
    if (!rom || (!try_state && !save_state && (!*s_dir || (!s_ncell && !s_cover)))) { fprintf(stderr, "usage: see explore.c\n"); return 1; }
    if (!s_nbtn) { s_btn[0] = GEN_BTN_C; s_btn[1] = GEN_BTN_B; s_nbtn = 2; }
    srand(seed);
    pargv[pargc] = NULL;
    platform_parse_args(pargc, pargv);
    if (!genrecomp_init("explore", 3)) return 1;
    char path[MAXPATHLEN];
    snprintf(path, sizeof path, "%s", rom);
    if (!load_rom(path)) { fprintf(stderr, "explore: cannot load %s\n", rom); return 1; }
    audio_init(44100, 0);
    input.system[0] = input.system[1] = SYSTEM_GAMEPAD;
    system_init();
    system_reset();
    s_tmp = malloc(STATE_SIZE);
    if (s_cover && s_novelty) { fprintf(stderr, "--novelty and --cover-base are separate modes (one CPU hook): pick one\n"); return 2; }
    if (s_cover) {
#ifdef HOOK_CPU
        s_romsz = cart.romsize < 0x400000 ? cart.romsize : 0x400000;
        s_known = calloc(1, 0x400000 / 16); s_found = calloc(1, 0x400000 / 16);
        FILE *bf = fopen(cover_base, "rb");
        if (bf) { size_t got = fread(s_known, 1, 0x400000 / 16, bf); (void)got; fclose(bf); }
        set_cpu_hook(cov_hook);
        io_pad_write_hook = cov_pad_hook;
        if (polls_file) {
            FILE *f = fopen(polls_file, "r");
            if (!f) { fprintf(stderr, "explore: cannot open %s\n", polls_file); return 1; }
            unsigned a, b; long cap = 1 << 16;
            s_rp = malloc((size_t)cap * sizeof *s_rp);
            while (fscanf(f, "%x %x", &a, &b) == 2) {
                if (s_rpn == cap) s_rp = realloc(s_rp, (size_t)(cap *= 2) * sizeof *s_rp);
                s_rp[s_rpn++] = (uint32_t)a << 16 | (b & 0xFFFF);
            }
            fclose(f);
        }
#else
        fprintf(stderr, "explore: --cover-base needs a build with -DGENRECOMP_TRACE=ON (HOOK_CPU)\n");
        return 1;
#endif
    }
    s_ztmp = malloc(zs_bound(STATE_SIZE));

    if (try_state) {   /* --try: from a stuck state, play inputs, screenshots + cell variables */
        for (int k = 0; k < npatch; k++) {
            cart.rom[patch_a[k] ^ 1] = (uint8)(patch_v[k] >> 8);
            cart.rom[(patch_a[k] + 1) ^ 1] = (uint8)patch_v[k];
        }
        FILE *sf = fopen(try_state, "rb");
        if (!sf) { fprintf(stderr, "explore: cannot open %s\n", try_state); return 1; }
        size_t len = fread(s_tmp, 1, STATE_SIZE, sf);
        fclose(sf);
        (void)len;
        state_load(s_tmp);
        static uint16_t in[1 << 17];
        int n = parse_inputs(try_inputs, in, 1 << 17);
        char dir[1024] = ""; long every = 10;
        if (shots) { snprintf(dir, sizeof dir, "%s", shots); char *c = strchr(dir, ':'); if (c) { *c = 0; every = atol(c + 1); } }
        for (int k = 0; k <= n; k++) {
            if (k == n || k % every == 0) {
                printf("f+%d pc %06X pad %04X", k, m68k_get_reg(M68K_REG_PC), k < n ? in[k] : 0);
                for (int i = 0; i < s_ncell; i++) printf(" %X", ram(s_cell[i].addr, s_cell[i].size));
                printf("\n");
                if (*dir) { char p[1100]; snprintf(p, sizeof p, "%s/t%05d.png", dir, k); screenshot(p); }
            }
            if (k < n) step(in[k]);
        }
        return 0;
    }

    /* prefix: the --press script up to the start frame (recorded for the route) */
    uint16_t *mv = NULL; long mvn = 0;
    if (movie) {                                /* a route from an earlier run: start where it ends */
        FILE *f = fopen(movie, "r");
        if (!f) { fprintf(stderr, "explore: cannot open %s\n", movie); return 1; }
        char line[64]; long cap = 1 << 16;
        mv = malloc((size_t)cap * sizeof *mv);
        while (fgets(line, sizeof line, f)) {
            unsigned p;
            if (line[0] == '#' || sscanf(line, "%x", &p) != 1) continue;
            if (mvn == cap) mv = realloc(mv, (size_t)(cap *= 2) * sizeof *mv);
            mv[mvn++] = (uint16_t)p;
        }
        fclose(f);
        if (!start || start > mvn) start = mvn;
    }
    if (start_state) {   /* --state: start from a saved state; --prefix-route: the inputs that led there */
        for (int k = 0; k < npatch; k++) {
            cart.rom[patch_a[k] ^ 1] = (uint8)(patch_v[k] >> 8);
            cart.rom[(patch_a[k] + 1) ^ 1] = (uint8)patch_v[k];
        }
        FILE *sf = fopen(start_state, "rb");
        if (!sf) { fprintf(stderr, "explore: cannot open %s\n", start_state); return 1; }
        if (!fread(s_tmp, 1, STATE_SIZE, sf)) { fclose(sf); return 1; }
        fclose(sf);
        state_load(s_tmp);
        long cap = 1 << 16; s_prefix = malloc((size_t)cap * sizeof *s_prefix);
        FILE *pf = prefix_route ? fopen(prefix_route, "r") : NULL;
        char l[64];
        while (pf && fgets(l, sizeof l, pf)) {
            unsigned v;
            if (l[0] == '#' || sscanf(l, "%x", &v) != 1) continue;
            if (s_prefix_n == cap) s_prefix = realloc(s_prefix, (size_t)(cap *= 2) * sizeof *s_prefix);
            s_prefix[s_prefix_n++] = (uint16_t)v;
        }
        if (pf) fclose(pf);
        s_frame = start = s_prefix_n;
    } else
        s_prefix = malloc((size_t)(start + 1) * sizeof *s_prefix);
    for (; s_frame < start; ) {
        platform_poll_events();
        recomp_input_update();
        uint16_t pad = mv ? mv[s_frame] : recomp_input_read_pad(0);
        if (s_frame == 300)                      /* after the ROM checksum test, like PS_ORACLE_PATCH */
            for (int k = 0; k < npatch; k++) {   /* GPGX keeps the ROM word-swapped */
                cart.rom[patch_a[k] ^ 1] = (uint8)(patch_v[k] >> 8);
                cart.rom[(patch_a[k] + 1) ^ 1] = (uint8)patch_v[k];
            }
        s_prefix[s_prefix_n++] = pad;
        step(pad);
        static uint8_t blank[GEN_RENDER_WIDTH * GEN_RENDER_HEIGHT * 4];
        platform_present_frame(blank);          /* advances the --press frame counter */
    }
    if (save_state) {   /* --save-state FILE: the state where --movie/--press end, then exit */
        int len = state_save(s_tmp);
        FILE *sf = fopen(save_state, "wb");
        if (!sf) return 1;
        fwrite(s_tmp, 1, (size_t)len, sf);
        fclose(sf);
        printf("state at frame %ld -> %s\n", s_frame, save_state);
        return 0;
    }
    if (s_rp) {   /* --polls: the recording from power-on, a cell every --anchor-every frames */
        add_cell(cell_key(), -1, NULL, 0);
        int parent = 0; long f0 = 0, added = 0;
        static uint16_t none[1 << 17];
        while (s_rpi < s_rpn) {
            step(0);
            if (s_frame - f0 >= anchor_every) { parent = add_cell(cell_key() ^ (uint64_t)s_frame << 40, parent, none, (int)(s_frame - f0)); f0 = s_frame; added++; }
        }
        s_rp = NULL;
        if (s_frame > f0) { parent = add_cell(cell_key() ^ (uint64_t)s_frame << 40, parent, none, (int)(s_frame - f0)); added++; }
        event("POLLS %ld polls of %s, %ld frames, %ld cells along it", s_rpn, polls_file, s_frame, added);
    } else
        add_cell(cell_key(), -1, NULL, 0);
    event("START frame %ld score %ld", start, s_cells[0].score);
    if (anchors) {   /* a person's play: a checkpoint cell every --anchor-every frames of it */
        FILE *af = fopen(anchors, "r");
        if (!af) { fprintf(stderr, "explore: cannot open %s\n", anchors); return 1; }
        static uint16_t seg[1 << 17]; char l[64]; int n = 0, parent = 0, added = 0;
        while (fgets(l, sizeof l, af)) {
            unsigned v;
            if (l[0] == '#' || sscanf(l, "%x", &v) != 1) continue;
            seg[n++] = (uint16_t)v;
            step((uint16_t)v);
            if (n == anchor_every || n == (1 << 17)) { parent = add_cell(cell_key(), parent, seg, n); n = 0; added++; s_anchor_raw[added] = score_now(); s_cells[parent].rank = added; }
        }
        fclose(af);
        if (n) { parent = add_cell(cell_key(), parent, seg, n); added++; s_anchor_raw[added] = score_now(); s_cells[parent].rank = added; }
        /* from here on a cell's progress is its checkpoint's rank, then its score gain over it */
        s_ranked = 1;
        for (int i = 0; i < s_ncells; i++) s_cells[i].score = (long)s_cells[i].rank * 10000000 + (i ? 1 : 0);
        event("ANCHORS %d checkpoints from %s, last score %ld", added, anchors, s_cells[parent].score);
    }

    time_t t0 = time(NULL);
    static uint16_t in[1 << 17];
    long it = 0, last_new = 0, stuck_n = 0, last_better = 0, best_score = s_cells[0].score;
    int best = 0;
    for (;;) {
        int c = pick();
        s_cells[c].tried++;
        int n = s_npol && rand() % 100 < s_pol_pct ? policy_burst(in, 1 << 17)
              : s_npc && rand() % 100 < s_prior_mix ? prior_burst(in, 1 << 17) : random_burst(in, 1 << 17);
        long seen_before = s_seen_n;
        s_iter = it;
        if (s_cover && rand() % 16 == 0) for (int k = 0; k < 3 && k < n; k++) in[k] = GEN_BTN_START;   /* menus, pause */
        long nb = s_newbits;
        int added = play(c, in, n, 0);
        s_cells[c].found += added;
        s_cells[c].fbits += s_newbits - nb;
        it++;
        if (added || s_seen_n > seen_before) last_new = it;
        int b = best_cell();
        if (b != best && s_cells[b].score > s_cells[best].score) {
            best = b;
            write_route(best);
            char vals[256] = ""; for (int i = 0; i < s_ncell; i++) snprintf(vals + strlen(vals), 32, " %X", s_cells[best].vals[i]);
            if (s_cells[best].vals[0] != s_cells[0].vals[0]) event("PROGRESS it %ld cells %d score %ld cell%s", it, s_ncells, s_cells[best].score, vals);
        }
        if (it % 500 == 0) {
            char p[1100]; snprintf(p, sizeof p, "%s/status.log", s_dir);
            FILE *f = fopen(p, "a");
            if (f) { fprintf(f, "it %ld cells %d best %d score %ld depth %ld code %ld\n", it, s_ncells, best, s_cells[best].score, s_cells[best].depth, s_seen_n); fclose(f); }
        }
        if (s_cover && ((seconds && time(NULL) - t0 >= seconds) || (max_iters && it >= max_iters))) {
            write_cover_routes(routes);
            event("END it %ld cells %d new instructions %ld", it, s_ncells, s_newbits);
            return 0;
        }
        if (s_cover) continue;   /* no hints: the loop script restarts from elsewhere */
        if (max_iters && it >= max_iters) { write_route(best); event("END it %ld cells %d best %d score %ld", it, s_ncells, best, s_cells[best].score); return 0; }
        if (s_cells[best].score > best_score) { best_score = s_cells[best].score; last_better = it; }
        if (it - last_new < stuck_after && (!plateau || it - last_better < plateau)) continue;

        /* stuck: show the best cell and wait for a hint */
        stuck_n++;
        restore(best);
        { int sk = s_skip_render; s_skip_render = 0; step(0); s_skip_render = sk; }
        char p[1100];
        snprintf(p, sizeof p, "%s/stuck_%ld.png", s_dir, stuck_n);
        screenshot(p);
        snprintf(p, sizeof p, "%s/stuck_%ld.txt", s_dir, stuck_n);
        FILE *f = fopen(p, "w");
        if (f) {
            fprintf(f, "iterations %ld, cells %d, best cell %d score %ld depth %ld frames\nbest cells (cell: vars):\n",
                    it, s_ncells, best, s_cells[best].score, s_cells[best].depth);
            int order[16], no = top_cells(order, 16);
            for (int k = 0; k < no; k++) {
                fprintf(f, "  %d: score %ld tried %d found %d vars", order[k], s_cells[order[k]].score, s_cells[order[k]].tried, s_cells[order[k]].found);
                for (int i = 0; i < s_ncell; i++) fprintf(f, " %X", s_cells[order[k]].vals[i]);
                fputc('\n', f);
            }
            fclose(f);
        }
        write_route(best);
        {   /* the best cell's state and route, for --try and for playing from it by hand */
            restore(best);
            int len = state_save(s_tmp);
            snprintf(p, sizeof p, "%s/stuck_%ld.state", s_dir, stuck_n);
            FILE *sf = fopen(p, "wb");
            if (sf) { fwrite(s_tmp, 1, (size_t)len, sf); fclose(sf); }
            char a[1100], b[1100];
            snprintf(a, sizeof a, "%s/route.txt", s_dir); snprintf(b, sizeof b, "%s/stuck_%ld.route", s_dir, stuck_n);
            FILE *in_f = fopen(a, "r"), *out_f = fopen(b, "w");
            if (in_f && out_f) { char l[64]; while (fgets(l, sizeof l, in_f)) fputs(l, out_f); }
            if (in_f) fclose(in_f);
            if (out_f) fclose(out_f);
        }
        event("STUCK %ld it %ld cells %d best %d score %ld", stuck_n, it, s_ncells, best, s_cells[best].score);
        char hp[1100]; snprintf(hp, sizeof hp, "%s/hint.txt", s_dir);
        for (;;) {
            FILE *h = fopen(hp, "r");
            if (!h) { sleep(2); continue; }
            char line[4096]; int done = 0;
            while (fgets(line, sizeof line, h)) {
                char *nl = strchr(line, '\n'); if (nl) *nl = 0;
                if (!strncmp(line, "macro ", 6)) {
                    char *q = line + 6, *sp = strchr(q, ' ');
                    if (!sp) continue;
                    *sp = 0;
                    int from = !strcmp(q, "best") ? best : atoi(q);
                    if (from < 0 || from >= s_ncells) continue;
                    int m = parse_inputs(sp + 1, in, 1 << 17);
                    int a = play(from, in, m, 1);
                    event("HINT macro from %d: %d frames, %d new cells, score now %ld", from, m, a, score_now());
                } else if (!strncmp(line, "play ", 5)) {     /* play CELL FILE: inputs recorded by hand */
                    char *q = line + 5, *sp = strchr(q, ' ');
                    if (!sp) continue;
                    *sp = 0;
                    int from = !strcmp(q, "best") ? best : atoi(q);
                    FILE *pf = fopen(sp + 1, "r");
                    if (!pf || from < 0 || from >= s_ncells) { if (pf) fclose(pf); continue; }
                    int m = 0; char l[64];
                    while (m < (1 << 17) && fgets(l, sizeof l, pf)) { unsigned v; if (l[0] != '#' && sscanf(l, "%x", &v) == 1) in[m++] = (uint16_t)v; }
                    fclose(pf);
                    int a = play(from, in, m, 1);
                    event("HINT play from %d: %d frames, %d new cells, score now %ld", from, m, a, score_now());
                } else if (!strncmp(line, "score ", 6)) {
                    s_nscore = parse_vars(line + 6, s_score, 8);
                    for (int i = 0; i < s_ncells; i++) {   /* rescore every cell from its state */
                        restore(i);
                        s_cells[i].score = s_ranked ? (long)s_cells[i].rank * 10000000 + score_now() - s_anchor_raw[s_cells[i].rank] : score_now();
                    }
                    best = best_cell();
                    event("HINT score: %d terms, best %d score %ld", s_nscore, best, s_cells[best].score);
                } else if (!strncmp(line, "cell ", 5) && s_ncell < 8) {
                    s_ncell += parse_vars(line + 5, s_cell + s_ncell, 1);
                    for (int i = 0; i < s_ncells; i++) {   /* re-key from each state; duplicates stay harmless */
                        restore(i);
                        s_cells[i].key = cell_key();
                        s_cells[i].vals[s_ncell - 1] = ram(s_cell[s_ncell - 1].addr, s_cell[s_ncell - 1].size);
                    }
                    hash_rebuild();
                    event("HINT cell: %d variables", s_ncell);
                } else if (!strncmp(line, "policy ", 7)) {
                    set_policy(line + 7);
                    event("HINT policy: %d%% of bursts from %d templates", s_pol_pct, s_npol);
                } else if (!strncmp(line, "damage ", 7)) {
                    s_allow_damage = !strcmp(line + 7, "on");
                    event("HINT damage %s", s_allow_damage ? "on" : "off");
                } else if (!strcmp(line, "done")) done = 1;
            }
            fclose(h);
            remove(hp);
            if (done) { write_route(best_cell()); event("DONE cells %d best %d", s_ncells, best_cell()); return 0; }
            break;
        }
        last_new = last_better = it;
        best_score = s_cells[best].score;
    }
}
