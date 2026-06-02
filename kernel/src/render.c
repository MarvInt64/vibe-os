#include "render.h"
#include "font_atlas.h"
#include "string.h"
#include "types.h"

static uint32_t mix_channel(uint32_t a, uint32_t b, uint32_t step, uint32_t total) {
    return ((a * (total - step)) + (b * step)) / total;
}

static uint32_t lerp_color(uint32_t a, uint32_t b, uint32_t step, uint32_t total) {
    uint32_t ar = (a >> 16) & 0xffu;
    uint32_t ag = (a >> 8) & 0xffu;
    uint32_t ab = a & 0xffu;
    uint32_t br = (b >> 16) & 0xffu;
    uint32_t bg = (b >> 8) & 0xffu;
    uint32_t bb = b & 0xffu;

    return (mix_channel(ar, br, step, total) << 16) |
           (mix_channel(ag, bg, step, total) << 8) |
           mix_channel(ab, bb, step, total);
}

static uint32_t blend_color(uint32_t dest, uint32_t src, uint8_t alpha) {
    uint32_t dr = (dest >> 16) & 0xffu;
    uint32_t dg = (dest >> 8) & 0xffu;
    uint32_t db = dest & 0xffu;
    uint32_t sr = (src >> 16) & 0xffu;
    uint32_t sg = (src >> 8) & 0xffu;
    uint32_t sb = src & 0xffu;

    return ((((sr * alpha) + (dr * (255u - alpha))) / 255u) << 16) |
           ((((sg * alpha) + (dg * (255u - alpha))) / 255u) << 8) |
           (((sb * alpha) + (db * (255u - alpha))) / 255u);
}

static int clamp_min(int value, int min) {
    return value < min ? min : value;
}

static int clamp_max(int value, int max) {
    return value > max ? max : value;
}

static int framebuffer_intersect_rect(const struct framebuffer *fb, int *x, int *y, int *width, int *height) {
    int x0 = *x;
    int y0 = *y;
    int x1 = *x + *width;
    int y1 = *y + *height;

    if (*width <= 0 || *height <= 0) {
        return 0;
    }

    x0 = clamp_min(x0, 0);
    y0 = clamp_min(y0, 0);
    x1 = clamp_max(x1, (int)fb->width);
    y1 = clamp_max(y1, (int)fb->height);

    if (fb->clip_enabled) {
        x0 = clamp_min(x0, fb->clip_x);
        y0 = clamp_min(y0, fb->clip_y);
        x1 = clamp_max(x1, fb->clip_x + fb->clip_width);
        y1 = clamp_max(y1, fb->clip_y + fb->clip_height);
    }

    if (x0 >= x1 || y0 >= y1) {
        return 0;
    }

    *x = x0;
    *y = y0;
    *width = x1 - x0;
    *height = y1 - y0;
    return 1;
}

static void fb_blend_pixel(struct framebuffer *fb, int x, int y, uint32_t color, uint8_t alpha) {
    uint8_t *row;
    uint32_t *pixel;

    if (alpha == 0u) {
        return;
    }
    if (alpha == 255u) {
        fb_put_pixel(fb, x, y, color);
        return;
    }
    if (fb == 0 || fb->base == 0) {
        return;
    }
    if (x < 0 || y < 0 || (uint32_t)x >= fb->width || (uint32_t)y >= fb->height) {
        return;
    }
    if (fb->clip_enabled &&
        (x < fb->clip_x || y < fb->clip_y || x >= fb->clip_x + fb->clip_width || y >= fb->clip_y + fb->clip_height)) {
        return;
    }

    row = (uint8_t *)fb->base + ((uint32_t)y * fb->pitch);
    pixel = (uint32_t *)(row + ((uint32_t)x * (fb->bpp / 8u)));
    *pixel = blend_color(*pixel, color, alpha);
}

static int rounded_rect_contains(int px, int py, int x, int y, int width, int height, int radius) {
	int right;
	int bottom;
	int dx;
	int dy;

	if (px < x || py < y || px >= x + width || py >= y + height) {
		return 0;
	}

	if (radius <= 0) {
		return 1;
	}

	right = x + width - 1;
	bottom = y + height - 1;

	if (px >= x + radius && px <= right - radius) {
		return 1;
	}
	if (py >= y + radius && py <= bottom - radius) {
		return 1;
	}

	dx = px < x + radius ? (x + radius - 1) - px : px - (right - radius + 1);
	dy = py < y + radius ? (y + radius - 1) - py : py - (bottom - radius + 1);
	return (dx * dx) + (dy * dy) <= (radius * radius);
}

