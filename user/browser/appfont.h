/* appfont — proportional, anti-aliased text for VibeOS apps via stb_truetype.
 *
 * Rasterizes glyphs from an embedded TrueType font into 8-bit coverage bitmaps,
 * caches them, and reports proportional advance widths and vertical metrics.
 * Needs userspace floating point (SSE), now enabled by the kernel. */
#ifndef VIBEOS_APPFONT_H
#define VIBEOS_APPFONT_H

#ifdef __cplusplus
extern "C" {
#endif

struct af_glyph {
    int w, h;             /* coverage bitmap size */
    int xoff, yoff;       /* offset from pen/baseline (yoff negative = above baseline) */
    int advance;          /* horizontal advance in px */
    const unsigned char *cov;  /* w*h 8-bit coverage, or 0 (e.g. space) */
};

int appfont_init(void);                          /* 0 ok, <0 on bad font */
int appfont_advance(int codepoint, int px);      /* advance width at pixel size */
int appfont_ascent(int px);                      /* baseline offset from line top */
int appfont_line_height(int px);                 /* full line height */
const struct af_glyph *appfont_get(int codepoint, int px);  /* cached AA glyph */

#ifdef __cplusplus
}
#endif

#endif
