#include "multiboot2.h"

int multiboot2_find_framebuffer(uintptr_t mbi_addr, struct boot_framebuffer *out_fb) {
    struct multiboot2_info *info;
    uintptr_t cursor;

    if (mbi_addr == 0 || out_fb == 0) {
        return 0;
    }

    info = (struct multiboot2_info *)mbi_addr;
    cursor = mbi_addr + 8;

    while (cursor < mbi_addr + info->total_size) {
        struct multiboot2_tag *tag = (struct multiboot2_tag *)cursor;

        if (tag->type == MULTIBOOT2_TAG_TYPE_END) {
            break;
        }

        if (tag->type == MULTIBOOT2_TAG_TYPE_FRAMEBUFFER) {
            struct multiboot2_framebuffer_tag *fb_tag = (struct multiboot2_framebuffer_tag *)tag;
            out_fb->address = (uintptr_t)fb_tag->framebuffer_addr;
            out_fb->pitch = fb_tag->framebuffer_pitch;
            out_fb->width = fb_tag->framebuffer_width;
            out_fb->height = fb_tag->framebuffer_height;
            out_fb->bpp = fb_tag->framebuffer_bpp;
            return 1;
        }

        cursor += (tag->size + 7u) & ~7u;
    }

    return 0;
}

