/*
 * Mega CD on the Genesis Plus GX backend (reference player, tracer): BIOS paths and backup RAM.
 * BIOS: PS_SCD_BIOS=DIR (default Application Support/PixelSorcery/bios), the Model 1 BIOS of each
 * region dumped from your own console, as scd_us.bin, scd_eu.bin, scd_jp.bin (GPGX picks one from
 * the disc; GENRECOMP_REGION forces one). Backup RAM - the internal
 * 8 KB and the 512 KB RAM cartridge GPGX plugs in when booting from CD - is formatted with Sega's
 * standard directory header when new (games otherwise stop at "RAM cartridge is not initialized")
 * and kept in Application Support/PixelSorcery/megacd/{scd,cart}.brm by players (not by tools).
 */
#include "shared.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

void ps_megacd_bios(void) {
    const char *dir = getenv("PS_SCD_BIOS"), *home = getenv("HOME");
    char def[512]; snprintf(def, sizeof def, "%s/Library/Application Support/PixelSorcery/bios", home ? home : ".");
    if (!dir || !*dir) dir = def;
    snprintf(CD_BIOS_US, sizeof CD_BIOS_US, "%s/scd_us.bin", dir);
    snprintf(CD_BIOS_EU, sizeof CD_BIOS_EU, "%s/scd_eu.bin", dir);
    snprintf(CD_BIOS_JP, sizeof CD_BIOS_JP, "%s/scd_jp.bin", dir);
}

static void brm_path(const char *name, char *out, size_t n) {
    const char *home = getenv("HOME");
    snprintf(out, n, "%s/Library/Application Support/PixelSorcery/megacd/%s", home ? home : ".", name);
}
static void brm_format(uint8 *ram, uint32 size) {
    static const char tail[0x20] = "SEGA_CD_ROM\0\1\0\0\0RAM_CARTRIDGE___";
    if (!memcmp(ram + size - 0x20, tail, 0x20)) return;               /* already formatted */
    memset(ram, 0, size);
    uint8 *h = ram + size - 0x40;
    memset(h, '_', 11); h[0x0F] = 0x40;                               /* volume name, directory */
    unsigned blocks = size / 64 - 3;                                  /* free blocks, 4 copies */
    for (int i = 0; i < 4; i++) { h[0x10 + 2 * i] = (uint8)(blocks >> 8); h[0x11 + 2 * i] = (uint8)blocks; }
    memcpy(h + 0x20, tail, 0x20);
}
/* after system_reset(): load (files = true) or just format; at exit: save */
void ps_megacd_brm(bool load_files, bool save) {
    if (system_hw != SYSTEM_MCD) return;
    struct { const char *name; uint8 *ram; uint32 size; } b[2] = {
        { "scd.brm", scd.bram, sizeof scd.bram },
        { "cart.brm", scd.cartridge.area, scd.cartridge.id ? scd.cartridge.mask + 1 : 0 } };
    char dir[600]; brm_path("", dir, sizeof dir);
    if (save) for (char *q = dir + 1; *q; q++) if (*q == '/') { *q = 0; mkdir(dir, 0755); *q = '/'; }
    for (int i = 0; i < 2; i++) {
        if (!b[i].size) continue;
        char p[700]; brm_path(b[i].name, p, sizeof p);
        if (save) { FILE *f = fopen(p, "wb"); if (f) { fwrite(b[i].ram, 1, b[i].size, f); fclose(f); } continue; }
        if (load_files) {
            FILE *f = fopen(p, "rb");
            if (f) { if (fread(b[i].ram, 1, b[i].size, f) != b[i].size) memset(b[i].ram, 0, b[i].size); fclose(f); }
        }
        brm_format(b[i].ram, b[i].size);
    }
}
