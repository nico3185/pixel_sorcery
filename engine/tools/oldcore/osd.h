/* osd.h for oldrun: the platform layer Genesis Plus GX's core includes (no frontend) */
#ifndef _OSD_H_
#define _OSD_H_
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <zlib.h>
#define MAX_INPUTS 8
#define MAX_KEYS   8
#define MAXPATHLEN 1024
#include "config.h"   /* the checkout's sdl/config.h (t_config), copied in by build.sh */
#define osd_input_update oldrun_input_update
void oldrun_input_update(void);
int load_archive(char *filename, unsigned char *buffer, int maxsize, char *extension);
#define GG_ROM      "./ggenie.bin"
#define AR_ROM      "./areplay.bin"
#define SK_ROM      "./sk.bin"
#define SK_UPMEM    "./sk2chip.bin"
#define CD_BIOS_US  "./bios_CD_U.bin"
#define CD_BIOS_EU  "./bios_CD_E.bin"
#define CD_BIOS_JP  "./bios_CD_J.bin"
#define MD_BIOS     "./bios_MD.bin"
#define MS_BIOS_US  "./bios_U.sms"
#define MS_BIOS_EU  "./bios_E.sms"
#define MS_BIOS_JP  "./bios_J.sms"
#define GG_BIOS     "./bios.gg"
#endif
