/*
 * MIT License
 *
 * Copyright (c) 2026 Marvin Kicha
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "ramdisk.h"
#include "alloc.h"
#include "string.h"
#include "io.h"
#include "serial.h"

#include <stddef.h>

int ramdisk_init(struct ramdisk_device *dev, uint64_t size_mb) {
    dev->block_size = 4096;
    dev->block_count = (size_mb * 1024 * 1024) / dev->block_size;
    dev->size = dev->block_count * dev->block_size;
    
    dev->data = (uint8_t *)kmalloc(dev->size);
    if (!dev->data) {
        return -1;
    }
    
    memset(dev->data, 0, dev->size);
    return 0;
}

void ramdisk_destroy(struct ramdisk_device *dev) {
    if (dev->data) {
        kfree(dev->data);
        dev->data = NULL;
    }
    dev->size = 0;
    dev->block_count = 0;
}

int ramdisk_read(struct ramdisk_device *dev, uint64_t block_num, void *buffer, size_t count) {
	if (!dev->read_fn) {
		uint64_t offset = block_num * dev->block_size;
		if (block_num >= dev->block_count) return -1;
		memcpy(buffer, dev->data + offset, count);
		return 0;
	}
	
	return dev->read_fn(dev->io_context, block_num, buffer, count);

	/* Otherwise, use memory-backed storage */
	uint64_t offset;

	if (block_num >= dev->block_count) {
		return -1;
	}

	offset = block_num * dev->block_size;
	memcpy(buffer, dev->data + offset, count);
	return 0;
}

int ramdisk_write(struct ramdisk_device *dev, uint64_t block_num, const void *buffer, size_t count) {
    /* If device has custom write function (e.g., IDE disk), use it */
    if (dev->write_fn) {
        return dev->write_fn(dev->io_context, block_num, buffer, count);
    }
    
    /* Otherwise, use memory-backed storage */
    uint64_t offset;

    if (block_num >= dev->block_count) {
        return -1;
    }

    offset = block_num * dev->block_size;
    memcpy(dev->data + offset, buffer, count);
    return 0;
}
