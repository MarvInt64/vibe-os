#ifndef VIBEOS_RAMDISK_H
#define VIBEOS_RAMDISK_H

#include "types.h"
#include <stddef.h>

struct ramdisk_device {
    uint8_t *data;           /* Memory pointer (for RAM disk) */
    uint64_t size;
    uint32_t block_size;
    uint64_t block_count;
    /* Function pointers for generic block I/O (NULL for RAM disk) */
    int (*read_fn)(void *ctx, uint64_t block_num, void *buffer, size_t count);
    int (*write_fn)(void *ctx, uint64_t block_num, const void *buffer, size_t count);
    void *io_context;        /* Context for read/write functions (e.g., IDE device) */
};

int ramdisk_init(struct ramdisk_device *dev, uint64_t size_mb);
void ramdisk_destroy(struct ramdisk_device *dev);
int ramdisk_read(struct ramdisk_device *dev, uint64_t block_num, void *buffer, size_t count);
int ramdisk_write(struct ramdisk_device *dev, uint64_t block_num, const void *buffer, size_t count);

#endif
