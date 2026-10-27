/* Minimal PNG writer (8-bit RGB, one zlib-compressed IDAT). */
#include "ps_png.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static void chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len) {
    uint8_t hdr[8];
    put32(hdr, len);
    memcpy(hdr + 4, type, 4);
    fwrite(hdr, 1, 8, f);
    if (len) fwrite(data, 1, len, f);
    uint32_t crc = (uint32_t)crc32(0, (const Bytef *)type, 4);
    if (len) crc = (uint32_t)crc32(crc, data, len);
    uint8_t c[4];
    put32(c, crc);
    fwrite(c, 1, 4, f);
}

bool ps_png_write(const char *path, const uint8_t *xrgb, int width, int height) {
    size_t raw_len = (size_t)height * (1 + (size_t)width * 3);
    uint8_t *raw = malloc(raw_len);
    if (!raw) return false;
    const uint32_t *px = (const uint32_t *)xrgb;
    uint8_t *o = raw;
    for (int y = 0; y < height; y++) {
        *o++ = 0;                                  /* filter: none */
        for (int x = 0; x < width; x++) {
            uint32_t p = px[(size_t)y * width + x];
            *o++ = (uint8_t)(p >> 16); *o++ = (uint8_t)(p >> 8); *o++ = (uint8_t)p;
        }
    }
    uLongf zlen = compressBound((uLong)raw_len);
    uint8_t *z = malloc(zlen);
    bool ok = z && compress2(z, &zlen, raw, (uLong)raw_len, 6) == Z_OK;
    FILE *f = ok ? fopen(path, "wb") : NULL;
    if (f) {
        static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
        uint8_t ihdr[13];
        fwrite(sig, 1, 8, f);
        put32(ihdr, (uint32_t)width); put32(ihdr + 4, (uint32_t)height);
        ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
        chunk(f, "IHDR", ihdr, 13);
        chunk(f, "IDAT", z, (uint32_t)zlen);
        chunk(f, "IEND", NULL, 0);
        fclose(f);
    }
    free(raw); free(z);
    return f != NULL;
}

/* 32-bit BMP (bottom-up, BGRX): what SDL_LoadBMP reads without an image library */
bool ps_bmp_write(const char *path, const uint8_t *xrgb, int width, int height) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    uint32_t size = (uint32_t)width * (uint32_t)height * 4;
    uint8_t h[54] = { 'B', 'M' };
    uint32_t v[] = { 54 + size, 0, 54, 40, (uint32_t)width, (uint32_t)height };
    for (int i = 0; i < 6; i++) for (int b = 0; b < 4; b++) h[2 + i * 4 + b] = (uint8_t)(v[i] >> (8 * b));
    h[26] = 1; h[28] = 32; h[34] = (uint8_t)size; h[35] = (uint8_t)(size >> 8);
    h[36] = (uint8_t)(size >> 16); h[37] = (uint8_t)(size >> 24);
    fwrite(h, 1, 54, f);
    for (int y = height - 1; y >= 0; y--) fwrite(xrgb + (size_t)y * width * 4, 4, (size_t)width, f);   /* XRGB LE = BGRX */
    fclose(f);
    return true;
}
