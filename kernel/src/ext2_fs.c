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
 * Ext2-like Filesystem Implementation for VibeOS
 */

#include <stdint.h>      // Für uint8_t, uint32_t, uint64_t
#include "ramdisk.h"     // Für ramdisk_read/ramdisk_write
#include "ext2_fs.h"
#include "string.h"
#include "alloc.h"
#include "serial.h"

#define EXT2_ROOT_INODE 2

static uint8_t g_ext2_sb_buffer[1024] __attribute__((aligned(16)));

static int ext2_read_block(struct ext2_filesystem *fs, uint32_t block_num, void *buffer) {
    uint64_t byte_offset = (uint64_t)block_num * fs->superblock.block_size;
    return ramdisk_read(fs->device, byte_offset / fs->device->block_size, buffer, fs->superblock.block_size);
}

static int ext2_write_block(struct ext2_filesystem *fs, uint32_t block_num, const void *buffer) {
    uint64_t byte_offset = (uint64_t)block_num * fs->superblock.block_size;
    return ramdisk_write(fs->device, byte_offset / fs->device->block_size, buffer, fs->superblock.block_size);
}

static uint32_t ext2_alloc_block(struct ext2_filesystem *fs) {
    uint32_t i;
    uint32_t blocks_count = fs->superblock.blocks_count;
    uint32_t start_block = fs->superblock.first_data_block;
    if (start_block < 1) start_block = 1;

    for (i = start_block; i < blocks_count; i++) {
        uint8_t byte = fs->block_bitmap[i / 8];
        uint8_t bit_mask = 1 << (i % 8);

        if (!(byte & bit_mask)) {
            fs->block_bitmap[i / 8] |= bit_mask;
            fs->superblock.free_blocks--;
            return i;
        }
    }

    return 0;
}

static void ext2_free_block(struct ext2_filesystem *fs, uint32_t block_num) {
    if (block_num < fs->superblock.blocks_count) {
        fs->block_bitmap[block_num / 8] &= ~(1 << (block_num % 8));
        fs->superblock.free_blocks++;
    }
}

static uint32_t ext2_alloc_inode_num(struct ext2_filesystem *fs) {
    uint32_t i;
    uint32_t inodes_count = fs->superblock.inodes_count;
    /* Inodes 1-10 are reserved in standard ext2 */
    uint32_t start_inode = 11;

    for (i = start_inode; i < inodes_count; i++) {
        uint8_t byte = fs->inode_bitmap[i / 8];
        uint8_t bit_mask = 1 << (i % 8);

        if (!(byte & bit_mask)) {
            fs->inode_bitmap[i / 8] |= bit_mask;
            fs->superblock.free_inodes--;
            return i + 1; // Return 1-based inode number
        }
    }

    return 0;
}

static void ext2_free_inode_num(struct ext2_filesystem *fs, uint32_t inode_num) {
    if (inode_num > 0 && inode_num <= fs->superblock.inodes_count) {
        uint32_t i = inode_num - 1;
        fs->inode_bitmap[i / 8] &= ~(1 << (i % 8));
        fs->superblock.free_inodes++;
    }
}

int ext2_write_inode(struct ext2_filesystem *fs, uint32_t inode_num) {
    uint32_t inode_block_num;
    uint32_t inode_offset;
    uint8_t *block;
    uint32_t bb_blks;

    if (inode_num == 0 || inode_num > fs->superblock.inodes_count) return -1;

    /* it_start depends on the block-bitmap size which scales with blocks_count
     * (8 blocks of bitmap per 64k blocks). The previous `4 +` assumed a 1-block
     * bitmap (≤8k blocks); on 64MB disks (8 bitmap blocks, it_start=11) it
     * wrote inodes back to data blocks, silently corrupting /etc whenever a
     * later inode was created. */
    bb_blks = (fs->superblock.blocks_count + 8u * fs->superblock.block_size - 1u)
              / (8u * fs->superblock.block_size);
    inode_block_num = 2u + bb_blks + 1u
                      + ((inode_num - 1) * sizeof(struct ext2_inode)) / fs->superblock.block_size;
    inode_offset = ((inode_num - 1) * sizeof(struct ext2_inode)) % fs->superblock.block_size;

    block = (uint8_t *)kmalloc(fs->superblock.block_size);
    if (!block) return -1;

    ext2_read_block(fs, inode_block_num, block);
    memcpy(block + inode_offset, &fs->inode_table[inode_num - 1], sizeof(struct ext2_inode));
    ext2_write_block(fs, inode_block_num, block);

    kfree(block);
    return 0;
}

static int ext2_write_superblock(struct ext2_filesystem *fs) {
    uint8_t *block = (uint8_t *)kmalloc(fs->superblock.block_size);
    if (!block) return -1;

    memset(block, 0, fs->superblock.block_size);
    memcpy(block, &fs->superblock, sizeof(fs->superblock));

    /* Superblock is always at block 1 (byte 1024) */
    int result = ext2_write_block(fs, 1, block);
    kfree(block);
    return result;
}

static int ext2_write_bitmaps(struct ext2_filesystem *fs) {
    uint32_t bs  = fs->superblock.block_size;
    uint32_t bb_blks = (fs->superblock.blocks_count + 8u * bs - 1u) / (8u * bs);
    uint32_t ib_blk  = 2u + bb_blks;
    uint32_t block_bitmap_size = (fs->superblock.blocks_count + 7) / 8;
    uint32_t inode_bitmap_size = (fs->superblock.inodes_count  + 7) / 8;
    uint8_t *block = (uint8_t *)kmalloc(bs);
    uint32_t i;

    if (!block) return -1;

    /* Write block bitmap across as many blocks as needed. */
    for (i = 0; i < bb_blks; ++i) {
        uint32_t off = i * bs;
        uint32_t len = bs;
        if (off + len > block_bitmap_size) len = block_bitmap_size - off;
        memset(block, 0, bs);
        memcpy(block, fs->block_bitmap + off, len);
        ext2_write_block(fs, 2u + i, block);
    }

    /* Write inode bitmap. */
    memset(block, 0, bs);
    memcpy(block, fs->inode_bitmap, inode_bitmap_size);
    ext2_write_block(fs, ib_blk, block);

    kfree(block);
    return 0;
}

