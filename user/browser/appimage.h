/* appimage - decode an in-memory PNG/JPEG/GIF/BMP to packed 0x00RRGGBB pixels.
 * Returns a umalloc'd w*h buffer (free with appimage_free), or 0 on failure
 * (unsupported format like WebP, or decode error). */
#ifndef VIBEOS_APPIMAGE_H
#define VIBEOS_APPIMAGE_H

#ifdef __cplusplus
extern "C" {
#endif
unsigned int *appimage_decode(const unsigned char *data, int len, int *w, int *h);
void appimage_free(unsigned int *px);
#ifdef __cplusplus
}
#endif

#endif