static void get_rounded_rect_scanline_bounds(int y_pos, int x, int y, int width, int height, int radius, int *out_x0, int *out_x1) {
	int right = x + width - 1;
	int bottom = y + height - 1;

	if (y_pos < y || y_pos > bottom) {
		*out_x0 = x;
		*out_x1 = x;
		return;
	}

	if (radius <= 0 || (y_pos >= y + radius && y_pos <= bottom - radius)) {
		*out_x0 = x;
		*out_x1 = right;
		return;
	}

	if (y_pos < y + radius) {
		int dy = (y + radius - 1) - y_pos;
		int dx = 0;
		while ((dx * dx) + (dy * dy) <= (radius * radius)) {
			++dx;
		}
		*out_x0 = x + radius - dx;
		*out_x1 = right - radius + dx;
	} else {
		int dy = y_pos - (bottom - radius + 1);
		int dx = 0;
		while ((dx * dx) + (dy * dy) <= (radius * radius)) {
			++dx;
		}
		*out_x0 = x + radius - dx;
		*out_x1 = right - radius + dx;
	}
}

static void fill_rounded_rect_gradient(struct framebuffer *fb, int x, int y, int width, int height, int radius, uint32_t top, uint32_t bottom) {
	int shape_x = x;
	int shape_y = y;
	int shape_width = width;
	int shape_height = height;
	int iy;

	if (!framebuffer_intersect_rect(fb, &x, &y, &width, &height)) {
		return;
	}

	for (iy = y; iy < y + height; ++iy) {
		uint32_t row_color = lerp_color(top, bottom, (uint32_t)(iy - shape_y), (uint32_t)(shape_height == 0 ? 1 : shape_height));
		int scan_x0, scan_x1;
		get_rounded_rect_scanline_bounds(iy, shape_x, shape_y, shape_width, shape_height, radius, &scan_x0, &scan_x1);
		if (scan_x0 < x) scan_x0 = x;
		if (scan_x1 > x + width - 1) scan_x1 = x + width - 1;
		if (scan_x0 <= scan_x1) {
			fb_fill_rect(fb, scan_x0, iy, scan_x1 - scan_x0 + 1, 1, row_color);
		}
	}
}

static void fill_rounded_rect_alpha(struct framebuffer *fb, int x, int y, int width, int height, int radius, uint32_t color, uint8_t alpha) {
	int shape_x = x;
	int shape_y = y;
	int shape_width = width;
	int shape_height = height;
	int iy;
	int ix;

	if (!framebuffer_intersect_rect(fb, &x, &y, &width, &height)) {
		return;
	}

	for (iy = y; iy < y + height; ++iy) {
		int scan_x0, scan_x1;
		get_rounded_rect_scanline_bounds(iy, shape_x, shape_y, shape_width, shape_height, radius, &scan_x0, &scan_x1);
		if (scan_x0 < x) scan_x0 = x;
		if (scan_x1 > x + width - 1) scan_x1 = x + width - 1;
		for (ix = scan_x0; ix <= scan_x1; ++ix) {
			fb_blend_pixel(fb, ix, iy, color, alpha);
		}
	}
}

static void draw_rounded_outline(struct framebuffer *fb, int x, int y, int width, int height, int radius, int thickness, uint32_t color) {
	int iy;

	for (iy = y; iy < y + height; ++iy) {
		int scan_x0, scan_x1;
		int inner_x0, inner_x1;
		get_rounded_rect_scanline_bounds(iy, x, y, width, height, radius, &scan_x0, &scan_x1);
		if (thickness > 0 && radius > thickness) {
			get_rounded_rect_scanline_bounds(iy, x + thickness, y + thickness, width - (thickness * 2), height - (thickness * 2), radius - thickness, &inner_x0, &inner_x1);
		} else if (thickness > 0) {
			get_rounded_rect_scanline_bounds(iy, x + thickness, y + thickness, width - (thickness * 2), height - (thickness * 2), 0, &inner_x0, &inner_x1);
		} else {
			inner_x0 = scan_x0;
			inner_x1 = scan_x1;
		}

		if (scan_x0 <= scan_x1) {
			if (iy == y || iy == y + height - 1) {
				fb_fill_rect(fb, scan_x0, iy, scan_x1 - scan_x0 + 1, 1, color);
			} else {
				if (scan_x0 <= inner_x0) {
					fb_fill_rect(fb, scan_x0, iy, inner_x0 - scan_x0 + 1, 1, color);
				}
				if (inner_x1 <= scan_x1) {
					fb_fill_rect(fb, inner_x1, iy, scan_x1 - inner_x1 + 1, 1, color);
				}
			}
		}
	}
}

void fb_init(struct framebuffer *fb, uintptr_t address, uint32_t width, uint32_t height, uint32_t pitch, uint8_t bpp) {
    fb->base = (uint32_t *)address;
    fb->width = width;
    fb->height = height;
    fb->pitch = pitch;
    fb->bpp = bpp;
    fb->clip_enabled = 0;
    fb->clip_x = 0;
    fb->clip_y = 0;
    fb->clip_width = (int)width;
    fb->clip_height = (int)height;
}