int ext2_format(struct ext2_filesystem *fs, struct ramdisk_device *device) {
    uint32_t block_bitmap_size;
    uint32_t inode_bitmap_size;
    uint32_t inode_table_size;
    uint32_t inode_table_blocks;

    memset(fs, 0, sizeof(*fs));
    fs->device = device;

    fs->superblock.magic = EXT2_MAGIC;
    fs->superblock.version = 1;
    fs->superblock.block_size = EXT2_BLOCK_SIZE;
    fs->superblock.blocks_count = device->size / EXT2_BLOCK_SIZE;
    fs->superblock.free_blocks = fs->superblock.blocks_count;
    fs->superblock.inodes_count = 1024; // Fixed count for simplicity
    if (fs->superblock.inodes_count > fs->superblock.blocks_count / 4)
        fs->superblock.inodes_count = fs->superblock.blocks_count / 4;
    
    fs->superblock.free_inodes = fs->superblock.inodes_count;
    
    inode_table_size = fs->superblock.inodes_count * sizeof(struct ext2_inode);
    inode_table_blocks = (inode_table_size + fs->superblock.block_size - 1) / fs->superblock.block_size;
    /* Layout: 0=boot 1=sb 2..2+bb_blks-1=block_bitmap ib_blk=inode_bitmap it_start..=inode_table */
    {
        uint32_t bb_blks = (fs->superblock.blocks_count + 8u * fs->superblock.block_size - 1u)
                           / (8u * fs->superblock.block_size);
        uint32_t ib_blk  = 2u + bb_blks;
        uint32_t it_start = ib_blk + 1u;
        fs->superblock.first_data_block = it_start + inode_table_blocks;
    }
    
    fs->superblock.blocks_per_group = 8192;
    fs->superblock.inodes_per_group = 2048;

    block_bitmap_size = (fs->superblock.blocks_count + 7) / 8;
    fs->block_bitmap = (uint8_t *)kmalloc(block_bitmap_size);
    if (!fs->block_bitmap) return -1;
    memset(fs->block_bitmap, 0, block_bitmap_size);

    inode_bitmap_size = (fs->superblock.inodes_count + 7) / 8;
    fs->inode_bitmap = (uint8_t *)kmalloc(inode_bitmap_size);
    if (!fs->inode_bitmap) {
        kfree(fs->block_bitmap);
        return -1;
    }
    memset(fs->inode_bitmap, 0, inode_bitmap_size);

    fs->inode_table = (struct ext2_inode *)kmalloc(inode_table_size);
    if (!fs->inode_table) {
        kfree(fs->block_bitmap);
        kfree(fs->inode_bitmap);
        return -1;
    }
    memset(fs->inode_table, 0, inode_table_size);

    /* Mark metadata blocks as used */
    for (uint32_t i = 0; i < fs->superblock.first_data_block; i++) {
        fs->block_bitmap[i / 8] |= (1 << (i % 8));
    }
    fs->superblock.free_blocks = fs->superblock.blocks_count - fs->superblock.first_data_block;

    /* Initialize root directory (Inode 2) */
    struct ext2_inode *root_inode = &fs->inode_table[EXT2_ROOT_INODE - 1];
    root_inode->mode = EXT2_S_IFDIR | 0755;
    root_inode->uid = 0;
    root_inode->gid = 0;
    root_inode->size = fs->superblock.block_size;
    root_inode->links_count = 2;
    
    fs->inode_bitmap[0] |= (1 << (EXT2_ROOT_INODE - 1));
    fs->superblock.free_inodes--;

    uint32_t root_block = ext2_alloc_block(fs);
    root_inode->block[0] = root_block;
    root_inode->blocks = fs->superblock.block_size / 512;

    uint8_t *dir_block = (uint8_t *)kmalloc(fs->superblock.block_size);
    memset(dir_block, 0, fs->superblock.block_size);
    
    struct ext2_dir_entry *dot = (struct ext2_dir_entry *)dir_block;
    dot->inode = EXT2_ROOT_INODE;
    dot->name_len = 1;
    dot->file_type = 2;
    dot->name[0] = '.';
    dot->rec_len = 12;
    
    struct ext2_dir_entry *dotdot = (struct ext2_dir_entry *)(dir_block + 12);
    dotdot->inode = EXT2_ROOT_INODE;
    dotdot->name_len = 2;
    dotdot->file_type = 2;
    dotdot->name[0] = '.';
    dotdot->name[1] = '.';
    dotdot->rec_len = fs->superblock.block_size - 12;
    
    ext2_write_block(fs, root_block, dir_block);
    kfree(dir_block);

    ext2_write_superblock(fs);
    ext2_write_bitmaps(fs);
    {
        uint32_t bb_blks_f = (fs->superblock.blocks_count + 8u * fs->superblock.block_size - 1u)
                              / (8u * fs->superblock.block_size);
        uint32_t it_start_f = 2u + bb_blks_f + 1u;
        for (uint32_t i = 0; i < inode_table_blocks; i++) {
            ext2_write_block(fs, it_start_f + i,
                             (uint8_t*)fs->inode_table + i * fs->superblock.block_size);
        }
    }

    /* Create basic directories */
    ext2_mkdir(fs, "/home", 0755);
    ext2_mkdir(fs, "/bin", 0755);
    ext2_mkdir(fs, "/etc", 0755);

    /* Seed /etc with the system identity files so they exist from the very
     * first boot — id/whoami/su look at /etc/passwd and would fall back to
     * "user" forever otherwise. The content is hard-coded here (no host-side
     * install needed); any later `edit /etc/passwd` and Save overwrites it
     * because the inode is on the persistent ext2 partition. */
    {
        static const char seed_passwd[] = "root:x:0:0:Root:/root:/bin/sh\n";
        static const char seed_group[]  = "root:x:0:\n";
        static const char seed_host[]   = "vibeos\n";
        static const char seed_issue[]  = "VibeOS \\r (kernel \\v)\\n\\n";

        struct { const char *path; const char *data; uint16_t mode; } seeds[] = {
            { "/etc/passwd", seed_passwd, 0644 },
            { "/etc/group",  seed_group,  0644 },
            { "/etc/hostname", seed_host, 0644 },
            { "/etc/issue",  seed_issue,  0644 },
        };
        for (size_t i = 0; i < sizeof(seeds) / sizeof(seeds[0]); ++i) {
            uint32_t ino = ext2_create(fs, seeds[i].path, seeds[i].mode);
            if (ino != 0) {
                ext2_write(fs, ino, 0, strlen(seeds[i].data), seeds[i].data);
            }
        }
    }

    return 0;
}

