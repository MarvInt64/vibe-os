/* pty.c — pseudo-terminal transport (see pty.h). */
#include "pty.h"
#include "syscall.h"

#define PTY_POOL_SIZE   4
#define PTY_RING_SIZE   16384u   /* per direction; must be a power of two */
#define PTY_RING_MASK   (PTY_RING_SIZE - 1u)

/* A lock-free-enough single-producer/single-consumer byte ring. Each direction
 * has exactly one writer and one reader (master vs slave), and the kernel runs
 * these ops non-preemptively per syscall, so plain head/tail indices suffice. */
struct pty_ring {
    unsigned char data[PTY_RING_SIZE];
    size_t head;   /* next write position */
    size_t tail;   /* next read position  */
};

struct pty {
    struct pty_ring in;    /* master -> slave (process stdin)  */
    struct pty_ring out;   /* slave  -> master (process stdout) */
    uint8_t used;
};

static struct pty g_ptys[PTY_POOL_SIZE];

static void ring_reset(struct pty_ring *r) {
    r->head = 0;
    r->tail = 0;
}

static size_t ring_used(const struct pty_ring *r) {
    return r->head - r->tail;
}

/* Append up to count bytes; returns the number actually stored (the rest is
 * dropped when the ring is full — terminals drain continuously so this is rare
 * and never blocks a writer). */
static size_t ring_write(struct pty_ring *r, const unsigned char *src, size_t count) {
    size_t free_space = PTY_RING_SIZE - ring_used(r);
    size_t i;

    if (count > free_space) {
        count = free_space;
    }
    for (i = 0; i < count; ++i) {
        r->data[r->head & PTY_RING_MASK] = src[i];
        ++r->head;
    }
    return count;
}

/* Drain up to count bytes; returns the number copied (0 when empty). */
static size_t ring_read(struct pty_ring *r, unsigned char *dst, size_t count) {
    size_t avail = ring_used(r);
    size_t i;

    if (count > avail) {
        count = avail;
    }
    for (i = 0; i < count; ++i) {
        dst[i] = r->data[r->tail & PTY_RING_MASK];
        ++r->tail;
    }
    return count;
}

struct pty *pty_alloc(void) {
    int i;

    for (i = 0; i < PTY_POOL_SIZE; ++i) {
        if (!g_ptys[i].used) {
            g_ptys[i].used = 1;
            ring_reset(&g_ptys[i].in);
            ring_reset(&g_ptys[i].out);
            return &g_ptys[i];
        }
    }
    return 0;
}

void pty_release(struct pty *pty) {
    if (pty == 0) {
        return;
    }
    pty->used = 0;
}

/* ---- master endpoint (terminal emulator) ---------------------------------- */

static ssize_t pty_master_read(void *object, void *buffer, size_t count) {
    struct pty *pty = (struct pty *)object;

    if (pty == 0 || buffer == 0 || count == 0) {
        return 0;
    }
    return (ssize_t)ring_read(&pty->out, (unsigned char *)buffer, count);
}

static ssize_t pty_master_write(void *object, const void *buffer, size_t count) {
    struct pty *pty = (struct pty *)object;

    if (pty == 0 || buffer == 0) {
        return -SYSCALL_EINVAL;
    }
    return (ssize_t)ring_write(&pty->in, (const unsigned char *)buffer, count);
}

/* ---- slave endpoint (child process stdin/stdout) -------------------------- */

static ssize_t pty_slave_read(void *object, void *buffer, size_t count) {
    struct pty *pty = (struct pty *)object;

    if (pty == 0 || buffer == 0 || count == 0) {
        return 0;
    }
    return (ssize_t)ring_read(&pty->in, (unsigned char *)buffer, count);
}

static ssize_t pty_slave_write(void *object, const void *buffer, size_t count) {
    struct pty *pty = (struct pty *)object;

    if (pty == 0 || buffer == 0) {
        return -SYSCALL_EINVAL;
    }
    return (ssize_t)ring_write(&pty->out, (const unsigned char *)buffer, count);
}

const struct fd_ops PTY_MASTER_FD_OPS = {
    pty_master_read,
    pty_master_write,
    0
};

const struct fd_ops PTY_SLAVE_FD_OPS = {
    pty_slave_read,
    pty_slave_write,
    0
};
