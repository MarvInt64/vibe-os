#ifndef VIBEOS_FONT_ATLAS_H
#define VIBEOS_FONT_ATLAS_H

#include "types.h"

/* Pre-baked anti-aliased glyph atlas, generated at build time from a TrueType
 * font (tools/genatlas.c -> kernel/src/font_atlas.c). The kernel renders these
 * 8-bit coverage bitmaps with integer alpha blending, so no floating point /
 * stb_truetype runs in the kernel (which is built -mno-sse). */

#define ATLAS_SIZES 3       /* indexed by text scale 1..3 -> 0..2 */
#define ATLAS_FIRST 32      /* first codepoint (space) */
#define ATLAS_COUNT 95      /* printable ASCII 32..126 */

struct atlas_glyph {
    short w, h;             /* coverage bitmap size */
    short xoff, yoff;       /* offset from pen (yoff relative to baseline) */
    short advance;          /* proportional advance width */
    const unsigned char *cov;   /* w*h coverage, or 0 (e.g. space) */
};

extern const struct atlas_glyph FONT_ATLAS[ATLAS_SIZES][ATLAS_COUNT];
extern const short FONT_ATLAS_PX[ATLAS_SIZES];
extern const short FONT_ATLAS_ASCENT[ATLAS_SIZES];
extern const short FONT_ATLAS_LINEH[ATLAS_SIZES];
extern const short FONT_ATLAS_SPACE[ATLAS_SIZES];   /* space advance */
/* Monospace cell width per size (max digit advance) for terminal/CLI layout. */
extern const short FONT_ATLAS_CELLW[ATLAS_SIZES];

#endif
