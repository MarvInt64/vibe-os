#ifndef VIBEOS_RAMDISK_DEMO_H
#define VIBEOS_RAMDISK_DEMO_H

#include "types.h"
#include "ramdisk.h"

/* Size of static RAM disk for demo (1MB) */
#define RAMDISK_SIZE (1 * 1024 * 1024)

/* Initialize static RAM disk for filesystem demo */
void ramdisk_demo_init(void);

/* Get pointer to static RAM disk device */
struct ramdisk_device *ramdisk_demo_get_device(void);

#endif