void fb_put_pixel(struct framebuffer *fb, int x, int y, uint32_t color) {
    uint8_t *row;
    uint32_t *pixel;

    if (fb == 0 || fb->base == 0) {
        return;
    }

    if (x < 0 || y < 0 || (uint32_t)x >= fb->width || (uint32_t)y >= fb->height) {
        return;
    }

    if (fb->clip_enabled &&
        (x < fb->clip_x || y < fb->clip_y || x >= fb->clip_x + fb->clip_width || y >= fb->clip_y + fb->clip_height)) {
        return;
    }

    row = (uint8_t *)fb->base + ((uint32_t)y * fb->pitch);
    pixel = (uint32_t *)(row + ((uint32_t)x * (fb->bpp / 8u)));
    *pixel = color;
}

void fb_clear(struct framebuffer *fb, uint32_t color) {
    memset32(fb->base, color, fb->width * fb->height);
}

void fb_fill_rect(struct framebuffer *fb, int x, int y, int width, int height, uint32_t color) {
    int iy;
    uint32_t *row_start;

    if (!framebuffer_intersect_rect(fb, &x, &y, &width, &height)) {
        return;
    }

    row_start = (uint32_t *)((uint8_t *)fb->base + ((uint32_t)y * fb->pitch) + ((uint32_t)x * (fb->bpp / 8u)));
    for (iy = 0; iy < height; ++iy) {
        memset32(row_start, color, (size_t)width);
        row_start = (uint32_t *)((uint8_t *)row_start + fb->pitch);
    }
}

void fb_draw_rect(struct framebuffer *fb, int x, int y, int width, int height, int thickness, uint32_t color) {
    if (thickness <= 0) {
        return;
    }

    fb_fill_rect(fb, x, y, width, thickness, color);
    fb_fill_rect(fb, x, y + height - thickness, width, thickness, color);
    fb_fill_rect(fb, x, y, thickness, height, color);
    fb_fill_rect(fb, x + width - thickness, y, thickness, height, color);
}

void fb_blit(struct framebuffer *dest, const struct framebuffer *src) {
    struct rect rect = {0, 0, (int)src->width, (int)src->height};

    fb_blit_rect(dest, src, &rect);
}

void fb_blit_rect(struct framebuffer *dest, const struct framebuffer *src, const struct rect *rect) {
    int x;
    int y;
    int width;
    int height;
    int end_y;
    uint32_t row_bytes;

    if (dest == 0 || src == 0 || dest->base == 0 || src->base == 0) {
        return;
    }

    if (dest->width != src->width || dest->height != src->height || dest->bpp != src->bpp) {
        return;
    }

    x = rect->x;
    y = rect->y;
    width = rect->width;
    height = rect->height;
    if (!framebuffer_intersect_rect(src, &x, &y, &width, &height)) {
        return;
    }

    row_bytes = (uint32_t)width * (src->bpp / 8u);
    end_y = y + height;
    for (; y < end_y; ++y) {
        uint8_t *dest_row = (uint8_t *)dest->base + ((uint32_t)y * dest->pitch) + ((uint32_t)x * (dest->bpp / 8u));
        const uint8_t *src_row = (const uint8_t *)src->base + ((uint32_t)y * src->pitch) + ((uint32_t)x * (src->bpp / 8u));
        memcpy(dest_row, src_row, row_bytes);
    }
}

void fb_set_clip(struct framebuffer *fb, const struct rect *rect) {
    int x0 = rect->x;
    int y0 = rect->y;
    int x1 = rect->x + rect->width;
    int y1 = rect->y + rect->height;

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int)fb->width) x1 = (int)fb->width;
    if (y1 > (int)fb->height) y1 = (int)fb->height;

    if (x0 >= x1 || y0 >= y1) {
        fb->clip_enabled = 1;
        fb->clip_x = 0; fb->clip_y = 0; fb->clip_width = 0; fb->clip_height = 0;
    } else {
        fb->clip_enabled = 1;
        fb->clip_x = x0; fb->clip_y = y0;
        fb->clip_width = x1 - x0; fb->clip_height = y1 - y0;
    }
}

void fb_reset_clip(struct framebuffer *fb) {
    fb->clip_enabled = 0;
    fb->clip_x = 0;
    fb->clip_y = 0;
    fb->clip_width = (int)fb->width;
    fb->clip_height = (int)fb->height;
}

void draw_gradient_background(struct framebuffer *fb, uint32_t top, uint32_t bottom) {
    int y0 = 0;
    int y1 = (int)fb->height;
    int y;
    uint32_t *row;
    uint32_t color;
    uint32_t x;

    if (fb->clip_enabled) {
        y0 = fb->clip_y;
        y1 = fb->clip_y + fb->clip_height;
    }

    if (y0 < 0) {
        y0 = 0;
    }
    if (y1 > (int)fb->height) {
        y1 = (int)fb->height;
    }

    for (y = y0; y < y1; ++y) {
        color = lerp_color(top, bottom, (uint32_t)y, fb->height == 0 ? 1u : fb->height);
        row = (uint32_t *)((uint8_t *)fb->base + ((uint32_t)y * fb->pitch));
        for (x = 0; x < fb->width; ++x) {
            row[x] = color;
        }
    }
    for (y = (int)fb->height - 160; y < (int)fb->height; ++y) {
        if (y >= 0) {
            row = (uint32_t *)((uint8_t *)fb->base + ((uint32_t)y * fb->pitch));
            for (x = 0; x < fb->width; ++x) {
                row[x] = 0x08070d16u;
            }
        }
    }
}

