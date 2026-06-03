/*
 * Simple Ext2-like Filesystem Structures
 * Designed for VibeOS - a minimal implementation for learning and testing
 */

#ifndef VIBEOS_EXT2_FS_H
#define VIBEOS_EXT2_FS_H

#include "types.h"
#include "ramdisk.h"

/* Filesystem magic number */
#define EXT2_MAGIC 0xEF53

/* Block size for filesystem (1KB, standard for ext2) */
#define EXT2_BLOCK_SIZE 1024

/* Inode structure - 256 bytes (ext2 with inode_size=256) */
struct ext2_inode {
    uint16_t mode;          /* File type and permissions */
    uint16_t uid;           /* Owner ID */
    uint32_t size;          /* File size in bytes */
    uint32_t atime;         /* Access time */
    uint32_t ctime;         /* Creation time */
    uint32_t mtime;         /* Modification time */
    uint32_t dtime;         /* Deletion time */
    uint16_t gid;           /* Group ID */
    uint16_t links_count;   /* Number of hard links */
    uint32_t blocks;        /* Number of 512B blocks used */
    uint32_t flags;         /* File flags */
    uint32_t osd1;          /* OS-dependent value 1 (Linux: reserved=0) */
    uint32_t block[15];     /* Direct, indirect, double indirect blocks */
    uint32_t file_acl;      /* File ACL */
    uint32_t dir_acl;       /* Directory ACL */
    uint32_t faddr;         /* Fragment block address */
    uint8_t  frag_num;      /* Fragment number */
    uint8_t  frag_size;     /* Fragment size */
    uint16_t reserved2;     /* Reserved */
    uint32_t reserved3[2];  /* More reserved - was 3 */
    /* Extended attributes (ext2 with inode_size=256) */
    uint32_t atime_extra;   /* Access time extra */
    uint32_t ctime_extra;   /* Creation time extra */
    uint32_t mtime_extra;   /* Modification time extra */
    uint32_t dtime_extra;   /* Deletion time extra */
    uint32_t version;       /* File version */
    uint32_t blocks_hi;     /* Blocks high */
    uint32_t reserved4[16]; /* Reserved for future use */
    uint32_t reserved5[11]; /* More reserved to reach 256 bytes */
} __attribute__((packed));

/* Directory entry */
struct ext2_dir_entry {
    uint32_t inode;         /* Inode number */
    uint16_t rec_len;       /* Directory entry length */
    uint8_t name_len;       /* Name length */
    uint8_t file_type;      /* File type */
    char name[256];         /* Filename (variable length) */
};

/* Superblock structure - proper ext2 layout */
struct ext2_superblock {
	uint32_t inodes_count;       /* 0x00 */
	uint32_t blocks_count;       /* 0x04 */
	uint32_t reserved_blocks;    /* 0x08 */
	uint32_t free_blocks;        /* 0x0C */
	uint32_t free_inodes;        /* 0x10 */
	uint32_t first_data_block;   /* 0x14 */
	uint32_t log_block_size;     /* 0x18 */
	uint32_t blocks_per_group;    /* 0x1C */
	uint32_t inodes_per_group;   /* 0x20 */
	uint32_t mtime;              /* 0x24 */
	uint32_t wtime;              /* 0x28 */
	uint16_t mnt_count;          /* 0x2C */
	uint16_t max_mnt_count;      /* 0x2E */
	uint16_t pad_magic;          /* 0x30 - pad to make magic at 0x38 */
	uint16_t state;               /* 0x32 */
	uint16_t errors;              /* 0x34 */
	uint16_t minor_rev_level;     /* 0x36 */
	uint16_t magic;              /* 0x38 */
	uint16_t pad2;               /* 0x3A */
	uint32_t first_inode;         /* 0x3C */
	uint16_t inode_size;         /* 0x40 */
	uint16_t block_group_nr;     /* 0x42 */
	uint32_t feature_compat;      /* 0x44 */
	uint32_t feature_incompat;    /* 0x48 */
	uint32_t feature_ro_compat;   /* 0x4C */
	uint8_t  uuid[16];           /* 0x50 */
	char     volume_name[16];    /* 0x60 */
	/* Computed fields (not from disk) */
	uint32_t block_size;         /* computed: 1024 << log_block_size */
	uint32_t version;            /* not stored, computed */
	uint8_t  pad[1024 - 0x80];  /* padding to 1024 bytes */
};

/* Filesystem instance */
struct ext2_filesystem {
    struct ramdisk_device *device; /* Underlying block device */
    struct ext2_superblock superblock; /* Superblock */
    uint8_t *block_bitmap; /* Block allocation bitmap */
    uint8_t *inode_bitmap; /* Inode allocation bitmap */
    struct ext2_inode *inode_table; /* Inode table */
    uint32_t block_bitmap_block; /* Block number of block bitmap */
    uint32_t inode_bitmap_block; /* Block number of inode bitmap */
    uint32_t inode_table_block; /* Block number of inode table */
    uint32_t first_data_block; /* First data block */
};

