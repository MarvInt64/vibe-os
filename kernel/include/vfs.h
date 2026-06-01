#ifndef VIBEOS_VFS_H
#define VIBEOS_VFS_H

#include "types.h"

struct vfs_file {
	const char *path;
	const uint8_t *data;
	const uint8_t *end;
};

enum vfs_node_kind {
	VFS_NODE_NONE = 0,
	VFS_NODE_FILE = 1,
	VFS_NODE_DIRECTORY = 2
};

struct vfs_stat {
	uint32_t kind;
	uint64_t size;
};

struct vfs_dir_entry {
	const char *name;
	uint32_t kind;
	uint64_t size;
};

const struct vfs_file *vfs_lookup(const char *path);
size_t vfs_file_size(const struct vfs_file *file);
int vfs_stat_path(const char *path, struct vfs_stat *stat);
int vfs_readdir(const char *path, uint32_t index, struct vfs_dir_entry *entry);

int vfs_create(const char *path);
int vfs_unlink(const char *path);
int vfs_mkdir(const char *path);
ssize_t vfs_write(const char *path, size_t offset, const void *data, size_t count);
ssize_t vfs_read_user(const char *path, size_t offset, void *buffer, size_t count);
ssize_t vfs_read(const char *path, size_t offset, void *buffer, size_t count);
int vfs_file_exists(const char *path);
ssize_t vfs_write_all(const char *path, const void *data, size_t count);

struct ext2_filesystem;
void vfs_set_ext2(struct ext2_filesystem *fs);
struct ext2_filesystem *vfs_get_ext2(void);

#endif