void draw_rounded_panel(struct framebuffer *fb, int x, int y, int width, int height, int radius, uint32_t top, uint32_t bottom, uint32_t border, uint32_t highlight) {
    int inset = 1;
    int highlight_height;

    if (width <= 0 || height <= 0) {
        return;
    }

    fill_rounded_rect_gradient(fb, x, y, width, height, radius, border, border);
    fill_rounded_rect_gradient(fb, x + inset, y + inset, width - (inset * 2), height - (inset * 2), radius > inset ? radius - inset : 0, top, bottom);
    highlight_height = height / 5;
    if (highlight_height > 3) {
        fill_rounded_rect_alpha(fb, x + inset + 1, y + inset + 1, width - ((inset + 1) * 2), highlight_height, radius > inset + 1 ? radius - inset - 1 : 0, highlight, 18u);
    }
}

void draw_panel(struct framebuffer *fb, int x, int y, int width, int height, uint32_t color, uint32_t border) {
    uint32_t top = lerp_color(color, 0x00ffffffu, 1u, 10u);
    uint32_t bottom = lerp_color(color, 0x00000000u, 1u, 12u);
    draw_rounded_panel(fb, x, y, width, height, 10, top, bottom, border, 0x00ffffffu);
}

void draw_soft_shadow(struct framebuffer *fb, int x, int y, int width, int height, int radius, int spread, uint32_t color) {
	int i;
	int iy;
	int outer_x0, outer_x1;

	if (spread < 1) {
		return;
	}

	/* Concentric ring outlines, each alpha-blended over the *existing* backdrop
	 * (not stamped opaque over black), so the result is a soft falloff that
	 * darkens the desktop slightly — never a hard near-black outline. Rings
	 * closest to the window are most opaque and fade out with distance. */
	for (i = spread; i >= 1; --i) {
		uint8_t alpha = (uint8_t)((uint32_t)(spread - i + 1) * 34u / (uint32_t)spread);
		int sx = x - i;
		int sy = y - i;
		int sw = width + (i * 2);
		int sh = height + (i * 2);
		int sr = radius + i;

		for (iy = 0; iy < sh; ++iy) {
			int abs_y = sy + iy;
			get_rounded_rect_scanline_bounds(abs_y, sx, sy, sw, sh, sr, &outer_x0, &outer_x1);
			if (outer_x0 > outer_x1) {
				continue;
			}
			if (iy == 0 || iy == sh - 1) {
				int k;
				for (k = outer_x0; k <= outer_x1; ++k) {
					fb_blend_pixel(fb, k, abs_y, color, alpha);
				}
			} else {
				fb_blend_pixel(fb, outer_x0, abs_y, color, alpha);
				fb_blend_pixel(fb, outer_x1, abs_y, color, alpha);
			}
		}
	}
}

void draw_shadow(struct framebuffer *fb, int x, int y, int width, int height, int spread, uint32_t color) {
    draw_soft_shadow(fb, x, y, width, height, 12, spread, color);
}