int ext2_mount(struct ext2_filesystem *fs, struct ramdisk_device *device) {
    uint8_t *block;
    struct ext2_superblock *sb;
    uint32_t block_bitmap_size;
    uint32_t inode_bitmap_size;
    uint32_t inode_table_size;
    uint64_t sb_block;
    int is_ext2;
    int read_result;

    serial_write("EXT2: Starting mount...\n");
    memset(fs, 0, sizeof(*fs));
    fs->device = device;

    block = g_ext2_sb_buffer;

    sb_block = 1024 / device->block_size;
    if (sb_block == 0) sb_block = 1;

    serial_write("EXT2: Reading superblock from block ");
    serial_write_hex_u64(sb_block);
    serial_write(" count=");
    serial_write_hex_u64(EXT2_BLOCK_SIZE);
    serial_write("\n");

    serial_write("EXT2: Calling ramdisk_read (dev=");
    serial_write_hex_u64((uint64_t)device);
    serial_write(")...\n");

    serial_write("EXT2: device->read_fn=");
    serial_write_hex_u64((uint64_t)device->read_fn);
    serial_write("\n");

    read_result = ramdisk_read(device, sb_block, block, EXT2_BLOCK_SIZE);
    serial_write("EXT2: back from ramdisk_read\n");
    serial_write("EXT2: ramdisk_read result=");
    serial_write_hex_u64(read_result);
    serial_write(" first32=");
    serial_write_hex_u64(((uint32_t*)block)[0]);
    serial_write(" bytes56-59=");
    serial_write_hex_u64(((uint32_t*)(block + 56))[0]);
    serial_write("\n");

    if (read_result < 0) {
        serial_write("EXT2: Read failed!\n");
        return -1;
    }

    sb = (struct ext2_superblock *)block;

    serial_write("EXT2: All bytes 56-72:\n");
    {
        uint8_t *bp = (uint8_t*)block;
        int j;
        for (j = 56; j < 72; j++) {
            serial_write_hex_u64(bp[j]);
            serial_write(" ");
        }
        serial_write("\n");
    }

    is_ext2 = (sb->magic == EXT2_MAGIC);

    serial_write("EXT2: is_ext2 = ");
    serial_write_hex_u64(is_ext2);
    serial_write("\n");

    if (!is_ext2) {
        serial_write("EXT2: Not ext2, formatting...\n");
        return ext2_format(fs, device);
    }

    memcpy(&fs->superblock, sb, sizeof(struct ext2_superblock));

    /* Compute block_size from log_block_size */
    fs->superblock.block_size = 1024UL << fs->superblock.log_block_size;
    fs->superblock.version = 1;

    serial_write("EXT2: inodes_count=");
    serial_write_hex_u64(fs->superblock.inodes_count);
    serial_write(" blocks_count=");
    serial_write_hex_u64(fs->superblock.blocks_count);
    serial_write(" block_size=");
    serial_write_hex_u64(fs->superblock.block_size);
    serial_write(" log_block_size=");
    serial_write_hex_u64(fs->superblock.log_block_size);
    serial_write("\n");

    block_bitmap_size = (fs->superblock.blocks_count + 7) / 8;
    fs->block_bitmap = (uint8_t *)kmalloc(block_bitmap_size);
    if (!fs->block_bitmap) return -1;

    inode_bitmap_size = (fs->superblock.inodes_count + 7) / 8;
    fs->inode_bitmap = (uint8_t *)kmalloc(inode_bitmap_size);
    if (!fs->inode_bitmap) {
        kfree(fs->block_bitmap);
        return -1;
    }

    block = (uint8_t *)kmalloc(fs->superblock.block_size);
    if (!block) {
        kfree(fs->block_bitmap);
        kfree(fs->inode_bitmap);
        return -1;
    }

    /* Block bitmap spans bb_blks consecutive blocks starting at block 2. */
    {
        uint32_t bb_blks = (fs->superblock.blocks_count + 8u * fs->superblock.block_size - 1u)
                           / (8u * fs->superblock.block_size);
        uint32_t i;
        for (i = 0; i < bb_blks; ++i) {
            uint32_t blk = 2u + i;
            size_t   off = (size_t)i * fs->superblock.block_size;
            size_t   len = fs->superblock.block_size;
            if (off + len > block_bitmap_size) len = block_bitmap_size - off;
            ramdisk_read(device, (blk * fs->superblock.block_size) / device->block_size,
                         block, fs->superblock.block_size);
            memcpy(fs->block_bitmap + off, block, len);
        }
        /* Inode bitmap follows directly after the block bitmap. */
        uint32_t ib_blk = 2u + bb_blks;
        ramdisk_read(device, (ib_blk * fs->superblock.block_size) / device->block_size,
                     block, fs->superblock.block_size);
        memcpy(fs->inode_bitmap, block, inode_bitmap_size);
    }

    kfree(block);

    inode_table_size = fs->superblock.inodes_count * sizeof(struct ext2_inode);
    fs->inode_table = (struct ext2_inode *)kmalloc(inode_table_size);
    if (!fs->inode_table) {
        kfree(fs->block_bitmap);
        kfree(fs->inode_bitmap);
        return -1;
    }

    serial_write("EXT2: Loading inode table...\n");
    serial_write("EXT2: inode size=");
    serial_write_hex_u64(sizeof(struct ext2_inode));
    serial_write(" block_size=");
    serial_write_hex_u64(fs->superblock.block_size);
    serial_write("\n");

    /* Load the full inode table — the previous 128-inode cap made
     * ext2_alloc_inode_num()'s output unusable past index 128 and left later
     * inodes as zeros in memory (so /etc/passwd and /etc/shadow, which the
     * host-side ext2_put.py placed in high inodes, appeared missing). */
    uint32_t max_inodes_to_read = fs->superblock.inodes_count;

    serial_write("EXT2: loading max_inodes=");
    serial_write_hex_u64(max_inodes_to_read);
    serial_write("\n");

    uint32_t last_block_num = 0xFFFFFFFF;
    uint8_t *inode_block = (uint8_t *)kmalloc(fs->superblock.block_size);
    if (!inode_block) return -1;

	/* Inode table starts after: boot(1) + sb(1) + block_bitmap(bb_blks) + inode_bitmap(1). */
	uint32_t bb_blks_mount = (fs->superblock.blocks_count + 8u * fs->superblock.block_size - 1u)
	                          / (8u * fs->superblock.block_size);
	uint32_t it_start_mount = 2u + bb_blks_mount + 1u;

	for (uint32_t i = 0; i < max_inodes_to_read; i++) {
		uint32_t inode_block_num = it_start_mount + (i * sizeof(struct ext2_inode)) / fs->superblock.block_size;
		uint32_t inode_offset = (i * sizeof(struct ext2_inode)) % fs->superblock.block_size;

		if (i < 4 || i == 13) {
			serial_write("EXT2: inode ");
			serial_write_hex_u64(i + 1);
			serial_write(" at block ");
			serial_write_hex_u64(inode_block_num);
			serial_write(" offset ");
			serial_write_hex_u64(inode_offset);
			serial_write("\\n");
		}

		if (inode_block_num != last_block_num) {
			ramdisk_read(device, (inode_block_num * fs->superblock.block_size) / device->block_size, inode_block, fs->superblock.block_size);
			last_block_num = inode_block_num;
		}
		memcpy(&fs->inode_table[i], inode_block + inode_offset, sizeof(struct ext2_inode));

		if (i < 4 || i == 13) {
			serial_write("EXT2: inode ");
			serial_write_hex_u64(i + 1);
			serial_write(" mode=");
			serial_write_hex_u64(fs->inode_table[i].mode);
			serial_write(" size=");
			serial_write_hex_u64(fs->inode_table[i].size);
			serial_write(" blocks[0]=");
			serial_write_hex_u64(fs->inode_table[i].block[0]);
			serial_write("\\n");
		}
	}
	kfree(inode_block);
    serial_write("EXT2: Inode table loaded\n");
    
    return 0;
}

