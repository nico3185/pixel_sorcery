/* mcd_disc.h -- the player's disc image (cue + bin) for the native Mega CD (mcd_disc.c) */
#ifndef PS_MCD_DISC_H
#define PS_MCD_DISC_H
#include <stdbool.h>
#include <stdint.h>

#define MCD_MAX_TRACKS 99
typedef struct {
    bool loaded;
    int first, last;                     /* track numbers */
    long lead_out;                       /* LBA of the lead-out */
    struct { long lba, file_lba; int file; bool data; } track[MCD_MAX_TRACKS + 1];   /* lba: index 1 */
} McdDisc;

bool mcd_disc_open(const char *cue);
void mcd_disc_close(void);
const McdDisc *mcd_disc(void);
int  mcd_disc_track_of(long lba);        /* 0 outside the program area */
bool mcd_disc_read(int lba, uint8_t raw[2352]);
#endif