/* 8x16 bitmap font, generated by tools/genfont.py. 8 px wide rows. */
static const uint8_t F16_SPACE[16] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
static const uint8_t F16_A[16] = {0,0,56,68,68,68,68,124,68,68,68,68,0,0,0,0};
static const uint8_t F16_B[16] = {0,0,120,68,68,68,120,68,68,68,68,120,0,0,0,0};
static const uint8_t F16_C[16] = {0,0,56,68,64,64,64,64,64,64,68,56,0,0,0,0};
static const uint8_t F16_D[16] = {0,0,112,72,68,68,68,68,68,68,72,112,0,0,0,0};
static const uint8_t F16_E[16] = {0,0,124,64,64,64,120,64,64,64,64,124,0,0,0,0};
static const uint8_t F16_F[16] = {0,0,124,64,64,64,120,64,64,64,64,64,0,0,0,0};
static const uint8_t F16_G[16] = {0,0,56,68,64,64,64,92,68,68,68,56,0,0,0,0};
static const uint8_t F16_H[16] = {0,0,68,68,68,68,124,68,68,68,68,68,0,0,0,0};
static const uint8_t F16_I[16] = {0,0,124,16,16,16,16,16,16,16,16,124,0,0,0,0};
static const uint8_t F16_J[16] = {0,0,30,4,4,4,4,4,4,68,68,56,0,0,0,0};
static const uint8_t F16_K[16] = {0,0,68,72,80,96,96,80,80,72,72,68,0,0,0,0};
static const uint8_t F16_L[16] = {0,0,64,64,64,64,64,64,64,64,64,124,0,0,0,0};
static const uint8_t F16_M[16] = {0,0,68,108,84,84,68,68,68,68,68,68,0,0,0,0};
static const uint8_t F16_N[16] = {0,0,68,100,100,84,84,76,76,68,68,68,0,0,0,0};
static const uint8_t F16_O[16] = {0,0,56,68,68,68,68,68,68,68,68,56,0,0,0,0};
static const uint8_t F16_P[16] = {0,0,120,68,68,68,120,64,64,64,64,64,0,0,0,0};
static const uint8_t F16_Q[16] = {0,0,56,68,68,68,68,68,68,84,72,52,0,0,0,0};
static const uint8_t F16_R[16] = {0,0,120,68,68,68,120,80,72,72,68,68,0,0,0,0};
static const uint8_t F16_S[16] = {0,0,56,68,64,64,56,4,4,4,68,56,0,0,0,0};
static const uint8_t F16_T[16] = {0,0,124,16,16,16,16,16,16,16,16,16,0,0,0,0};
static const uint8_t F16_U[16] = {0,0,68,68,68,68,68,68,68,68,68,56,0,0,0,0};
static const uint8_t F16_V[16] = {0,0,68,68,68,68,68,68,40,40,16,16,0,0,0,0};
static const uint8_t F16_W[16] = {0,0,68,68,68,68,84,84,84,108,68,68,0,0,0,0};
static const uint8_t F16_X[16] = {0,0,68,68,40,40,16,16,40,40,68,68,0,0,0,0};
static const uint8_t F16_Y[16] = {0,0,68,68,40,40,16,16,16,16,16,16,0,0,0,0};
static const uint8_t F16_Z[16] = {0,0,124,4,8,16,16,32,32,64,64,124,0,0,0,0};
static const uint8_t F16_0[16] = {0,0,56,68,68,76,84,100,68,68,68,56,0,0,0,0};
static const uint8_t F16_1[16] = {0,0,16,48,80,16,16,16,16,16,16,124,0,0,0,0};
static const uint8_t F16_2[16] = {0,0,56,68,4,4,8,16,32,64,64,124,0,0,0,0};
static const uint8_t F16_3[16] = {0,0,120,4,4,4,56,4,4,4,68,56,0,0,0,0};
static const uint8_t F16_4[16] = {0,0,12,20,36,68,68,124,4,4,4,4,0,0,0,0};
static const uint8_t F16_5[16] = {0,0,124,64,64,64,120,4,4,4,68,56,0,0,0,0};
static const uint8_t F16_6[16] = {0,0,56,68,64,64,120,68,68,68,68,56,0,0,0,0};
static const uint8_t F16_7[16] = {0,0,124,4,8,8,16,16,32,32,32,32,0,0,0,0};
static const uint8_t F16_8[16] = {0,0,56,68,68,68,56,68,68,68,68,56,0,0,0,0};
static const uint8_t F16_9[16] = {0,0,56,68,68,68,68,60,4,4,68,56,0,0,0,0};
static const uint8_t F16_DOT[16] = {0,0,0,0,0,0,0,0,0,0,24,24,0,0,0,0};
static const uint8_t F16_COMMA[16] = {0,0,0,0,0,0,0,0,0,0,24,24,8,48,0,0};
static const uint8_t F16_COLON[16] = {0,0,0,0,24,24,0,0,0,24,24,0,0,0,0,0};
static const uint8_t F16_SEMI[16] = {0,0,0,0,24,24,0,0,0,24,24,8,48,0,0,0};
static const uint8_t F16_DASH[16] = {0,0,0,0,0,0,124,0,0,0,0,0,0,0,0,0};
static const uint8_t F16_USCORE[16] = {0,0,0,0,0,0,0,0,0,0,0,0,126,0,0,0};
static const uint8_t F16_SLASH[16] = {0,0,4,4,8,8,16,16,32,32,64,64,0,0,0,0};
static const uint8_t F16_BSLASH[16] = {0,0,64,64,32,32,16,16,8,8,4,4,0,0,0,0};
static const uint8_t F16_LPAREN[16] = {0,0,8,16,32,32,32,32,32,32,16,8,0,0,0,0};
static const uint8_t F16_RPAREN[16] = {0,0,32,16,8,8,8,8,8,8,16,32,0,0,0,0};
static const uint8_t F16_LT[16] = {0,0,0,4,8,16,32,64,32,16,8,4,0,0,0,0};
static const uint8_t F16_GT[16] = {0,0,0,64,32,16,8,4,8,16,32,64,0,0,0,0};
static const uint8_t F16_PIPE[16] = {0,0,16,16,16,16,16,16,16,16,16,16,0,0,0,0};
static const uint8_t F16_PLUS[16] = {0,0,0,0,16,16,124,16,16,0,0,0,0,0,0,0};
static const uint8_t F16_EQ[16] = {0,0,0,0,0,124,0,124,0,0,0,0,0,0,0,0};
static const uint8_t F16_EXCL[16] = {0,0,16,16,16,16,16,16,16,0,16,16,0,0,0,0};
static const uint8_t F16_QUEST[16] = {0,0,56,68,4,8,16,16,0,0,16,16,0,0,0,0};
static const uint8_t F16_APOS[16] = {0,0,16,16,16,0,0,0,0,0,0,0,0,0,0,0};
static const uint8_t F16_QUOTE[16] = {0,0,40,40,40,0,0,0,0,0,0,0,0,0,0,0};
static const uint8_t F16_DOLLAR[16] = {0,16,60,84,80,80,56,20,20,84,60,16,0,0,0,0};
static const uint8_t F16_STAR[16] = {0,0,0,16,84,56,84,16,0,0,0,0,0,0,0,0};
static const uint8_t F16_HASH[16] = {0,0,40,40,124,40,40,124,40,40,0,0,0,0,0,0};
static const uint8_t F16_PCT[16] = {0,0,100,104,16,16,32,32,76,76,0,0,0,0,0,0};
static const uint8_t F16_AT[16] = {0,0,56,68,92,84,84,92,64,68,56,0,0,0,0,0};
static const uint8_t F16_AMP[16] = {0,0,48,72,72,48,116,88,72,52,0,0,0,0,0,0};
static const uint8_t F16_CARET[16] = {0,16,40,68,0,0,0,0,0,0,0,0,0,0,0,0};
static const uint8_t F16_TILDE[16] = {0,0,0,0,0,52,76,0,0,0,0,0,0,0,0,0};

