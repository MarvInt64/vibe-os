#ifndef VIBEOS_MULTIBOOT2_H
#define VIBEOS_MULTIBOOT2_H

#include "types.h"

#define MULTIBOOT2_BOOTLOADER_MAGIC 0x36d76289u
#define MULTIBOOT2_TAG_TYPE_END 0u
#define MULTIBOOT2_TAG_TYPE_MMAP 6u
#define MULTIBOOT2_TAG_TYPE_FRAMEBUFFER 8u
#define MULTIBOOT2_MEMORY_AVAILABLE 1u

struct multiboot2_info {
    uint32_t total_size;
    uint32_t reserved;
};

struct multiboot2_tag {
    uint32_t type;
    uint32_t size;
};

struct multiboot2_framebuffer_tag {
    uint32_t type;
    uint32_t size;
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t framebuffer_bpp;
    uint8_t framebuffer_type;
    uint16_t reserved;
};

struct multiboot2_mmap_tag {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
};

struct multiboot2_mmap_entry {
    uint64_t base_addr;
    uint64_t length;
    uint32_t type;
    uint32_t reserved;
};

struct boot_framebuffer {
    uintptr_t address;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint8_t bpp;
};

struct boot_memory_info {
    uint64_t available_bytes;
    uint64_t highest_available_end;
    uint64_t heap_region_base;
    uint64_t heap_region_end;
};

int multiboot2_find_framebuffer(uintptr_t mbi_addr, struct boot_framebuffer *out_fb);
int multiboot2_memory_info(uintptr_t mbi_addr, uintptr_t min_heap_start, struct boot_memory_info *out_mem);

#endif
