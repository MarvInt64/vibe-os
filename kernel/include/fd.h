#ifndef VIBEOS_FD_H
#define VIBEOS_FD_H

#include "types.h"

#define FD_TABLE_CAPACITY 16

struct fd_ops {
    ssize_t (*read)(void *object, void *buffer, size_t count);
    ssize_t (*write)(void *object, const void *buffer, size_t count);
    int (*ioctl)(void *object, uint32_t request, uintptr_t arg);
};

struct fd_entry {
    const struct fd_ops *ops;
    void *object;
    uint8_t used;
};

struct fd_table {
    struct fd_entry entries[FD_TABLE_CAPACITY];
};

void fd_table_init(struct fd_table *table);
int fd_bind(struct fd_table *table, int fd, const struct fd_ops *ops, void *object);
int fd_alloc(struct fd_table *table, const struct fd_ops *ops, void *object, int start_fd);
int fd_close(struct fd_table *table, int fd);
ssize_t fd_read(struct fd_table *table, int fd, void *buffer, size_t count);
ssize_t fd_write(struct fd_table *table, int fd, const void *buffer, size_t count);
int fd_ioctl(struct fd_table *table, int fd, uint32_t request, uintptr_t arg);

#endif
