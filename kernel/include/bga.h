#ifndef VIBEOS_BGA_H
#define VIBEOS_BGA_H

#include "multiboot2.h"
#include "types.h"

#define BGA_LFB_FALLBACK_PHYS_ADDR 0xFD000000u
#define BGA_DEFAULT_WIDTH 1920u
#define BGA_DEFAULT_HEIGHT 1080u
#define BGA_DEFAULT_BPP 32u

int bga_init_framebuffer(struct boot_framebuffer *out_fb, uint32_t width, uint32_t height, uint32_t bpp);

#endif
