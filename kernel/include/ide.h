#ifndef VIBEOS_IDE_H
#define VIBEOS_IDE_H

#include "types.h"
#include <stddef.h>

/* IDE Controller Ports */
#define IDE_PRIMARY_DATA        0x1F0
#define IDE_PRIMARY_ERROR       0x1F1
#define IDE_PRIMARY_SECTOR_COUNT 0x1F2
#define IDE_PRIMARY_LBA_LOW     0x1F3
#define IDE_PRIMARY_LBA_MID     0x1F4
#define IDE_PRIMARY_LBA_HIGH    0x1F5
#define IDE_PRIMARY_DRIVE       0x1F6
#define IDE_PRIMARY_STATUS      0x1F7
#define IDE_PRIMARY_COMMAND     0x1F7
#define IDE_PRIMARY_ALT_STATUS  0x3F6
#define IDE_PRIMARY_CONTROL     0x3F6

/* IDE Commands */
#define IDE_CMD_READ_SECTORS    0x20
#define IDE_CMD_WRITE_SECTORS   0x30
#define IDE_CMD_IDENTIFY        0xEC
#define IDE_CMD_CACHE_FLUSH     0xE7

/* IDE Status Flags */
#define IDE_STATUS_BSY          0x80
#define IDE_STATUS_DRDY         0x40
#define IDE_STATUS_DF           0x20
#define IDE_STATUS_DRQ          0x08
#define IDE_STATUS_ERR          0x01

/* IDE Drive Select */
#define IDE_DRIVE_MASTER        0xA0
#define IDE_DRIVE_SLAVE         0xB0
#define IDE_DRIVE_LBA           0x40

struct ide_device {
    uint8_t present;
    uint8_t master;
    uint32_t sectors;
    uint32_t bytes_per_sector;
};

int ide_init(void);
int ide_read_sectors(uint32_t lba, uint8_t count, void *buffer);
int ide_write_sectors(uint32_t lba, uint8_t count, const void *buffer);
int ide_identify(struct ide_device *dev);

/* Block device interface for filesystem */
struct block_device {
    uint32_t block_size;
    uint64_t block_count;
    void *private_data;
    int (*read)(struct block_device *dev, uint64_t block_num, void *buffer, size_t count);
    int (*write)(struct block_device *dev, uint64_t block_num, const void *buffer, size_t count);
};

int ide_block_device_init(struct block_device *bdev);

#endif
