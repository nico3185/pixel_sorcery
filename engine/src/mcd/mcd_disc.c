/*
 * mcd_disc.c -- the player's Mega CD disc image for the native Mega CD: a cue sheet with its bin
 * file(s) (one per track or one for all; MODE1/2352 data, AUDIO tracks, INDEX 00 pregaps inside
 * the files). Gives the drive the table of contents and raw 2352-byte sectors by LBA (sector 0 =
 * track 1, index 1 = 00:02:00 absolute). The port's own code.
 *
 * PS_CD_AUDIO=other.cue: the audio tracks come from another release of the same disc (SHCD: the
 * US soundtrack on the Japanese/European disc, as the game-select screen offers). The disc keeps its
 * data track(s); each audio track N is the other disc's track N with that track's length, laid out
 * after the data as on a pressed disc (the table of contents the game reads says so).
 */
#include "mcd_disc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    McdDisc d;
    FILE *files[MCD_MAX_TRACKS + 1];
    char paths[MCD_MAX_TRACKS + 1][1024];
    int nfiles;
} Image;
static Image s_main, s_alt;
static McdDisc s_disc;                       /* what the drive sees */
static bool s_mixed;                         /* audio tracks from s_alt */

const McdDisc *mcd_disc(void) { return &s_disc; }

static long file_sectors(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f) / 2352;
    fclose(f);
    return n;
}

static void image_close(Image *im) {
    for (int i = 0; i < im->nfiles; i++) if (im->files[i]) { fclose(im->files[i]); im->files[i] = NULL; }
    im->nfiles = 0;
    memset(&im->d, 0, sizeof im->d);
}

static bool image_open(Image *im, const char *cue) {
    image_close(im);
    FILE *f = fopen(cue, "r");
    if (!f) return false;
    char dir[1024]; snprintf(dir, sizeof dir, "%s", cue);
    char *slash = strrchr(dir, '/'); if (slash) slash[1] = 0; else dir[0] = 0;
    char line[1200];
    int track = 0, file = -1;
    long base = 0;                                  /* LBA where the current file starts */
    long fsec = 0;                                  /* its length in sectors */
    McdDisc *d = &im->d;
    while (fgets(line, sizeof line, f)) {
        char *p = line; while (*p == ' ' || *p == '\t') p++;
        if (!strncmp(p, "FILE", 4)) {
            char *q1 = strchr(p, '"'), *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
            if (!q1 || !q2 || im->nfiles > MCD_MAX_TRACKS) continue;
            if (file >= 0) base += fsec;
            file = im->nfiles++;
            snprintf(im->paths[file], sizeof im->paths[file], "%s%.*s", dir, (int)(q2 - q1 - 1), q1 + 1);
            fsec = file_sectors(im->paths[file]);
            if (fsec < 0) { fclose(f); return false; }
        } else if (!strncmp(p, "TRACK", 5)) {
            int n; char type[32] = "";
            if (sscanf(p, "TRACK %d %31s", &n, type) == 2 && n >= 1 && n <= MCD_MAX_TRACKS) {
                track = n;
                d->track[n].data = strncmp(type, "AUDIO", 5) != 0;
                d->track[n].file = file;
                if (!d->first) d->first = n;
                d->last = n;
            }
        } else if (!strncmp(p, "INDEX", 5) && track) {
            int idx, mm, ss, ff;
            if (sscanf(p, "INDEX %d %d:%d:%d", &idx, &mm, &ss, &ff) == 4) {
                long off = (mm * 60L + ss) * 75 + ff;
                if (idx == 1) { d->track[track].lba = base + off; d->track[track].file_lba = base; }
            }
        }
    }
    fclose(f);
    if (file >= 0) base += fsec;
    d->lead_out = base;
    d->loaded = d->first > 0;
    return d->loaded;
}

/* the sector at LBA of one image, by its own table */
static bool image_read(Image *im, long lba, uint8_t raw[2352]) {
    const McdDisc *d = &im->d;
    if (!d->loaded || lba < 0 || lba >= d->lead_out) return false;
    int fi = -1;
    for (int n = d->first; n <= d->last; n++)       /* the file holding it: the last starting at or before */
        if (d->track[n].file_lba <= lba) fi = d->track[n].file;
    if (fi < 0) return false;
    long first = 0;
    for (int n = d->first; n <= d->last; n++) if (d->track[n].file == fi) { first = d->track[n].file_lba; break; }
    if (!im->files[fi] && !(im->files[fi] = fopen(im->paths[fi], "rb"))) return false;
    if (fseek(im->files[fi], (lba - first) * 2352L, SEEK_SET)) return false;
    return fread(raw, 1, 2352, im->files[fi]) == 2352;
}

static long track_len(const McdDisc *d, int n) {    /* index 1 to the next track's index 1 (or the lead-out) */
    return (n < d->last ? d->track[n + 1].lba : d->lead_out) - d->track[n].lba;
}

bool mcd_disc_open(const char *cue) {
    mcd_disc_close();
    if (!image_open(&s_main, cue)) return false;
    s_disc = s_main.d;
    const char *alt = getenv("PS_CD_AUDIO");
    if (alt && *alt && strcmp(alt, cue) && image_open(&s_alt, alt)) {
        const McdDisc *a = &s_alt.d;
        int n = s_disc.first;
        while (n <= s_disc.last && s_disc.track[n].data) n++;           /* the first audio track */
        if (n <= s_disc.last && a->last == s_disc.last && a->first == s_disc.first) {
            for (int k = n; k <= s_disc.last; k++) {
                if (s_disc.track[k].data || a->track[k].data) { image_close(&s_alt); s_disc = s_main.d; return s_disc.loaded; }
                if (k > n) s_disc.track[k].lba = s_disc.track[k - 1].lba + track_len(a, k - 1);
            }
            s_disc.lead_out = s_disc.track[s_disc.last].lba + track_len(a, s_disc.last);
            s_mixed = true;
            fprintf(stderr, "disc: audio tracks %d-%d from %s\n", n, s_disc.last, alt);
        } else {
            fprintf(stderr, "disc: %s has another track layout: its audio is not used\n", alt);
            image_close(&s_alt);
        }
    }
    return s_disc.loaded;
}

void mcd_disc_close(void) {
    image_close(&s_main); image_close(&s_alt);
    memset(&s_disc, 0, sizeof s_disc);
    s_mixed = false;
}

/* the track LBA lies in (pregaps belong to the track they precede), 0 outside the program area */
int mcd_disc_track_of(long lba) {
    if (!s_disc.loaded || lba < 0 || lba >= s_disc.lead_out) return 0;
    int t = s_disc.first;
    for (int n = s_disc.first; n <= s_disc.last; n++) if (s_disc.track[n].lba <= lba) t = n;
    return t;
}

bool mcd_disc_read(int lba, uint8_t raw[2352]) {
    int t = mcd_disc_track_of(lba);
    if (!t) return false;
    if (s_mixed && !s_disc.track[t].data && lba >= s_disc.track[t].lba)       /* the other disc's track t */
        return image_read(&s_alt, s_alt.d.track[t].lba + (lba - s_disc.track[t].lba), raw);
    return image_read(&s_main, lba, raw);
}
