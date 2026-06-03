/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Marvin Kicha <https://github.com/MarvInt64/> */

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

int multiboot2_memory_info(uintptr_t mbi_addr, uintptr_t min_heap_start, struct boot_memory_info *out_mem) {
    struct multiboot2_info *info;
    uintptr_t cursor;
    uint64_t best_len = 0;

    if (mbi_addr == 0 || out_mem == 0) {
        return 0;
    }

    out_mem->available_bytes = 0;
    out_mem->highest_available_end = 0;
    out_mem->heap_region_base = 0;
    out_mem->heap_region_end = 0;

    info = (struct multiboot2_info *)mbi_addr;
    cursor = mbi_addr + 8;

    while (cursor < mbi_addr + info->total_size) {
        struct multiboot2_tag *tag = (struct multiboot2_tag *)cursor;

        if (tag->type == MULTIBOOT2_TAG_TYPE_END) {
            break;
        }

        if (tag->type == MULTIBOOT2_TAG_TYPE_MMAP) {
            struct multiboot2_mmap_tag *mmap = (struct multiboot2_mmap_tag *)tag;
            uintptr_t entry_cursor = cursor + sizeof(struct multiboot2_mmap_tag);
            uintptr_t entry_end = cursor + tag->size;

            if (mmap->entry_size < sizeof(struct multiboot2_mmap_entry)) {
                cursor += (tag->size + 7u) & ~7u;
                continue;
            }

            while (entry_cursor + sizeof(struct multiboot2_mmap_entry) <= entry_end) {
                struct multiboot2_mmap_entry *entry = (struct multiboot2_mmap_entry *)entry_cursor;
                if (entry->type == MULTIBOOT2_MEMORY_AVAILABLE && entry->length > 0) {
                    uint64_t base = entry->base_addr;
                    uint64_t end = entry->base_addr + entry->length;
                    out_mem->available_bytes += entry->length;
                    if (end > out_mem->highest_available_end) {
                        out_mem->highest_available_end = end;
                    }
                    if (end > min_heap_start) {
                        uint64_t heap_base = base > min_heap_start ? base : min_heap_start;
                        uint64_t heap_len = end - heap_base;
                        if (heap_len > best_len) {
                            best_len = heap_len;
                            out_mem->heap_region_base = heap_base;
                            out_mem->heap_region_end = end;
                        }
                    }
                }
                entry_cursor += mmap->entry_size;
            }
        }

        cursor += (tag->size + 7u) & ~7u;
    }

    return out_mem->heap_region_end > out_mem->heap_region_base;
}
