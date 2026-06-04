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

#include "vfs.h"
#include "ext2_fs.h"
#include "process.h"
#include "string.h"
#include "serial.h"
#include "syscall.h"

#define VFS_USER_MAX_FILES 32
#define VFS_USER_MAX_SIZE 8192

struct vfs_user_file {
	char path[64];
	uint8_t data[VFS_USER_MAX_SIZE];
	size_t size;
	uint8_t used;
};

static struct vfs_user_file g_user_files[VFS_USER_MAX_FILES];

extern const uint8_t user_init_elf_start[];
extern const uint8_t user_init_elf_end[];
extern const uint8_t user_hello_elf_start[];
extern const uint8_t user_hello_elf_end[];
extern const uint8_t user_windowmgr_elf_start[];
extern const uint8_t user_windowmgr_elf_end[];
extern const uint8_t user_sh_start[];
extern const uint8_t user_sh_end[];
extern const uint8_t user_edit_start[];
extern const uint8_t user_edit_end[];

static const uint8_t g_motd_data[] = "WELCOME TO VIBEOS\\nREAD ONLY VFS ONLINE\\n";

static struct ext2_filesystem *g_ext2 = 0;

void vfs_set_ext2(struct ext2_filesystem *fs) {
    g_ext2 = fs;
}

struct ext2_filesystem *vfs_get_ext2(void) {
    return g_ext2;
}

static char ascii_lower(char c) {
    if (c >= 'A' && c <= 'Z') {
        return (char)(c + ('a' - 'A'));
    }
    return c;
}

static int string_equals_ci(const char *left, const char *right) {
    while (*left != '\0' && *right != '\0') {
        if (ascii_lower(*left) != ascii_lower(*right)) {
            return 0;
        }
        ++left;
        ++right;
    }

    return *left == '\0' && *right == '\0';
}

static const struct vfs_file g_vfs_files[] = {
{"/bin/sh", user_sh_start, user_sh_end},
{"/bin/edit", user_edit_start, user_edit_end},
{"/etc/motd", g_motd_data, g_motd_data + sizeof(g_motd_data) - 1u}
};

#define VFS_FILES_COUNT (sizeof(g_vfs_files) / sizeof(g_vfs_files[0]))

const struct vfs_file *vfs_lookup(const char *path) {
    size_t i;

    if (path == 0) {
        return 0;
    }

    for (i = 0; i < VFS_FILES_COUNT; ++i) {
        if (string_equals_ci(path, g_vfs_files[i].path)) {
            return &g_vfs_files[i];
        }
    }

    return 0;
}

size_t vfs_file_size(const struct vfs_file *file) {
    if (file == 0 || file->end < file->data) {
        return 0;
    }

    return (size_t)(file->end - file->data);
}

/* --- ext2-backed stat --- */

static int ext2_vfs_stat(const char *path, struct vfs_stat *stat) {
    uint32_t ino;
    struct ext2_inode inode;

    if (g_ext2 == 0) return 0;

    ino = ext2_lookup_inode(g_ext2, path);
    if (ino == 0) return 0;

    if (ext2_stat(g_ext2, ino, &inode) < 0) return 0;

    if ((inode.mode & EXT2_S_IFMT) == EXT2_S_IFDIR) {
        stat->kind = VFS_NODE_DIRECTORY;
    } else {
        stat->kind = VFS_NODE_FILE;
    }
    stat->size = inode.size;
    stat->mode = inode.mode & 0777u;  /* permission bits only */
    stat->uid  = inode.uid;
    stat->gid  = inode.gid;
    return 1;
}