static const uint8_t *glyph_for_char(char c) {
    if (c >= 'a' && c <= 'z') {
        c = (char)(c - ('a' - 'A'));
    }
    switch (c) {
        case 'A': return F16_A;
        case 'B': return F16_B;
        case 'C': return F16_C;
        case 'D': return F16_D;
        case 'E': return F16_E;
        case 'F': return F16_F;
        case 'G': return F16_G;
        case 'H': return F16_H;
        case 'I': return F16_I;
        case 'J': return F16_J;
        case 'K': return F16_K;
        case 'L': return F16_L;
        case 'M': return F16_M;
        case 'N': return F16_N;
        case 'O': return F16_O;
        case 'P': return F16_P;
        case 'Q': return F16_Q;
        case 'R': return F16_R;
        case 'S': return F16_S;
        case 'T': return F16_T;
        case 'U': return F16_U;
        case 'V': return F16_V;
        case 'W': return F16_W;
        case 'X': return F16_X;
        case 'Y': return F16_Y;
        case 'Z': return F16_Z;
        case '0': return F16_0;
        case '1': return F16_1;
        case '2': return F16_2;
        case '3': return F16_3;
        case '4': return F16_4;
        case '5': return F16_5;
        case '6': return F16_6;
        case '7': return F16_7;
        case '8': return F16_8;
        case '9': return F16_9;
        case '.': return F16_DOT;
        case ',': return F16_COMMA;
        case ':': return F16_COLON;
        case ';': return F16_SEMI;
        case '-': return F16_DASH;
        case '_': return F16_USCORE;
        case '/': return F16_SLASH;
        case '\\': return F16_BSLASH;
        case '(': return F16_LPAREN;
        case ')': return F16_RPAREN;
        case '<': return F16_LT;
        case '>': return F16_GT;
        case '|': return F16_PIPE;
        case '+': return F16_PLUS;
        case '=': return F16_EQ;
        case '!': return F16_EXCL;
        case '?': return F16_QUEST;
        case '\'': return F16_APOS;
        case '"': return F16_QUOTE;
        case '$': return F16_DOLLAR;
        case '*': return F16_STAR;
        case '#': return F16_HASH;
        case '%': return F16_PCT;
        case '@': return F16_AT;
        case '&': return F16_AMP;
        case '^': return F16_CARET;
        case '~': return F16_TILDE;
        case '[': return F16_LPAREN;
        case ']': return F16_RPAREN;
        default: return F16_SPACE;
    }
}

static void draw_glyph(struct framebuffer *fb, int x, int y, char c, uint32_t color, int scale) {
    const uint8_t *glyph = glyph_for_char(c);
    int row;
    int col;
    int sy;
    int sx;

    for (row = 0; row < 16; ++row) {
        for (col = 0; col < 8; ++col) {
            if ((glyph[row] >> (7 - col)) & 1u) {
                for (sy = 0; sy < scale; ++sy) {
                    for (sx = 0; sx < scale; ++sx) {
                        fb_put_pixel(fb, x + (col * scale) + sx, y + (row * scale) + sy, color);
                    }
                }
            }
        }
    }
}

