#include <stdint.h>
#include "ide.h"
#include "io.h"
#include "serial.h"
#include <stddef.h>

static uint8_t ide_buffer[2048];
static struct ide_device ide_devices[2];

static void ide_delay(void) {
    /* Read alternate status port 4 times = ~400ns delay */
    inb(IDE_PRIMARY_ALT_STATUS);
    inb(IDE_PRIMARY_ALT_STATUS);
    inb(IDE_PRIMARY_ALT_STATUS);
    inb(IDE_PRIMARY_ALT_STATUS);
}

static uint8_t ide_wait_ready(void) {
    uint8_t status;
    int timeout = 100000;
    while (timeout-- > 0) {
        status = inb(IDE_PRIMARY_STATUS);
        if (!(status & IDE_STATUS_BSY)) {
            return 0;
        }
    }
    serial_write("IDE: Wait ready timeout, status=");
    serial_write_hex_u64(status);
    serial_write("\n");
    return 1;
}

static uint8_t ide_poll(void) {
    uint8_t status;
    
    /* Wait for BSY to clear and DRQ or ERR to set */
    while (1) {
        status = inb(IDE_PRIMARY_STATUS);
        if (!(status & IDE_STATUS_BSY)) {
            break;
        }
    }
    
    if (status & IDE_STATUS_ERR) {
        serial_write("IDE: Error status detected\n");
        return 1;
    }
    
    if (status & IDE_STATUS_DF) {
        serial_write("IDE: Drive fault detected\n");
        return 1;
    }
    
    return 0;
}

int ide_init(void) {
    serial_write("IDE: Initializing...\n");
    
    /* Disable interrupts on primary controller */
    outb(IDE_PRIMARY_CONTROL, 0x02);
    
    /* Wait for drive to be ready */
    ide_delay();
    
    /* Reset drives */
    outb(IDE_PRIMARY_DRIVE, IDE_DRIVE_MASTER);
    ide_delay();
    
    /* Check if drives exist */
    outb(IDE_PRIMARY_SECTOR_COUNT, 0x55);
    outb(IDE_PRIMARY_LBA_LOW, 0xAA);
    
    uint8_t sc = inb(IDE_PRIMARY_SECTOR_COUNT);
    uint8_t lbal = inb(IDE_PRIMARY_LBA_LOW);
    
    if (sc == 0x55 && lbal == 0xAA) {
        /* Primary controller exists */
        serial_write("IDE: Primary controller detected\n");
        
        /* Try to identify master */
        if (ide_identify(&ide_devices[0]) == 0) {
            serial_write("IDE: Master drive detected\n");
        } else {
            serial_write("IDE: No master drive\n");
        }
    } else {
        serial_write("IDE: No primary controller\n");
    }
    
    return 0;
}

int ide_identify(struct ide_device *dev) {
    uint8_t status;
    int i;
    
    /* Select master drive */
    outb(IDE_PRIMARY_DRIVE, IDE_DRIVE_MASTER);
    ide_delay();
    
    /* Send IDENTIFY command */
    outb(IDE_PRIMARY_COMMAND, IDE_CMD_IDENTIFY);
    ide_delay();
    
    status = inb(IDE_PRIMARY_STATUS);
    if (status == 0) {
        /* No drive */
        return -1;
    }
    
    /* Wait for BSY to clear */
    while (status & IDE_STATUS_BSY) {
        status = inb(IDE_PRIMARY_STATUS);
    }
    
    /* Check if this is an ATA device */
    uint8_t lba_mid = inb(IDE_PRIMARY_LBA_MID);
    uint8_t lba_high = inb(IDE_PRIMARY_LBA_HIGH);
    
    if (lba_mid != 0 || lba_high != 0) {
        /* Not an ATA device (probably ATAPI) */
        serial_write("IDE: Not an ATA device\n");
        return -1;
    }
    
    /* Wait for data ready */
    if (ide_poll() != 0) {
        serial_write("IDE: Identify poll failed\n");
        return -1;
    }
    
    /* Read 256 words of identification data */
    for (i = 0; i < 256; i++) {
        ((uint16_t*)ide_buffer)[i] = inw(IDE_PRIMARY_DATA);
    }
    
    /* Parse identification data */
    dev->present = 1;
    dev->master = 1;
    dev->bytes_per_sector = 512;
    
    /* Total number of sectors (LBA28) - words 60-61 */
    dev->sectors = *(uint32_t*)(ide_buffer + 120);
    
    serial_write("IDE: Drive has ");
    serial_write_hex_u64(dev->sectors);
    serial_write(" sectors\n");
    
    return 0;
}