int vfs_stat_path(const char *path, struct vfs_stat *stat) {
    const struct vfs_file *file;
    char normalized_path[64];
    size_t len;

    if (path == 0 || stat == 0) {
        return 0;
    }

    len = string_length(path);
    if (len >= sizeof(normalized_path)) return 0;
    
    strcpy(normalized_path, path);
    if (len > 1 && normalized_path[len - 1] == '/') {
        normalized_path[len - 1] = '\0';
    }

    /* Check embedded files first */
    file = vfs_lookup(normalized_path);
    if (file != 0) {
        stat->kind = VFS_NODE_FILE;
        stat->size = vfs_file_size(file);
        return 1;
    }

    /* Try ext2 */
    if (ext2_vfs_stat(normalized_path, stat)) {
        return 1;
    }

    /* Hardcoded directories that always exist (overlay) */
    if (string_equals_ci(normalized_path, "/") ||
        string_equals_ci(normalized_path, "/bin") ||
        string_equals_ci(normalized_path, "/etc") ||
        string_equals_ci(normalized_path, "/home")) {
        stat->kind = VFS_NODE_DIRECTORY;
        stat->size = 0;
        return 1;
    }

    /* Check if it is a directory in user files (prefix match) */
    for (size_t i = 0; i < VFS_USER_MAX_FILES; ++i) {
        if (g_user_files[i].used) {
            size_t flen = string_length(g_user_files[i].path);
            size_t nlen = string_length(normalized_path);
            if (flen > nlen && g_user_files[i].path[nlen] == '/') {
                int match = 1;
                for (size_t k = 0; k < nlen; k++) {
                    if (g_user_files[i].path[k] != normalized_path[k]) { match = 0; break; }
                }
                if (match) {
                    stat->kind = VFS_NODE_DIRECTORY;
                    stat->size = 0;
                    return 1;
                }
            }
        }
    }

    return 0;
}

/* --- ext2-backed readdir --- */

/*
 * Collect unique embedded file names that live directly under `dir_path`.
 * For example, dir_path="/bin" yields {"sh","sh2","hello","windowmgr"}.
 * Returns how many names were found (up to max).
 */
static int embedded_children(const char *dir_path, const char **names_out,
                             uint32_t *kinds_out, uint64_t *sizes_out, int max) {
    int count = 0;
    size_t dir_len = string_length(dir_path);
    size_t i;

    /* Normalize: if dir_path is "/bin/" -> "/bin" */
    size_t effective_dir_len = dir_len;
    if (dir_len > 1 && dir_path[dir_len - 1] == '/') {
        effective_dir_len--;
    }

    for (i = 0; i < VFS_FILES_COUNT && count < max; ++i) {
        const char *fpath = g_vfs_files[i].path;
        const char *name = 0;

        if (effective_dir_len == 1 && dir_path[0] == '/') {
            /* Root: e.g. dir_path="/", fpath="/test.txt" -> name="test.txt" */
            name = fpath + 1;
        } else {
            /* Subdir: e.g. dir_path="/bin", fpath="/bin/sh" */
            int match = 1;
            size_t k;
            for (k = 0; k < effective_dir_len; k++) {
                if (ascii_lower(fpath[k]) != ascii_lower(dir_path[k])) { match = 0; break; }
            }
            if (match && fpath[effective_dir_len] == '/') {
                name = fpath + effective_dir_len + 1;
            }
        }

        if (name && *name != '\0') {
            /* Check for deeper nesting (no more slashes in name) */
            const char *p = name;
            int has_subdir = 0;
            while (*p) {
                if (*p == '/') { has_subdir = 1; break; }
                p++;
            }
            if (!has_subdir) {
                names_out[count] = name;
                kinds_out[count] = VFS_NODE_FILE;
                sizes_out[count] = vfs_file_size(&g_vfs_files[i]);
                count++;
            }
        }
    }
    return count;
}

/* Hardcoded top-level overlay directories */
static const char *g_overlay_dirs[] = { "bin", "etc", "home" };
#define OVERLAY_DIR_COUNT 3

static int already_listed(const char *name, const char **prev, int prev_count) {
    int i;
    for (i = 0; i < prev_count; ++i) {
        if (string_equals_ci(name, prev[i])) return 1;
    }
    return 0;
}

/* Name buffer for ext2 dir entries returned by readdir (static to avoid stack blow) */
static char g_ext2_name_buf[32][64];

