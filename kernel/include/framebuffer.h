#ifndef VIBEOS_FRAMEBUFFER_H
#define VIBEOS_FRAMEBUFFER_H

#include "types.h"

struct framebuffer {
    uint32_t *base;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint8_t bpp;
    uint8_t clip_enabled;
    int clip_x;
    int clip_y;
    int clip_width;
    int clip_height;
};

struct rect {
    int x;
    int y;
    int width;
    int height;
};

void fb_init(struct framebuffer *fb, uintptr_t address, uint32_t width, uint32_t height, uint32_t pitch, uint8_t bpp);
void fb_put_pixel(struct framebuffer *fb, int x, int y, uint32_t color);
void fb_clear(struct framebuffer *fb, uint32_t color);
void fb_fill_rect(struct framebuffer *fb, int x, int y, int width, int height, uint32_t color);
void fb_draw_rect(struct framebuffer *fb, int x, int y, int width, int height, int thickness, uint32_t color);
void fb_blit(struct framebuffer *dest, const struct framebuffer *src);
void fb_blit_rect(struct framebuffer *dest, const struct framebuffer *src, const struct rect *rect);
void fb_set_clip(struct framebuffer *fb, const struct rect *rect);
void fb_reset_clip(struct framebuffer *fb);

#endif