int ide_read_sectors(uint32_t lba, uint8_t count, void *buffer) {
    uint8_t *buf = (uint8_t*)buffer;
    int i;
    uint8_t status;
    uint8_t sectors_read = 0;

    if (count == 0 || count > 255) {
        serial_write("IDE: Invalid sector count provided.\n");
        return -1;
    }
    
    /* Select master drive with LBA first */
    outb(IDE_PRIMARY_DRIVE, IDE_DRIVE_MASTER | IDE_DRIVE_LBA | ((lba >> 24) & 0x0F));
    ide_delay();
    ide_delay();
    
    /* Wait for drive to be ready */
    if (ide_wait_ready() != 0) { 
        serial_write("IDE: Drive not ready after wait.\n");
        return -1;
    }
    
    /* Setup transfer */
    outb(IDE_PRIMARY_SECTOR_COUNT, count);
    outb(IDE_PRIMARY_LBA_LOW, lba & 0xFF);
    outb(IDE_PRIMARY_LBA_MID, (lba >> 8) & 0xFF);
    outb(IDE_PRIMARY_LBA_HIGH, (lba >> 16) & 0xFF);
    
    /* Send read command */
    outb(IDE_PRIMARY_COMMAND, IDE_CMD_READ_SECTORS);
    
    /* Read each sector */
    while (sectors_read < count) {
        /* Wait for data ready */
        if (ide_poll() != 0) {
            serial_write("IDE: ide_poll failed before DRQ.\n");
            return -1;
        }
        
        /* Check if data is ready */
        status = inb(IDE_PRIMARY_STATUS);
        if (!(status & IDE_STATUS_DRQ)) {
            serial_write("IDE: Drive not ready for data (DRQ not set).\n");
            return -1;
        }
        
        /* Read 256 words (512 bytes) */
        for (i = 0; i < 256; i++) {
            ((uint16_t*)buf)[i] = inw(IDE_PRIMARY_DATA);
        }
        
        buf += 512;
        sectors_read++;
    }

    return 0;
}

int ide_write_sectors(uint32_t lba, uint8_t count, const void *buffer) {
    const uint8_t *buf = (const uint8_t*)buffer;
    int i;
    
    if (count == 0 || count > 255) {
        serial_write("IDE: Invalid sector count for write\n");
        return -1;
    }
    
    /* Wait for drive to be ready */
    ide_wait_ready();
    
    /* Select master drive with LBA */
    outb(IDE_PRIMARY_DRIVE, IDE_DRIVE_MASTER | IDE_DRIVE_LBA | ((lba >> 24) & 0x0F));
    ide_delay();
    
    /* Setup transfer */
    outb(IDE_PRIMARY_SECTOR_COUNT, count);
    outb(IDE_PRIMARY_LBA_LOW, lba & 0xFF);
    outb(IDE_PRIMARY_LBA_MID, (lba >> 8) & 0xFF);
    outb(IDE_PRIMARY_LBA_HIGH, (lba >> 16) & 0xFF);
    
    /* Send write command */
    outb(IDE_PRIMARY_COMMAND, IDE_CMD_WRITE_SECTORS);
    
    uint8_t sectors_written = 0;
    /* Write each sector */
    while (sectors_written < count) {
        /* Wait for data ready */
        if (ide_poll() != 0) {
            serial_write("IDE: Poll failed before DRQ for write\n");
            return -1;
        }
        
        /* Check if drive is ready for data */
        uint8_t status = inb(IDE_PRIMARY_STATUS);
        if (!(status & IDE_STATUS_DRQ)) {
            serial_write("IDE: Drive not ready for data (DRQ not set) for write\n");
            return -1;
        }
        
        /* Write 256 words (512 bytes) */
        for (i = 0; i < 256; i++) {
            outw(IDE_PRIMARY_DATA, ((const uint16_t*)buf)[i]);
        }
        
        buf += 512;
        sectors_written++;
    }
    
    /* Flush cache */
    outb(IDE_PRIMARY_COMMAND, IDE_CMD_CACHE_FLUSH);
    if (ide_poll() != 0) {
        serial_write("IDE: Poll failed after cache flush\n");
        return -1;
    }
    
    return 0;
}

/* Block device interface implementation */
static struct ide_device *ide_block_private = NULL;

static int ide_block_read(struct block_device *bdev, uint64_t block_num, void *buffer, size_t count) {
	(void)bdev;

	/* Convert count to sectors (count bytes / 512 bytes per sector) */
	uint32_t sectors_u32 = (count + 511) / 512;
	uint8_t sectors = (uint8_t)sectors_u32; /* Cast to uint8_t as per ide_read_sectors argument */
	if (sectors_u32 > 255) { /* Check against uint32_t before casting to uint8_t */
		sectors = 255;
	}

	return ide_read_sectors((uint32_t)block_num, sectors, buffer);
}

static int ide_block_write(struct block_device *bdev, uint64_t block_num, const void *buffer, size_t count) {
    (void)bdev;
    
    uint32_t sectors_u32 = (count + 511) / 512;
    uint8_t sectors = (uint8_t)sectors_u32;
    if (sectors_u32 > 255) {
        sectors = 255;
    }
    
    return ide_write_sectors((uint32_t)block_num, sectors, buffer);
}

int ide_block_device_init(struct block_device *bdev) {
    if (ide_identify(&ide_devices[0]) != 0) {
        serial_write("IDE: ide_identify failed\n");
        return -1;
    }
    
    ide_block_private = &ide_devices[0];
    
    bdev->block_size = 512;
    bdev->block_count = ide_devices[0].sectors;
    bdev->private_data = ide_block_private;
    bdev->read = ide_block_read;
    bdev->write = ide_block_write;
    
    serial_write("IDE: Block device initialized with ");
    serial_write_hex_u64(bdev->block_count);
    serial_write(" blocks\n");
    
    return 0;
}