int vfs_readdir(const char *path, uint32_t index, struct vfs_dir_entry *entry) {
    /*
     * Strategy: merge entries from three sources in this order:
     *   1. ext2 directory entries (skip "." and "..")
     *   2. overlay directories (for "/" only: bin, etc)
     *   3. embedded file children
     * De-duplicate by name so overlay/embedded entries win when ext2 has the same name.
     *
     * Because the caller iterates by index (0, 1, 2, …), we build the full merged
     * list each time. This is O(n) per call but the lists are tiny.
     */

    const char *merged_names[64];
    uint32_t merged_kinds[64];
    uint64_t merged_sizes[64];
    int merged_count = 0;
    int is_root;

    if (path == 0 || entry == 0) {
        return 0;
    }

    serial_write("VFS: readdir path=");
    serial_write(path);
    serial_write(" index=");
    serial_write_hex_u64(index);
    serial_write(" g_ext2=");
    serial_write_hex_u64((uint64_t)g_ext2);
    serial_write("\n");

    is_root = string_equals_ci(path, "/");

    /* 1. ext2 entries */
    if (g_ext2 != 0) {
        uint32_t ino = ext2_lookup_inode(g_ext2, path);
        serial_write("VFS: ext2 lookup ino=");
        serial_write_hex_u64(ino);
        serial_write("\n");
        if (ino != 0) {
            struct ext2_dir_entry ext2_entries[32];
            int n = ext2_readdir(g_ext2, ino, ext2_entries, 32);
            serial_write("VFS: ext2_readdir returned n=");
            serial_write_hex_u64(n);
            serial_write("\n");
            int i;
            for (i = 0; i < n && merged_count < 64; ++i) {
                /* skip . and .. */
                if (ext2_entries[i].name_len == 1 && ext2_entries[i].name[0] == '.') continue;
                if (ext2_entries[i].name_len == 2 && ext2_entries[i].name[0] == '.' && ext2_entries[i].name[1] == '.') continue;
                /* copy name to static buffer */
                {
                    size_t k;
                    for (k = 0; k < ext2_entries[i].name_len && k < 63; ++k)
                        g_ext2_name_buf[merged_count][k] = ext2_entries[i].name[k];
                    g_ext2_name_buf[merged_count][k] = '\0';
                }
                merged_names[merged_count] = g_ext2_name_buf[merged_count];
                merged_kinds[merged_count] = (ext2_entries[i].file_type == 2) ? VFS_NODE_DIRECTORY : VFS_NODE_FILE;
                merged_sizes[merged_count] = 0;
                merged_count++;
            }
        }
    }

    /* 2. overlay directories at root */
    if (is_root) {
        int i;
        for (i = 0; i < OVERLAY_DIR_COUNT && merged_count < 64; ++i) {
            if (!already_listed(g_overlay_dirs[i], merged_names, merged_count)) {
                merged_names[merged_count] = g_overlay_dirs[i];
                merged_kinds[merged_count] = VFS_NODE_DIRECTORY;
                merged_sizes[merged_count] = 0;
                merged_count++;
            }
        }
    }

    /* 3. embedded file children */
    {
        const char *emb_names[16];
	uint32_t emb_kinds[16];
	uint64_t emb_sizes[16];
	int emb_count = embedded_children(path, emb_names, emb_kinds, emb_sizes, 16);
	int i;
	for (i = 0; i < emb_count && merged_count < 64; ++i) {
		if (!already_listed(emb_names[i], merged_names, merged_count)) {
			merged_names[merged_count] = emb_names[i];
			merged_kinds[merged_count] = emb_kinds[i];
			merged_sizes[merged_count] = emb_sizes[i];
			merged_count++;
		}
	}
}

/* 4. User files */
{
	size_t ui;
	size_t path_len = string_length(path);
	size_t effective_path_len = path_len;
	if (path_len > 1 && path[path_len - 1] == '/') {
		effective_path_len--;
	}

	for (ui = 0; ui < VFS_USER_MAX_FILES && merged_count < 64; ++ui) {
		if (g_user_files[ui].used) {
			const char *fpath = g_user_files[ui].path;
			const char *name = 0;

			if (effective_path_len == 1 && path[0] == '/') {
				/* Root: e.g. path="/", fpath="/test.txt" -> name="test.txt" */
				if (fpath[0] == '/') {
					name = fpath + 1;
				}
			} else {
				/* Subdir: e.g. path="/home", fpath="/home/test.txt" */
				int match = 1;
				size_t k;
				for (k = 0; k < effective_path_len; k++) {
					if (fpath[k] != path[k]) { match = 0; break; }
				}
				if (match && fpath[effective_path_len] == '/') {
					name = fpath + effective_path_len + 1;
				}
			}

			if (name && *name != '\0') {
				/* Check for deeper nesting (no more slashes in name) */
				int has_subdir = 0;
				for (size_t k = 0; name[k]; k++) {
					if (name[k] == '/') { has_subdir = 1; break; }
				}
				if (!has_subdir && !already_listed(name, merged_names, merged_count)) {
					merged_names[merged_count] = name;
					merged_kinds[merged_count] = VFS_NODE_FILE;
					merged_sizes[merged_count] = g_user_files[ui].size;
					merged_count++;
				}
			}
		}
	}
}

/* Return the entry at `index` */
    serial_write("VFS: readdir merged_count=");
    serial_write_hex_u64(merged_count);
    serial_write(" index=");
    serial_write_hex_u64(index);
    serial_write("\n");
    {
        int i;
        for (i = 0; i < merged_count; i++) {
            serial_write("VFS:  entry[");
            serial_write_hex_u64(i);
            serial_write("]=");
            serial_write(merged_names[i]);
            serial_write("\n");
        }
    }

	if (index < (uint32_t)merged_count) {
		entry->name = merged_names[index];
		entry->kind = merged_kinds[index];
		entry->size = merged_sizes[index];
		return 1;
	}

	return 0;
}