static void draw_glyph_fast(struct framebuffer *fb, int x, int y, char c, uint32_t color, int scale) {
    const uint8_t *glyph = glyph_for_char(c);
    int row;
    int col;
    int glyph_width = 8 * scale;
    int glyph_height = 16 * scale;
    int x1 = x;
    int y1 = y;
    int x2 = x + glyph_width;
    int y2 = y + glyph_height;
    uint32_t *row_ptr;
    int pixel_x;
    int pixel_y;
    int sy;
    int sx;
    int base_x;

    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > (int)fb->width) x2 = (int)fb->width;
    if (y2 > (int)fb->height) y2 = (int)fb->height;

    if (fb->clip_enabled) {
        if (x1 < fb->clip_x) x1 = fb->clip_x;
        if (y1 < fb->clip_y) y1 = fb->clip_y;
        if (x2 > fb->clip_x + fb->clip_width) x2 = fb->clip_x + fb->clip_width;
        if (y2 > fb->clip_y + fb->clip_height) y2 = fb->clip_y + fb->clip_height;
    }

    if (x1 >= x2 || y1 >= y2) return;

    for (row = 0; row < 16; ++row) {
        uint8_t row_bits = glyph[row];
        for (col = 0; col < 8; ++col) {
            if ((row_bits >> (7 - col)) & 1u) {
                pixel_y = y + row * scale;
                for (sy = 0; sy < scale; ++sy) {
                    int actual_y = pixel_y + sy;
                    if (actual_y < y1 || actual_y >= y2) continue;
                    row_ptr = (uint32_t *)((uint8_t *)fb->base + ((uint32_t)actual_y * fb->pitch));
                    pixel_x = x + col * scale;
                    base_x = pixel_x;
                    for (sx = 0; sx < scale; ++sx) {
                        int actual_x = base_x + sx;
                        if (actual_x >= x1 && actual_x < x2) {
                            row_ptr[actual_x] = color;
                        }
                    }
                }
            }
        }
    }
}

static int glyph_coverage_bit(const uint8_t *glyph, int col, int row) {
    if (col < 0 || col > 7 || row < 0 || row > 15) {
        return 0;
    }
    return (glyph[row] >> (7 - col)) & 1u ? 255 : 0;
}

/* Bilinearly sample the 7x9 1-bit glyph at fixed-point source coordinate
 * (sx_fp, sy_fp) (8 fractional bits) and return coverage 0..255. This turns
 * the hard-edged bitmap into a smooth, anti-aliased shape when upscaled. */
static int glyph_sample_aa(const uint8_t *glyph, int sx_fp, int sy_fp) {
    int x0 = sx_fp >> 8;
    int y0 = sy_fp >> 8;
    int fx = sx_fp & 255;
    int fy = sy_fp & 255;
    int v00 = glyph_coverage_bit(glyph, x0, y0);
    int v01 = glyph_coverage_bit(glyph, x0 + 1, y0);
    int v10 = glyph_coverage_bit(glyph, x0, y0 + 1);
    int v11 = glyph_coverage_bit(glyph, x0 + 1, y0 + 1);
    int top = (v00 * (256 - fx) + v01 * fx) >> 8;
    int bottom = (v10 * (256 - fx) + v11 * fx) >> 8;
    return (top * (256 - fy) + bottom * fy) >> 8;
}

/* Supersampled, anti-aliased glyph rendering. Each output pixel averages
 * GLYPH_SS x GLYPH_SS bilinear samples of the 1-bit master, smoothing the hard
 * bitmap edges into grayscale coverage even at 1:1 size. */
#define GLYPH_SS 3

static void draw_glyph_aa(struct framebuffer *fb, int x, int y, char c, uint32_t color, int scale) {
    const uint8_t *glyph = glyph_for_char(c);
    int out_w = 8 * scale;
    int out_h = 16 * scale;
    int denom = 2 * scale * GLYPH_SS;
    int ox;
    int oy;
    int si;
    int sj;

    for (oy = 0; oy < out_h; ++oy) {
        for (ox = 0; ox < out_w; ++ox) {
            int acc = 0;
            for (sj = 0; sj < GLYPH_SS; ++sj) {
                int sy_fp = (((2 * (oy * GLYPH_SS + sj) + 1) << 8) / denom) - 128;
                for (si = 0; si < GLYPH_SS; ++si) {
                    int sx_fp = (((2 * (ox * GLYPH_SS + si) + 1) << 8) / denom) - 128;
                    acc += glyph_sample_aa(glyph, sx_fp, sy_fp);
                }
            }
            acc /= (GLYPH_SS * GLYPH_SS);
            if (acc > 0) {
                if (acc > 255) acc = 255;
                fb_blend_pixel(fb, x + ox, y + oy, color, (uint8_t)acc);
            }
        }
    }
}

/* ---- Anti-aliased proportional text from the pre-baked TrueType atlas ---- */
static int atlas_idx(int scale) {
    int i = scale - 1;
    if (i < 0) i = 0;
    if (i >= ATLAS_SIZES) i = ATLAS_SIZES - 1;
    return i;
}

