#ifndef VIBEOS_RENDER_H
#define VIBEOS_RENDER_H

#include "framebuffer.h"

void draw_gradient_background(struct framebuffer *fb, uint32_t top, uint32_t bottom);
void draw_text(struct framebuffer *fb, int x, int y, const char *text, uint32_t color, int scale);
void draw_glyph_mono(struct framebuffer *fb, int x, int y, char c, uint32_t color, int scale);
int text_line_height(int scale);
int text_char_advance(int scale);   /* fixed monospace cell width */
int text_width(const char *text, int scale);   /* proportional string width */
void draw_rounded_panel(struct framebuffer *fb, int x, int y, int width, int height, int radius, uint32_t top, uint32_t bottom, uint32_t border, uint32_t highlight);
void draw_soft_shadow(struct framebuffer *fb, int x, int y, int width, int height, int radius, int spread, uint32_t color);
void draw_panel(struct framebuffer *fb, int x, int y, int width, int height, uint32_t color, uint32_t border);
void draw_shadow(struct framebuffer *fb, int x, int y, int width, int height, int spread, uint32_t color);

#endif
