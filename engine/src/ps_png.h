/* Minimal PNG writer (RGB, zlib) for debug dumps. */
#ifndef PS_PNG_H
#define PS_PNG_H
#include <stdbool.h>
#include <stdint.h>
/* `xrgb`: width*height XRGB8888 pixels, row stride = width. */
bool ps_png_write(const char *path, const uint8_t *xrgb, int width, int height);
/* Same pixels as a 32-bit BMP (the launcher's cartridge labels: SDL_LoadBMP reads it). */
bool ps_bmp_write(const char *path, const uint8_t *xrgb, int width, int height);
#endif