static int parse_path_component(const char **path, char *component, size_t max_len) {
    size_t len = 0;
    
    while (**path == '/') (*path)++;
    
    if (**path == '\0') return 0;
    
    while (**path != '\0' && **path != '/' && len < max_len - 1) {
        component[len++] = *(*path)++;
    }
    
    component[len] = '\0';
    return (int)len;
}

static uint32_t find_entry_in_dir(struct ext2_filesystem *fs, uint32_t dir_inode_num, const char *name) {
    struct ext2_inode *dir_inode;
    uint8_t *block;
    uint32_t offset;
    
    if (dir_inode_num >= fs->superblock.inodes_count) return 0;
    
    dir_inode = &fs->inode_table[dir_inode_num - 1];
    if ((dir_inode->mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return 0;
    
    block = (uint8_t *)kmalloc(fs->superblock.block_size);
    if (!block) return 0;
    
    for (int i = 0; i < 12 && dir_inode->block[i]; i++) {
        ext2_read_block(fs, dir_inode->block[i], block);
        
        offset = 0;
        while (offset < dir_inode->size && offset < fs->superblock.block_size) {
            struct ext2_dir_entry *entry = (struct ext2_dir_entry *)(block + offset);
            
            if (entry->inode != 0 && entry->name_len > 0) {
                char entry_name[256];
                memcpy(entry_name, entry->name, entry->name_len);
                entry_name[entry->name_len] = '\0';
                
                if (strcmp(entry_name, name) == 0) {
                    kfree(block);
                    return entry->inode;
                }
            }
            
            if (entry->rec_len == 0) break;
            offset += entry->rec_len;
        }
    }
    
    kfree(block);
    return 0;
}

uint32_t ext2_lookup_inode(struct ext2_filesystem *fs, const char *path) {
    char component[256];
    uint32_t current_inode = EXT2_ROOT_INODE;
    
    if (path == 0 || path[0] == '\0') return 0;
    
    if (path[0] == '/' && path[1] == '\0') return EXT2_ROOT_INODE;
    
    while (parse_path_component(&path, component, sizeof(component)) > 0) {
        if (component[0] == '\0') break;

        current_inode = find_entry_in_dir(fs, current_inode, component);
        if (current_inode == 0) return 0;
    }

    return current_inode;
}

static int add_entry_to_dir(struct ext2_filesystem *fs, uint32_t dir_inode_num, uint32_t new_inode_num, const char *name, uint8_t file_type) {
    struct ext2_inode *dir_inode;
    uint8_t *block;
    uint32_t name_len = strlen(name);
    uint32_t needed_len = (8 + name_len + 3) & ~3;

    if (dir_inode_num == 0 || dir_inode_num >= fs->superblock.inodes_count) return -1;
    dir_inode = &fs->inode_table[dir_inode_num - 1];
    if ((dir_inode->mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return -1;

    block = (uint8_t *)kmalloc(fs->superblock.block_size);
    if (!block) return -1;

    for (int i = 0; i < 12; i++) {
        if (dir_inode->block[i] == 0) {
            uint32_t new_block = ext2_alloc_block(fs);
            if (new_block == 0) { kfree(block); return -1; }
            dir_inode->block[i] = new_block;
            dir_inode->size += fs->superblock.block_size;
            dir_inode->blocks += fs->superblock.block_size / 512;
            memset(block, 0, fs->superblock.block_size);
            
            struct ext2_dir_entry *new_entry = (struct ext2_dir_entry *)block;
            new_entry->inode = new_inode_num;
            new_entry->rec_len = fs->superblock.block_size;
            new_entry->name_len = name_len;
            new_entry->file_type = file_type;
            memcpy(new_entry->name, name, name_len);
            
            ext2_write_block(fs, new_block, block);
            ext2_write_inode(fs, dir_inode_num);
            kfree(block);
            return 0;
        }

        ext2_read_block(fs, dir_inode->block[i], block);
        uint32_t offset = 0;
        while (offset < fs->superblock.block_size) {
            struct ext2_dir_entry *entry = (struct ext2_dir_entry *)(block + offset);
            uint32_t used_len = (8 + entry->name_len + 3) & ~3;

            if (entry->inode == 0) {
                if (entry->rec_len >= needed_len) {
                    entry->inode = new_inode_num;
                    entry->name_len = name_len;
                    entry->file_type = file_type;
                    memcpy(entry->name, name, name_len);
                    ext2_write_block(fs, dir_inode->block[i], block);
                    kfree(block);
                    return 0;
                }
            } else {
                if (entry->rec_len >= used_len + needed_len) {
                    uint32_t old_rec_len = entry->rec_len;
                    entry->rec_len = used_len;
                    
                    offset += used_len;
                    entry = (struct ext2_dir_entry *)(block + offset);
                    entry->inode = new_inode_num;
                    entry->rec_len = old_rec_len - used_len;
                    entry->name_len = name_len;
                    entry->file_type = file_type;
                    memcpy(entry->name, name, name_len);
                    
                    ext2_write_block(fs, dir_inode->block[i], block);
                    kfree(block);
                    return 0;
                }
            }
            if (entry->rec_len == 0) break;
            offset += entry->rec_len;
        }
    }

    kfree(block);
    return -1;
}

uint32_t ext2_create(struct ext2_filesystem *fs, const char *path, uint16_t mode) {
    char parent_path[256];
    char name[256];
    const char *last_slash;
    uint32_t parent_inode;
    uint32_t new_inode_num;
    struct ext2_inode *new_inode;
    
    if (!path || path[0] != '/') return 0;
    
    last_slash = strrchr(path, '/');
    if (last_slash == path) {
        strcpy(parent_path, "/");
        strcpy(name, last_slash + 1);
    } else {
        size_t parent_len = last_slash - path;
        memcpy(parent_path, path, parent_len);
        parent_path[parent_len] = '\0';
        strcpy(name, last_slash + 1);
    }
    
    if (name[0] == '\0') return 0;
    
    parent_inode = ext2_lookup_inode(fs, parent_path);
    if (parent_inode == 0) return 0;
    
    if (find_entry_in_dir(fs, parent_inode, name) != 0) return 0;
    
	new_inode_num = ext2_alloc_inode_num(fs);
	if (new_inode_num == 0) return 0;

	new_inode = &fs->inode_table[new_inode_num - 1];
	new_inode->mode = (mode & ~EXT2_S_IFMT) | EXT2_S_IFREG;
    new_inode->uid = 0;
    new_inode->gid = 0;
    new_inode->size = 0;
    new_inode->links_count = 1;
    new_inode->blocks = 0;
    new_inode->block[0] = 0;
    
	if (add_entry_to_dir(fs, parent_inode, new_inode_num, name, 1) < 0) {
		ext2_free_inode_num(fs, new_inode_num);
		return 0;
	}

	ext2_write_inode(fs, new_inode_num);
	ext2_write_superblock(fs);
	ext2_write_bitmaps(fs);

	return new_inode_num;
}

/* Remove a directory entry for `name` from a parent directory and free the
 * target inode and its direct data blocks. Returns 0 on success. Refuses to
 * remove non-empty directories. */
int ext2_unlink(struct ext2_filesystem *fs, const char *path) {
    char parent_path[256];
    char name[256];
    const char *last_slash;
    uint32_t parent_inode;
    uint32_t target_inode = 0;
    uint8_t *block;
    int i;
    int done = 0;

    if (!path || path[0] != '/') return -1;

    last_slash = strrchr(path, '/');
    if (last_slash == path) {
        strcpy(parent_path, "/");
        strcpy(name, last_slash + 1);
    } else {
        size_t parent_len = (size_t)(last_slash - path);
        memcpy(parent_path, path, parent_len);
        parent_path[parent_len] = '\0';
        strcpy(name, last_slash + 1);
    }
    if (name[0] == '\0') return -1;

    parent_inode = ext2_lookup_inode(fs, parent_path);
    if (parent_inode == 0) return -1;

    block = (uint8_t *)kmalloc(fs->superblock.block_size);
    if (!block) return -1;

    {
        struct ext2_inode *dir = &fs->inode_table[parent_inode - 1];
        for (i = 0; i < 12 && !done; i++) {
            uint32_t offset = 0;
            struct ext2_dir_entry *prev = 0;
            if (dir->block[i] == 0) continue;
            ext2_read_block(fs, dir->block[i], block);
            while (offset < fs->superblock.block_size) {
                struct ext2_dir_entry *e = (struct ext2_dir_entry *)(block + offset);
                if (e->rec_len == 0) break;
                if (e->inode != 0 && e->name_len == strlen(name) &&
                    memcmp(e->name, name, e->name_len) == 0) {
                    target_inode = e->inode;
                    /* Unlink the entry: merge into previous, or zero the inode. */
                    if (prev != 0) {
                        prev->rec_len = (uint16_t)(prev->rec_len + e->rec_len);
                    } else {
                        e->inode = 0;
                    }
                    ext2_write_block(fs, dir->block[i], block);
                    done = 1;
                    break;
                }
                prev = e;
                offset += e->rec_len;
            }
        }
    }

    if (!target_inode) {
        kfree(block);
        return -1;
    }

    /* If it's a directory, ensure it is empty (only . and ..). */
    {
        struct ext2_inode *ti = &fs->inode_table[target_inode - 1];
        if ((ti->mode & EXT2_S_IFMT) == EXT2_S_IFDIR) {
            int entries = 0;
            if (ti->block[0] != 0) {
                uint32_t offset = 0;
                ext2_read_block(fs, ti->block[0], block);
                while (offset < fs->superblock.block_size) {
                    struct ext2_dir_entry *e = (struct ext2_dir_entry *)(block + offset);
                    if (e->rec_len == 0) break;
                    if (e->inode != 0) entries++;
                    offset += e->rec_len;
                }
            }
            if (entries > 2) { /* more than . and .. */
                kfree(block);
                return -2; /* directory not empty */
            }
        }

        /* Free the inode's direct data blocks, then the inode itself. */
        for (i = 0; i < 12; i++) {
            if (ti->block[i] != 0) {
                ext2_free_block(fs, ti->block[i]);
                ti->block[i] = 0;
            }
        }
        ti->links_count = 0;
        ti->size = 0;
        ti->blocks = 0;
        ext2_write_inode(fs, target_inode);
        ext2_free_inode_num(fs, target_inode);
    }

    ext2_write_superblock(fs);
    ext2_write_bitmaps(fs);
    kfree(block);
    return 0;
}

uint32_t ext2_mkdir(struct ext2_filesystem *fs, const char *path, uint16_t mode) {
    char parent_path[256];
    char name[256];
    const char *last_slash;
    uint32_t parent_inode;
    uint32_t new_inode_num;
    struct ext2_inode *new_inode;
    uint32_t new_block;
    uint8_t *block;
    
    if (!path || path[0] != '/') return 0;
    
    last_slash = strrchr(path, '/');
    if (last_slash == path) {
        strcpy(parent_path, "/");
        strcpy(name, last_slash + 1);
    } else {
        size_t parent_len = last_slash - path;
        memcpy(parent_path, path, parent_len);
        parent_path[parent_len] = '\0';
        strcpy(name, last_slash + 1);
    }
    
    if (name[0] == '\0') return 0;
    
    parent_inode = ext2_lookup_inode(fs, parent_path);
    if (parent_inode == 0) return 0;
    
    if (find_entry_in_dir(fs, parent_inode, name) != 0) return 0;
    
    new_inode_num = ext2_alloc_inode_num(fs);
    if (new_inode_num == 0) return 0;
    
    new_block = ext2_alloc_block(fs);
    if (new_block == 0) {
        ext2_free_inode_num(fs, new_inode_num);
        return 0;
    }
    
	new_inode = &fs->inode_table[new_inode_num - 1];
	new_inode->mode = (mode & ~EXT2_S_IFMT) | EXT2_S_IFDIR;
    new_inode->uid = 0;
    new_inode->gid = 0;
    new_inode->size = fs->superblock.block_size;
    new_inode->links_count = 2;
    new_inode->blocks = fs->superblock.block_size / 512;
    new_inode->block[0] = new_block;
    
    block = (uint8_t *)kmalloc(fs->superblock.block_size);
    if (!block) {
        ext2_free_block(fs, new_block);
        ext2_free_inode_num(fs, new_inode_num);
        return 0;
    }
    
    memset(block, 0, fs->superblock.block_size);
    
    uint32_t offset = 0;
    struct ext2_dir_entry *dot = (struct ext2_dir_entry *)(block + offset);
    dot->inode = new_inode_num;
    dot->name_len = 1;
    dot->file_type = 2;
    dot->name[0] = '.';
    dot->rec_len = 12;
    offset += dot->rec_len;
    
    struct ext2_dir_entry *dotdot = (struct ext2_dir_entry *)(block + offset);
    dotdot->inode = parent_inode;
    dotdot->name_len = 2;
    dotdot->file_type = 2;
    dotdot->name[0] = '.';
    dotdot->name[1] = '.';
    dotdot->rec_len = fs->superblock.block_size - offset;
    
    ext2_write_block(fs, new_block, block);
    kfree(block);
    
    if (add_entry_to_dir(fs, parent_inode, new_inode_num, name, 2) < 0) {
        ext2_free_block(fs, new_block);
        ext2_free_inode_num(fs, new_inode_num);
        return 0;
    }

    fs->inode_table[parent_inode - 1].links_count++;

    /* Persist the new directory's inode and the updated parent inode — without
     * this the directory only existed in RAM and vanished on the next mount. */
    ext2_write_inode(fs, new_inode_num);
    ext2_write_inode(fs, parent_inode);

    ext2_write_superblock(fs);
    ext2_write_bitmaps(fs);

    return new_inode_num;
}

ssize_t ext2_read(struct ext2_filesystem *fs, uint32_t inode_num, uint32_t offset, size_t size, void *buffer) {
    struct ext2_inode *inode;
    uint8_t *block_buffer;
    uint8_t *indirect_buffer = 0;
    uint8_t *dind_buffer = 0;       /* double-indirect L1 table (block[13]) */
    uint8_t *dind_l2_buffer = 0;    /* current L2 table */
    uint32_t dind_l2_cached = 0;    /* which L2 block dind_l2_buffer holds */
    uint32_t ptrs = fs->superblock.block_size / sizeof(uint32_t);
    size_t bytes_read = 0;

    if (inode_num >= fs->superblock.inodes_count) return -1;
    
    inode = &fs->inode_table[inode_num - 1];
    if ((inode->mode & EXT2_S_IFMT) == EXT2_S_IFDIR) return -1;
    if (offset >= inode->size) return 0;
    
    if (offset + size > inode->size) size = inode->size - offset;
    
    block_buffer = (uint8_t *)kmalloc(fs->superblock.block_size);
    if (!block_buffer) return -1;
    
    while (bytes_read < size) {
        uint32_t block_index = (offset + bytes_read) / fs->superblock.block_size;
        uint32_t block_offset = (offset + bytes_read) % fs->superblock.block_size;
        uint32_t data_block = 0;
        
        if (block_index < 12) {
            data_block = inode->block[block_index];
        } else if (block_index < 12 + ptrs && inode->block[12] != 0) {
            if (indirect_buffer == 0) {
                indirect_buffer = (uint8_t *)kmalloc(fs->superblock.block_size);
                if (indirect_buffer == 0 || ext2_read_block(fs, inode->block[12], indirect_buffer) < 0) {
                    memset((uint8_t *)buffer + bytes_read, 0, size - bytes_read);
                    break;
                }
            }
            data_block = ((uint32_t *)indirect_buffer)[block_index - 12];
        } else if (block_index < 12 + ptrs + ptrs * ptrs && inode->block[13] != 0) {
            /* double-indirect: block[13] -> L1 table -> L2 table -> data */
            uint32_t di = block_index - 12 - ptrs;
            uint32_t l1 = di / ptrs;
            uint32_t l2 = di % ptrs;
            uint32_t l2_block;
            if (dind_buffer == 0) {
                dind_buffer = (uint8_t *)kmalloc(fs->superblock.block_size);
                if (dind_buffer == 0 || ext2_read_block(fs, inode->block[13], dind_buffer) < 0) {
                    memset((uint8_t *)buffer + bytes_read, 0, size - bytes_read);
                    break;
                }
            }
            l2_block = ((uint32_t *)dind_buffer)[l1];
            if (l2_block == 0) {
                memset((uint8_t *)buffer + bytes_read, 0, size - bytes_read);
                break;
            }
            if (dind_l2_buffer == 0) dind_l2_buffer = (uint8_t *)kmalloc(fs->superblock.block_size);
            if (dind_l2_buffer == 0) { memset((uint8_t *)buffer + bytes_read, 0, size - bytes_read); break; }
            if (dind_l2_cached != l2_block) {
                if (ext2_read_block(fs, l2_block, dind_l2_buffer) < 0) {
                    memset((uint8_t *)buffer + bytes_read, 0, size - bytes_read);
                    break;
                }
                dind_l2_cached = l2_block;
            }
            data_block = ((uint32_t *)dind_l2_buffer)[l2];
        }

        if (data_block == 0) {
            memset((uint8_t *)buffer + bytes_read, 0, size - bytes_read);
            break;
        }
        
        if (ext2_read_block(fs, data_block, block_buffer) < 0) break;
        
        size_t chunk = size - bytes_read;
        if (chunk > fs->superblock.block_size - block_offset) {
            chunk = fs->superblock.block_size - block_offset;
        }
        
        memcpy((uint8_t *)buffer + bytes_read, block_buffer + block_offset, chunk);
        bytes_read += chunk;
    }
    
    if (indirect_buffer) kfree(indirect_buffer);
    if (dind_buffer) kfree(dind_buffer);
    if (dind_l2_buffer) kfree(dind_l2_buffer);
    kfree(block_buffer);
    return bytes_read;
}

/* Read one pointer from a (possibly just-allocated) indirect table block. */
static uint32_t ext2_ptr_get(struct ext2_filesystem *fs, uint32_t table_block, uint32_t idx) {
    uint8_t *t = (uint8_t *)kmalloc(fs->superblock.block_size);
    uint32_t v;
    if (!t) return 0;
    if (ext2_read_block(fs, table_block, t) < 0) { kfree(t); return 0; }
    v = ((uint32_t *)t)[idx];
    kfree(t);
    return v;
}
static void ext2_ptr_set(struct ext2_filesystem *fs, uint32_t table_block, uint32_t idx, uint32_t val) {
    uint8_t *t = (uint8_t *)kmalloc(fs->superblock.block_size);
    if (!t) return;
    if (ext2_read_block(fs, table_block, t) < 0) memset(t, 0, fs->superblock.block_size);
    ((uint32_t *)t)[idx] = val;
    ext2_write_block(fs, table_block, t);
    kfree(t);
}

/* Map logical block index -> physical block, allocating data + any indirect
 * table blocks on the way. Returns 0 on failure. Supports direct, single and
 * double indirect (matches ext2_read). */
static uint32_t ext2_bmap_alloc(struct ext2_filesystem *fs, struct ext2_inode *inode, uint32_t bi) {
    uint32_t ptrs = fs->superblock.block_size / sizeof(uint32_t);
    uint32_t sectors = fs->superblock.block_size / 512;

    if (bi < 12) {
        if (inode->block[bi] == 0) {
            uint32_t nb = ext2_alloc_block(fs);
            if (!nb) return 0;
            inode->block[bi] = nb; inode->blocks += sectors;
        }
        return inode->block[bi];
    }
    if (bi < 12 + ptrs) {
        uint32_t idx = bi - 12, db;
        if (inode->block[12] == 0) {
            uint32_t nb = ext2_alloc_block(fs);
            if (!nb) return 0;
            { uint8_t *z = kmalloc(fs->superblock.block_size); if (z) { memset(z,0,fs->superblock.block_size); ext2_write_block(fs,nb,z); kfree(z);} }
            inode->block[12] = nb; inode->blocks += sectors;
        }
        db = ext2_ptr_get(fs, inode->block[12], idx);
        if (db == 0) {
            db = ext2_alloc_block(fs);
            if (!db) return 0;
            ext2_ptr_set(fs, inode->block[12], idx, db);
            inode->blocks += sectors;
        }
        return db;
    }
    if (bi < 12 + ptrs + ptrs * ptrs) {
        uint32_t di = bi - 12 - ptrs, l1 = di / ptrs, l2 = di % ptrs, l2blk, db;
        if (inode->block[13] == 0) {
            uint32_t nb = ext2_alloc_block(fs);
            if (!nb) return 0;
            { uint8_t *z = kmalloc(fs->superblock.block_size); if (z) { memset(z,0,fs->superblock.block_size); ext2_write_block(fs,nb,z); kfree(z);} }
            inode->block[13] = nb; inode->blocks += sectors;
        }
        l2blk = ext2_ptr_get(fs, inode->block[13], l1);
        if (l2blk == 0) {
            l2blk = ext2_alloc_block(fs);
            if (!l2blk) return 0;
            { uint8_t *z = kmalloc(fs->superblock.block_size); if (z) { memset(z,0,fs->superblock.block_size); ext2_write_block(fs,l2blk,z); kfree(z);} }
            ext2_ptr_set(fs, inode->block[13], l1, l2blk);
            inode->blocks += sectors;
        }
        db = ext2_ptr_get(fs, l2blk, l2);
        if (db == 0) {
            db = ext2_alloc_block(fs);
            if (!db) return 0;
            ext2_ptr_set(fs, l2blk, l2, db);
            inode->blocks += sectors;
        }
        return db;
    }
    return 0;   /* beyond double-indirect */
}

int ext2_truncate(struct ext2_filesystem *fs, const char *path) {
    uint32_t inode_num = ext2_lookup_inode(fs, path);
    struct ext2_inode *inode;
    if (inode_num == 0 || inode_num >= fs->superblock.inodes_count) return -1;
    inode = &fs->inode_table[inode_num - 1];
    inode->size = 0;                     /* keep block[] for reuse on next write */
    ext2_write_inode(fs, inode_num);
    return 0;
}

ssize_t ext2_write(struct ext2_filesystem *fs, uint32_t inode_num, uint32_t offset, size_t size, const void *buffer) {
    struct ext2_inode *inode;
    uint8_t *block_buffer;
    size_t bytes_written = 0;

    if (inode_num >= fs->superblock.inodes_count) return -1;

    inode = &fs->inode_table[inode_num - 1];
    if ((inode->mode & EXT2_S_IFMT) == EXT2_S_IFDIR) return -1;

    block_buffer = (uint8_t *)kmalloc(fs->superblock.block_size);
    if (!block_buffer) return -1;

    while (bytes_written < size) {
        uint32_t block_index = (offset + bytes_written) / fs->superblock.block_size;
        uint32_t block_offset = (offset + bytes_written) % fs->superblock.block_size;
        uint32_t data_block = ext2_bmap_alloc(fs, inode, block_index);

        if (data_block == 0) break;

        if (block_offset != 0 || (size - bytes_written) < fs->superblock.block_size) {
            ext2_read_block(fs, data_block, block_buffer);   /* partial block: preserve */
        }

        size_t chunk = size - bytes_written;
        if (chunk > fs->superblock.block_size - block_offset) {
            chunk = fs->superblock.block_size - block_offset;
        }

        memcpy(block_buffer + block_offset, (const uint8_t *)buffer + bytes_written, chunk);
        ext2_write_block(fs, data_block, block_buffer);

        bytes_written += chunk;
    }

    if (offset + bytes_written > inode->size) {
        inode->size = offset + bytes_written;
    }
    ext2_write_inode(fs, inode_num);

    kfree(block_buffer);

    ext2_write_superblock(fs);
    ext2_write_bitmaps(fs);

    return bytes_written;
}

int ext2_readdir(struct ext2_filesystem *fs, uint32_t inode_num, struct ext2_dir_entry *entries, int max_entries) {
    struct ext2_inode *inode;
    uint8_t *block;
    int count = 0;
    uint32_t offset;
    
    serial_write("EXT2: readdir ENTER inode_num=");
    serial_write_hex_u64(inode_num);
    serial_write(" max_entries=");
    serial_write_hex_u64(max_entries);
    serial_write(" inodes_count=");
    serial_write_hex_u64(fs->superblock.inodes_count);
    serial_write("\n");
    
    if (inode_num >= fs->superblock.inodes_count || max_entries <= 0) return 0;
    
    inode = &fs->inode_table[inode_num - 1];
    serial_write("EXT2: readdir inode=");
    serial_write_hex_u64(inode_num);
    serial_write(" mode=");
    serial_write_hex_u64(inode->mode);
    serial_write(" blocks[0]=");
    serial_write_hex_u64(inode->block[0]);
    serial_write(" blocks[1]=");
    serial_write_hex_u64(inode->block[1]);
    serial_write("\n");
    if ((inode->mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return 0;
    
    block = (uint8_t *)kmalloc(fs->superblock.block_size);
    if (!block) return 0;
    
    for (int i = 0; i < 12 && inode->block[i] && count < max_entries; i++) {
        serial_write("EXT2: reading block ");
        serial_write_hex_u64(inode->block[i]);
        serial_write("\n");
        ext2_read_block(fs, inode->block[i], block);
        offset = 0;
        while (offset < inode->size && offset < fs->superblock.block_size && count < max_entries) {
            struct ext2_dir_entry *entry = (struct ext2_dir_entry *)(block + offset);
            
            if (entry->rec_len == 0) break;
            
            if (entry->inode != 0 && entry->name_len > 0) {
                entries[count].inode = entry->inode;
                entries[count].rec_len = entry->rec_len;
                entries[count].name_len = entry->name_len;
                entries[count].file_type = entry->file_type;
                memcpy(entries[count].name, entry->name, entry->name_len);
                entries[count].name[entry->name_len] = '\0';
                count++;
            }
            
            offset += entry->rec_len;
        }
    }
    
    kfree(block);
    return count;
}

int ext2_stat(struct ext2_filesystem *fs, uint32_t inode_num, struct ext2_inode *inode_out) {
    if (inode_num >= fs->superblock.inodes_count) return -1;
    memcpy(inode_out, &fs->inode_table[inode_num - 1], sizeof(struct ext2_inode));
    return 0;
}

void ext2_unmount(struct ext2_filesystem *fs) {
    if (fs->block_bitmap) {
        kfree(fs->block_bitmap);
        fs->block_bitmap = NULL;
    }
    if (fs->inode_bitmap) {
        kfree(fs->inode_bitmap);
        fs->inode_bitmap = NULL;
    }
    if (fs->inode_table) {
        kfree(fs->inode_table);
        fs->inode_table = NULL;
    }
    memset(fs, 0, sizeof(*fs));
}
