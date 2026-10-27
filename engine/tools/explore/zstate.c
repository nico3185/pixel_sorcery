/* savestate (de)compression for explore.c, apart: zlib.h and GPGX's osd.h both declare crc32 */
#include <zlib.h>
unsigned long zs_bound(unsigned long n) { return compressBound(n); }
unsigned long zs_pack(void *dst, unsigned long cap, const void *src, unsigned long n) {
    uLongf len = cap;
    return compress2(dst, &len, src, n, 1) == Z_OK ? len : 0;
}
unsigned long zs_unpack(void *dst, unsigned long cap, const void *src, unsigned long n) {
    uLongf len = cap;
    return uncompress(dst, &len, src, n) == Z_OK ? len : 0;
}