static struct vfs_user_file *find_user_file(const char *path) {
	size_t i;
	for (i = 0; i < VFS_USER_MAX_FILES; ++i) {
		if (g_user_files[i].used && strcmp(path, g_user_files[i].path) == 0) {
			return &g_user_files[i];
		}
	}
	return 0;
}

/* Forward declaration — defined near the bottom of this file with the other
 * permission helpers so they all live in one place. */
static int vfs_access_ok(const char *path, int want);

/*
 * Extract the parent directory of a path into buf.
 * "/foo/bar" -> "/foo", "/file" -> "/", "/" -> "/".
 * This lets permission checks target the directory that will be modified,
 * which is the POSIX rule: write on the directory, not on the file itself.
 */
static void parent_dir(const char *path, char *buf, size_t cap) {
    size_t len = string_length(path);
    size_t last_slash = 0;
    size_t i;

    /* Find the last '/' — everything before it is the parent. */
    for (i = 0; i < len; ++i) {
        if (path[i] == '/') last_slash = i;
    }

    if (last_slash == 0) {
        /* The parent is root (e.g. path="/file"). */
        buf[0] = '/';
        buf[1] = '\0';
        return;
    }

    if (last_slash >= cap - 1) last_slash = cap - 2;
    for (i = 0; i < last_slash && i < cap - 1; ++i)
        buf[i] = path[i];
    buf[i] = '\0';
}

int vfs_create(const char *path) {
    if (path == 0 || string_length(path) >= 64) {
        return -1;
    }

    /* Check write+execute on the parent directory so unprivileged processes
     * cannot create files in directories they don't own (e.g. /bin, /etc). */
    if (g_ext2 != 0) {
        char parent[64];
        parent_dir(path, parent, sizeof(parent));
        if (!vfs_access_ok(parent, 06)) {
            return -SYSCALL_EACCES;
        }
    }

    /* When a persistent ext2 filesystem is mounted it is authoritative:
     * never create a RAM-overlay entry, or it would shadow the real file on
     * reads (vfs_read checks the overlay before ext2). */
    if (g_ext2 != 0 && path[0] == '/') {
        if (ext2_lookup_inode(g_ext2, path) != 0) {
            return 0; /* already exists */
        }
        return ext2_create(g_ext2, path, 0644) != 0 ? 0 : -1;
    }

    /* Fall back to user files (only used when no disk is mounted) */
    if (find_user_file(path) != 0) {
        return 0;
    }
    for (size_t i = 0; i < VFS_USER_MAX_FILES; ++i) {
        if (!g_user_files[i].used) {
            strcpy(g_user_files[i].path, path);
            g_user_files[i].size = 0;
            g_user_files[i].used = 1;
            return 0;
        }
    }
    return -1;
}

int vfs_unlink(const char *path) {
	struct vfs_user_file *f;

	/* Removing an entry modifies the directory, so check write+execute on
	 * the parent — same rule as vfs_create. */
	if (g_ext2 != 0 && path != 0) {
		char parent[64];
		parent_dir(path, parent, sizeof(parent));
		if (!vfs_access_ok(parent, 06)) {
			return -SYSCALL_EACCES;
		}
	}

	/* Prefer the persistent ext2 filesystem when mounted. */
	if (g_ext2 != 0 && path != 0 && path[0] == '/') {
		if (ext2_unlink(g_ext2, path) == 0) {
			return 0;
		}
	}

	f = find_user_file(path);
	if (f == 0) {
		return -1;
	}
	f->used = 0;
	return 0;
}

