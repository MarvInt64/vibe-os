/* libimage — shared userspace image decoder.
 *
 * Decodes an in-memory image (PNG/JPEG/GIF/BMP, via stb_image) into a buffer of
 * packed 0x00RRGGBB pixels. Used by any app that needs raster images (the
 * browser, the wallpaper setter, …) so none of them reach into another app's
 * sources for decoding. */
#ifndef VIBEOS_IMAGE_H
#define VIBEOS_IMAGE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Decode `len` bytes at `data` into a freshly allocated w*h buffer of
 * 0x00RRGGBB pixels (alpha dropped). Returns 0 on failure (unsupported format
 * or decode error). Free the result with image_free. */
unsigned int *image_decode(const unsigned char *data, int len, int *w, int *h);
void image_free(unsigned int *px);

#ifdef __cplusplus
}
#endif

#endif