/* Blit one atlas glyph; (x,y) is the text-line top-left, baseline derived from
 * the size's ascent. Returns the proportional advance. */
static int draw_atlas_glyph(struct framebuffer *fb, int x, int y, unsigned char c, uint32_t color, int idx) {
    const struct atlas_glyph *g;
    int baseline = y + FONT_ATLAS_ASCENT[idx];
    int gx, gy;

    if (c < ATLAS_FIRST || c >= ATLAS_FIRST + ATLAS_COUNT) {
        return FONT_ATLAS_SPACE[idx];
    }
    g = &FONT_ATLAS[idx][c - ATLAS_FIRST];
    if (g->cov) {
        for (gy = 0; gy < g->h; ++gy) {
            for (gx = 0; gx < g->w; ++gx) {
                uint8_t a = g->cov[gy * g->w + gx];
                if (a) fb_blend_pixel(fb, x + g->xoff + gx, baseline + g->yoff + gy, color, a);
            }
        }
    }
    return g->advance;
}

/* Draw a single glyph in a fixed monospace cell (terminal/CLI): the glyph is
 * centred horizontally in the cell so columns stay aligned. */
void draw_glyph_mono(struct framebuffer *fb, int x, int y, char c, uint32_t color, int scale) {
    int idx = atlas_idx(scale);
    int cell = FONT_ATLAS_CELLW[idx];
    const struct atlas_glyph *g;
    int pad = 0;
    if ((unsigned char)c < ATLAS_FIRST || (unsigned char)c >= ATLAS_FIRST + ATLAS_COUNT) return;
    g = &FONT_ATLAS[idx][(unsigned char)c - ATLAS_FIRST];
    if (g->advance < cell) pad = (cell - g->advance) / 2;
    (void)draw_atlas_glyph(fb, x + pad, y, (unsigned char)c, color, idx);
}

void draw_text(struct framebuffer *fb, int x, int y, const char *text, uint32_t color, int scale) {
    int idx = atlas_idx(scale);
    int cursor_x = x;
    int cursor_y = y;
    int line_h = FONT_ATLAS_LINEH[idx];

    while (*text != '\0') {
        if (*text == '\n') {
            cursor_x = x;
            cursor_y += line_h;
        } else {
            cursor_x += draw_atlas_glyph(fb, cursor_x, cursor_y, (unsigned char)*text, color, idx);
        }
        ++text;
    }
}

int text_line_height(int scale) {
    return FONT_ATLAS_LINEH[atlas_idx(scale)];
}

/* Fixed cell width — used by the terminal and for coarse layout estimates. */
int text_char_advance(int scale) {
    return FONT_ATLAS_CELLW[atlas_idx(scale)];
}

/* Proportional pixel width of a string (for centring / context-menu sizing). */
int text_width(const char *text, int scale) {
    int idx = atlas_idx(scale);
    int w = 0;
    while (*text != '\0' && *text != '\n') {
        unsigned char c = (unsigned char)*text++;
        if (c < ATLAS_FIRST || c >= ATLAS_FIRST + ATLAS_COUNT) { w += FONT_ATLAS_SPACE[idx]; continue; }
        w += FONT_ATLAS[idx][c - ATLAS_FIRST].advance;
    }
    return w;
}

/* Render a string into a caller-supplied 32-bit ARGB buffer (no pitch padding)
 * using the shared atlas — this is what userspace apps reach through
 * SYS_TEXT_DRAW so vexui widgets get the same anti-aliased glyphs as the chrome.
 * Out-of-bounds pixels are clipped by fb_put_pixel. Returns the advance width. */
int draw_text_to_argb(uint32_t *buf, int buf_w, int buf_h, int x, int y,
                      const char *text, uint32_t color, int scale) {
    struct framebuffer fb;
    if (buf == 0 || buf_w <= 0 || buf_h <= 0) {
        return 0;
    }
    fb.base = buf;
    fb.width = (uint32_t)buf_w;
    fb.height = (uint32_t)buf_h;
    fb.pitch = (uint32_t)buf_w * 4u;
    fb.bpp = 32;
    fb.clip_enabled = 0;
    fb.clip_x = fb.clip_y = fb.clip_width = fb.clip_height = 0;
    draw_text(&fb, x, y, text, color, scale);
    return text_width(text, scale);
}

/* Packed atlas metrics for a size: lineh | ascent<<8 | cellw<<16 | space<<24.
 * Each field fits in a byte (max glyph box is well under 256px). */
uint32_t font_metrics_packed(int scale) {
    int i = atlas_idx(scale);
    return (uint32_t)FONT_ATLAS_LINEH[i]
         | ((uint32_t)FONT_ATLAS_ASCENT[i] << 8)
         | ((uint32_t)FONT_ATLAS_CELLW[i] << 16)
         | ((uint32_t)FONT_ATLAS_SPACE[i] << 24);
}
