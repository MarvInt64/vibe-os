#ifndef LIBSVG_SVG_H
#define LIBSVG_SVG_H

/* libsvg — a small, self-contained SVG rasterizer for UI icons and logos.
 *
 * It renders into a caller-provided straight-alpha RGBA buffer; the caller
 * then blits or composites that buffer wherever it likes (a window canvas,
 * the framebuffer, ...). The library has no I/O and no libc dependency, so
 * it can be reused by any VibeOS component that can supply a pixel buffer.
 *
 * Supported SVG subset:
 *   - arbitrary viewBox (uniform fit, centered, small edge inset)
 *   - <defs>: <linearGradient> (userSpaceOnUse, x1/y1/x2/y2, <stop>s) and
 *     <filter> (feGaussianBlur / feColorMatrix / feBlend)
 *   - <g> with filter / opacity (grouped, filtered layers)
 *   - <path d> with M/L/H/V/C/Z and their relative forms
 *   - <rect> (x/y/width/height), <circle> (cx/cy/r)
 *   - fill / stroke as none | #hex | url(#gradient) | currentColor
 *   - fill-opacity, stroke-opacity, opacity, stroke-width, round caps/joins
 *
 * Note: this implementation uses SSE float math, so it must be compiled for
 * a target where floating point is available (VibeOS userspace, not the
 * SSE-less kernel).
 */

#define SVG_MAX_DIM 128   /* maximum rendered icon side, in pixels */

#ifdef __cplusplus
extern "C" {
#endif

/* Rasterize `svg` into `out`, a `size`x`size` row-major buffer of straight
 * alpha pixels in 0xAARRGGBB order. `size` is clamped to SVG_MAX_DIM and the
 * buffer is fully overwritten. `current_color` (0xRRGGBB) resolves any
 * `currentColor` paint references. */
void svg_render_rgba(const char *svg, unsigned int *out, int size,
                     unsigned int current_color);

#ifdef __cplusplus
}
#endif

#endif /* LIBSVG_SVG_H */
