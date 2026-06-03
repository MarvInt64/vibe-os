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

/* 
 * Static RAM Disk demo for filesystem testing
 * This uses static memory instead of heap for testing purposes
 */

#include "ramdisk.h"
#include "types.h"
#include "serial.h"
#include "string.h"

#include <stddef.h>

#define RAMDISK_SIZE (1024 * 1024)

static uint8_t g_ramdisk_storage[RAMDISK_SIZE];
static struct ramdisk_device g_ramdisk_dev;

/*
 * Initialize static RAM disk for filesystem demo
 */
void ramdisk_demo_init(void) {
    g_ramdisk_dev.data = g_ramdisk_storage;
    g_ramdisk_dev.size = RAMDISK_SIZE;
    g_ramdisk_dev.block_size = 4096;
    g_ramdisk_dev.block_count = RAMDISK_SIZE / 4096;
    g_ramdisk_dev.read_fn = NULL;
    g_ramdisk_dev.write_fn = NULL;
    g_ramdisk_dev.io_context = NULL;
    
    serial_write("RAMDISK: Initialized static 1MB RAM disk at ");
    serial_write_hex_u64((uint64_t)g_ramdisk_storage);
    serial_write("\n");
}

/*
 * Get pointer to static RAM disk device
 */
struct ramdisk_device *ramdisk_demo_get_device(void) {
    return &g_ramdisk_dev;
}