int vfs_mkdir(const char *path) {
	if (path == 0 || path[0] != '/') {
		return -1;
	}
	/* Creating a directory appends an entry to the parent directory,
	 * so the caller needs write+execute on the parent. */
	if (g_ext2 != 0) {
		char parent[64];
		parent_dir(path, parent, sizeof(parent));
		if (!vfs_access_ok(parent, 06)) {
			return -SYSCALL_EACCES;
		}
		if (ext2_mkdir(g_ext2, path, 0755) != 0) {
			return 0;
		}
		return -1;
	}
	/* No writable filesystem mounted. */
	return -1;
}

ssize_t vfs_write(const char *path, size_t offset, const void *data, size_t count) {
	/* 1. Try ext2 */
	if (g_ext2 != 0 && path[0] == '/') {
		uint32_t ino = ext2_lookup_inode(g_ext2, path);
		if (ino == 0) {
			ino = ext2_create(g_ext2, path, 0644);
		}
		if (ino != 0) {
			return ext2_write(g_ext2, ino, (uint32_t)offset, count, data);
		}
	}

	/* 2. Fallback to RAM user files */
	struct vfs_user_file *f = find_user_file(path);
	size_t i;
	if (f == 0) {
		if (vfs_create(path) < 0) {
			return -1;
		}
		f = find_user_file(path);
		if (f == 0) {
			return -1;
		}
	}
	if (offset + count > VFS_USER_MAX_SIZE) {
		count = VFS_USER_MAX_SIZE - offset;
	}
	for (i = 0; i < count; ++i) {
		f->data[offset + i] = ((const uint8_t *)data)[i];
	}
	if (offset + count > f->size) {
		f->size = offset + count;
	}
	return (ssize_t)count;
}

int vfs_file_exists(const char *path) {
    struct vfs_stat stat;
    return vfs_stat_path(path, &stat) && stat.kind == VFS_NODE_FILE;
}

/* Single-traversal open helper: checks existence AND returns ext2 inode number.
 * Returns 1 if the file exists; *ino_out = inode (0 for embedded/user files). */
int vfs_open_ino(const char *path, uint32_t *ino_out) {
    const struct vfs_file *embedded;
    struct vfs_user_file *uf;

    if (ino_out) *ino_out = 0u;
    if (path == 0) return 0;

    /* Embedded files: in ROM, no disk I/O, inode = 0 */
    embedded = vfs_lookup(path);
    if (embedded != 0) return 1;

    /* Ext2 files: one directory traversal, capture the inode */
    if (g_ext2 != 0) {
        uint32_t ino = ext2_lookup_inode(g_ext2, path);
        if (ino != 0) {
            if (ino_out) *ino_out = ino;
            return 1;
        }
    }

    /* User (in-memory) files: no disk I/O, inode = 0 */
    uf = find_user_file(path);
    if (uf != 0) return 1;

    return 0;
}

ssize_t vfs_read(const char *path, size_t offset, void *buffer, size_t count) {
    const struct vfs_file *file;
    struct vfs_user_file *uf;

    if (path == 0 || buffer == 0) return -1;

    /* 1. Embedded files */
    file = vfs_lookup(path);
    if (file != 0) {
        size_t file_size = vfs_file_size(file);
        if (offset >= file_size) return 0;
        size_t avail = file_size - offset;
        size_t to_copy = count < avail ? count : avail;
        size_t i;
        for (i = 0; i < to_copy; ++i)
            ((uint8_t *)buffer)[i] = file->data[offset + i];
        return (ssize_t)to_copy;
    }

    /* 2. User files */
    uf = find_user_file(path);
    if (uf != 0) {
        return vfs_read_user(path, offset, buffer, count);
    }

    /* 3. Ext2 */
    if (g_ext2 != 0) {
        uint32_t ino = ext2_lookup_inode(g_ext2, path);
        if (ino != 0) {
            return ext2_read(g_ext2, ino, (uint32_t)offset, count, buffer);
        }
    }

    return -1;
}

