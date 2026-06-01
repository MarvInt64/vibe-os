#include "fd.h"
#include "syscall.h"

void fd_table_init(struct fd_table *table) {
    int i;

    for (i = 0; i < FD_TABLE_CAPACITY; ++i) {
        table->entries[i].ops = 0;
        table->entries[i].object = 0;
        table->entries[i].used = 0;
    }
}

int fd_bind(struct fd_table *table, int fd, const struct fd_ops *ops, void *object) {
    if (table == 0 || ops == 0 || fd < 0 || fd >= FD_TABLE_CAPACITY) {
        return -SYSCALL_EINVAL;
    }

    table->entries[fd].ops = ops;
    table->entries[fd].object = object;
    table->entries[fd].used = 1;
    return fd;
}

int fd_alloc(struct fd_table *table, const struct fd_ops *ops, void *object, int start_fd) {
    int fd;

    if (table == 0 || ops == 0 || start_fd < 0) {
        return -SYSCALL_EINVAL;
    }

    for (fd = start_fd; fd < FD_TABLE_CAPACITY; ++fd) {
        if (!table->entries[fd].used) {
            table->entries[fd].ops = ops;
            table->entries[fd].object = object;
            table->entries[fd].used = 1;
            return fd;
        }
    }

    return -SYSCALL_EMFILE;
}

int fd_close(struct fd_table *table, int fd) {
    if (table == 0 || fd < 0 || fd >= FD_TABLE_CAPACITY || !table->entries[fd].used) {
        return -SYSCALL_EBADF;
    }

    table->entries[fd].ops = 0;
    table->entries[fd].object = 0;
    table->entries[fd].used = 0;
    return 0;
}

ssize_t fd_read(struct fd_table *table, int fd, void *buffer, size_t count) {
    if (table == 0 || fd < 0 || fd >= FD_TABLE_CAPACITY || !table->entries[fd].used || table->entries[fd].ops == 0 || table->entries[fd].ops->read == 0) {
        return -SYSCALL_EBADF;
    }

    return table->entries[fd].ops->read(table->entries[fd].object, buffer, count);
}

ssize_t fd_write(struct fd_table *table, int fd, const void *buffer, size_t count) {
    if (table == 0 || fd < 0 || fd >= FD_TABLE_CAPACITY || !table->entries[fd].used || table->entries[fd].ops == 0 || table->entries[fd].ops->write == 0) {
        return -SYSCALL_EBADF;
    }

    return table->entries[fd].ops->write(table->entries[fd].object, buffer, count);
}

int fd_ioctl(struct fd_table *table, int fd, uint32_t request, uintptr_t arg) {
    if (table == 0 || fd < 0 || fd >= FD_TABLE_CAPACITY || !table->entries[fd].used || table->entries[fd].ops == 0 || table->entries[fd].ops->ioctl == 0) {
        return -SYSCALL_EBADF;
    }

    return table->entries[fd].ops->ioctl(table->entries[fd].object, request, arg);
}
