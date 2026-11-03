/*
 * mcd_disc.c -- the player's Mega CD disc image for the native Mega CD: a cue sheet with its bin
 * file(s) (one per track or one for all; MODE1/2352 data, AUDIO tracks, INDEX 00 pregaps inside
 * the files). Gives the drive the table of contents and raw 2352-byte sectors by LBA (sector 0 =
 * track 1, index 1 = 00:02:00 absolute). The port's own code.
 */
#include "mcd_disc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static McdDisc s_disc;
static FILE *s_files[MCD_MAX_TRACKS + 1];
static char s_paths[MCD_MAX_TRACKS + 1][1024];
static int s_nfiles;

const McdDisc *mcd_disc(void) { return &s_disc; }

static long file_sectors(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f) / 2352;
    fclose(f);
    return n;
}

bool mcd_disc_open(const char *cue) {
    mcd_disc_close();
    FILE *f = fopen(cue, "r");
    if (!f) return false;
    char dir[1024]; snprintf(dir, sizeof dir, "%s", cue);
    char *slash = strrchr(dir, '/'); if (slash) slash[1] = 0; else dir[0] = 0;
    char line[1200];
    int track = 0, file = -1;
    long base = 0;                                  /* LBA where the current file starts */
    long fsec = 0;                                  /* its length in sectors */
    while (fgets(line, sizeof line, f)) {
        char *p = line; while (*p == ' ' || *p == '\t') p++;
        if (!strncmp(p, "FILE", 4)) {
            char *q1 = strchr(p, '"'), *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
            if (!q1 || !q2 || s_nfiles > MCD_MAX_TRACKS) continue;
            if (file >= 0) base += fsec;
            file = s_nfiles++;
            snprintf(s_paths[file], sizeof s_paths[file], "%s%.*s", dir, (int)(q2 - q1 - 1), q1 + 1);
            fsec = file_sectors(s_paths[file]);
            if (fsec < 0) { fclose(f); return false; }
        } else if (!strncmp(p, "TRACK", 5)) {
            int n; char type[32] = "";
            if (sscanf(p, "TRACK %d %31s", &n, type) == 2 && n >= 1 && n <= MCD_MAX_TRACKS) {
                track = n;
                s_disc.track[n].data = strncmp(type, "AUDIO", 5) != 0;
                s_disc.track[n].file = file;
                if (!s_disc.first) s_disc.first = n;
                s_disc.last = n;
            }
        } else if (!strncmp(p, "INDEX", 5) && track) {
            int idx, mm, ss, ff;
            if (sscanf(p, "INDEX %d %d:%d:%d", &idx, &mm, &ss, &ff) == 4) {
                long off = (mm * 60L + ss) * 75 + ff;
                if (idx == 1) { s_disc.track[track].lba = base + off; s_disc.track[track].file_lba = base; }
            }
        }
    }
    fclose(f);
    if (file >= 0) base += fsec;
    s_disc.lead_out = base;
    s_disc.loaded = s_disc.first > 0;
    return s_disc.loaded;
}

void mcd_disc_close(void) {
    for (int i = 0; i < s_nfiles; i++) if (s_files[i]) { fclose(s_files[i]); s_files[i] = NULL; }
    s_nfiles = 0;
    memset(&s_disc, 0, sizeof s_disc);
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
    int fi = s_disc.track[t].file;
    /* the file holding this sector: the last file starting at or before it */
    for (int n = s_disc.first; n <= s_disc.last; n++)
        if (s_disc.track[n].file_lba <= lba) fi = s_disc.track[n].file;
    long first = 0;
    for (int n = s_disc.first; n <= s_disc.last; n++) if (s_disc.track[n].file == fi) { first = s_disc.track[n].file_lba; break; }
    if (!s_files[fi] && !(s_files[fi] = fopen(s_paths[fi], "rb"))) return false;
    if (fseek(s_files[fi], (lba - first) * 2352L, SEEK_SET)) return false;
    return fread(raw, 1, 2352, s_files[fi]) == 2352;
}