ssize_t vfs_write_all(const char *path, const void *data, size_t count) {
    if (path == 0) return -1;

    /* 1. Try ext2 */
    if (g_ext2 != 0) {
        uint32_t ino = ext2_lookup_inode(g_ext2, path);
        if (ino == 0) {
            ino = ext2_create(g_ext2, path, 0644);
        }
        if (ino != 0) {
            return ext2_write(g_ext2, ino, 0, count, data);
        }
    }

    /* 2. Fallback: user files (truncate first) */
    {
        struct vfs_user_file *f = find_user_file(path);
        if (f == 0) {
            if (vfs_create(path) < 0) return -1;
            f = find_user_file(path);
            if (f == 0) return -1;
        }
        f->size = 0; /* truncate */
        return vfs_write(path, 0, data, count);
    }
}

ssize_t vfs_read_user(const char *path, size_t offset, void *buffer, size_t count) {
	struct vfs_user_file *f = find_user_file(path);
	size_t to_copy;
	size_t i;
	if (f == 0 || buffer == 0) {
		return -1;
	}
	if (offset >= f->size) {
		return 0;
	}
	to_copy = count;
	if (offset + to_copy > f->size) {
		to_copy = f->size - offset;
	}
	for (i = 0; i < to_copy; ++i) {
		((uint8_t *)buffer)[i] = f->data[offset + i];
	}
	return (ssize_t)to_copy;
}

/* ---- Permission helpers ------------------------------------------------- */

/*
 * Check whether a process with the given uid may perform an operation on an
 * inode.  'want' is a combination of 04 (read), 02 (write), 01 (execute).
 *
 * The three standard Unix permission tiers are checked in order:
 *   owner bits  (shift 6) when uid == inode.uid
 *   other bits  (shift 0) otherwise
 * Group bits are omitted — groups are not yet tracked per-process.
 *
 * Root (uid 0) always has full access.
 */
static int perm_ok(const struct ext2_inode *inode, uint32_t uid, int want) {
    uint16_t mode = inode->mode & 0777u;

    if (uid == 0)
        return 1;  /* root is omnipotent */

    int shift = (uid == (uint32_t)inode->uid) ? 6 : 0;
    return ((mode >> shift) & want) == (unsigned)want;
}

int vfs_chmod(const char *path, uint16_t mode) {
    uint32_t ino;
    struct ext2_inode *ip;
    uint32_t caller_uid = process_current_uid();

    if (!g_ext2 || !path) return -1;

    ino = ext2_lookup_inode(g_ext2, path);
    if (ino == 0) return -2;  /* ENOENT */

    /* Work directly on the in-memory inode table entry so we can write it
     * back with a single ext2_write_inode() call. */
    ip = &g_ext2->inode_table[ino - 1];

    /* Only the file owner or root may change permission bits (POSIX rule). */
    if (caller_uid != 0 && caller_uid != (uint32_t)ip->uid)
        return -1;  /* EPERM */

    /* Preserve the file-type bits in the high nibble; update only the nine
     * rwxrwxrwx permission bits in the low twelve bits. */
    ip->mode = (ip->mode & ~(uint16_t)0777u) | (mode & 0777u);
    ext2_write_inode(g_ext2, ino);
    return 0;
}

int vfs_chown(const char *path, uint16_t uid, uint16_t gid) {
    uint32_t ino;
    struct ext2_inode *ip;
    uint32_t caller_uid = process_current_uid();

    if (!g_ext2 || !path) return -1;

    ino = ext2_lookup_inode(g_ext2, path);
    if (ino == 0) return -2;  /* ENOENT */

    /* Only root may reassign file ownership — same rule as Linux. */
    if (caller_uid != 0) return -1;  /* EPERM */

    ip = &g_ext2->inode_table[ino - 1];
    ip->uid = uid;
    ip->gid = gid;
    ext2_write_inode(g_ext2, ino);
    return 0;
}

/*
 * Enforce read/write permission for the current process on 'path'.
 * Returns 1 if access is allowed, 0 if denied.
 * Skips the check when no ext2 filesystem is mounted (RAM-only mode).
 */
static int vfs_access_ok(const char *path, int want) {
    uint32_t ino;
    struct ext2_inode inode;

    if (!g_ext2) return 1;  /* no persistent fs — always allow */

    ino = ext2_lookup_inode(g_ext2, path);
    if (ino == 0) return 1;  /* doesn't exist yet; let the caller handle ENOENT */

    if (ext2_stat(g_ext2, ino, &inode) < 0) return 1;

    return perm_ok(&inode, process_current_uid(), want);
}