/* File and directory modes */
#define EXT2_S_IFMT  0170000  /* Format mask */
#define EXT2_S_IFSOCK 0140000 /* Socket */
#define EXT2_S_IFLNK  0120000 /* Symbolic link */
#define EXT2_S_IFREG  0100000 /* Regular file */
#define EXT2_S_IFBLK  0060000 /* Block device */
#define EXT2_S_IFDIR  0040000 /* Directory */
#define EXT2_S_IFCHR  0020000 /* Character device */
#define EXT2_S_IFIFO  0010000 /* FIFO */
#define EXT2_S_ISUID  0004000 /* SUID */
#define EXT2_S_ISGID  0002000 /* SGID */
#define EXT2_S_ISVTX  0001000 /* Sticky bit */
#define EXT2_S_IRWXU  00700   /* User access */
#define EXT2_S_IWUSR   00200   /* Write user */

/* Function prototypes */

/*
 * Format a new filesystem on a block device
 * @param fs: Filesystem structure to populate
 * @param device: Block device to format
 * @return 0 on success, -1 on failure
 */
int ext2_format(struct ext2_filesystem *fs, struct ramdisk_device *device);

/*
 * Mount an existing filesystem
 * @param fs: Filesystem structure to populate
 * @param device: Block device containing filesystem
 * @return 0 on success, -1 on failure
 */
int ext2_mount(struct ext2_filesystem *fs, struct ramdisk_device *device);

/*
 * Create a new file with specified mode
 * @param fs: Filesystem instance
 * @param path: Full path to file
 * @param mode: File permissions and type
 * @return Inode number on success, 0 on failure
 */
uint32_t ext2_create(struct ext2_filesystem *fs, const char *path, uint16_t mode);

/*
 * Create a new directory
 * @param fs: Filesystem instance
 * @param path: Full path to directory
 * @param mode: Directory permissions
 * @return Inode number on success, 0 on failure
 */
uint32_t ext2_mkdir(struct ext2_filesystem *fs, const char *path, uint16_t mode);

/*
 * Remove a file or empty directory and free its inode + data blocks.
 * @return 0 on success, negative on failure (-2 = directory not empty)
 */
int ext2_unlink(struct ext2_filesystem *fs, const char *path);

/*
 * Read data from a file
 * @param fs: Filesystem instance
 * @param inode: Inode number of file
 * @param offset: Byte offset to read from
 * @param size: Number of bytes to read
 * @param buffer: Buffer to store data
 * @return Number of bytes read, or -1 on error
 */
ssize_t ext2_read(struct ext2_filesystem *fs, uint32_t inode, uint32_t offset, size_t size, void *buffer);

/*
 * Write data to a file
 * @param fs: Filesystem instance
 * @param inode: Inode number of file
 * @param offset: Byte offset to write to
 * @param size: Number of bytes to write
 * @param buffer: Data to write
 * @return Number of bytes written, or -1 on error
 */
ssize_t ext2_write(struct ext2_filesystem *fs, uint32_t inode, uint32_t offset, size_t size, const void *buffer);

/*
 * Lookup inode by path
 * @param fs: Filesystem instance
 * @param path: Path to lookup
 * @return Inode number, 0 if not found
 */
uint32_t ext2_lookup_inode(struct ext2_filesystem *fs, const char *path);

/* Reset a file's logical size to 0 (keeps its data blocks for reuse, so
 * repeatedly rewriting the same file does not leak blocks). 0 on success. */
int ext2_truncate(struct ext2_filesystem *fs, const char *path);

/*
 * Read directory entries
 * @param fs: Filesystem instance
 * @param inode: Inode number of directory
 * @param entries: Array to store entries
 * @param max_entries: Maximum number of entries
 * @return Number of entries read
 */
int ext2_readdir(struct ext2_filesystem *fs, uint32_t inode, struct ext2_dir_entry *entries, int max_entries);

/*
 * Get file/directory stats
 * @param fs: Filesystem instance
 * @param inode: Inode number
 * @param inode_out: Output buffer for inode structure
 * @return 0 on success, -1 on failure
 */
int ext2_stat(struct ext2_filesystem *fs, uint32_t inode, struct ext2_inode *inode_out);

/*
 * Write the in-memory inode table entry for 'inode_num' back to disk.
 * Call this after modifying fs->inode_table[inode_num - 1] directly
 * (e.g. to update mode/uid/gid without a full re-create).
 * Returns 0 on success, -1 on failure.
 */
int ext2_write_inode(struct ext2_filesystem *fs, uint32_t inode_num);

/*
 * Unmount filesystem and free resources
 * @param fs: Filesystem instance
 */
void ext2_unmount(struct ext2_filesystem *fs);

#endif
