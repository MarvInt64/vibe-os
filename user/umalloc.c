/* umalloc — first-fit free-list allocator over SYS_SBRK heap pages. */
#include "umalloc.h"
#include <sys/syscall.h>

/* ---- Thread safety -------------------------------------------------------
 * Multiple threads (e.g. a browser UI thread and a network worker) share one
 * address space and therefore one heap. A yielding spinlock serialises every
 * allocator entry point so the free list never corrupts under preemption.
 * urealloc is built on the *_nolock helpers to avoid re-entering the lock. */
extern void vos_yield(void);
static volatile unsigned char g_alloc_lock;

static void alloc_lock(void) {
    while (__atomic_test_and_set(&g_alloc_lock, __ATOMIC_ACQUIRE))
        vos_yield();
}
static void alloc_unlock(void) {
    __atomic_clear(&g_alloc_lock, __ATOMIC_RELEASE);
}

#define UMALLOC_ALIGN 16u
#define UMALLOC_GROW_MIN (64u * 1024u)

/* Address-ordered, doubly linked list of all blocks (free and used). Splitting
 * and coalescing keep the list contiguous so neighbours can always be merged.
 * The header is padded to the alignment so payloads stay 16-byte aligned. */
struct block {
    umsize_t size;          /* payload bytes (excludes this header) */
    struct block *prev;     /* previous block in address order, or 0 */
    struct block *next;     /* next block in address order, or 0 */
    unsigned int free;      /* 1 = available */
    unsigned int _pad;      /* keep sizeof(struct block) a multiple of 16 */
};

static struct block *g_head;   /* first block; 0 until initialised */
static struct block *g_tail;
static umsize_t g_used;
static umsize_t g_capacity;

static umsize_t align_up(umsize_t n) {
    return (n + (UMALLOC_ALIGN - 1u)) & ~((umsize_t)UMALLOC_ALIGN - 1u);
}

static void coalesce_forward(struct block *b);

static umsize_t page_align(umsize_t n) {
    return (n + 0xfffu) & ~(umsize_t)0xfffu;
}

static int grow_heap(umsize_t need) {
    umsize_t bytes = need + sizeof(struct block);
    struct block *b;
    void *base;

    if (bytes < UMALLOC_GROW_MIN) {
        bytes = UMALLOC_GROW_MIN;
    }
    bytes = page_align(bytes);
    base = (void *)(unsigned long)__sc1(SYS_SBRK, (uint64_t)bytes);
    if ((unsigned long)base == ~(unsigned long)0) {
        return 0;
    }

    b = (struct block *)base;
    b->size = bytes - sizeof(struct block);
    b->prev = g_tail;
    b->next = 0;
    b->free = 1;
    b->_pad = 0;
    if (g_tail) {
        g_tail->next = b;
    } else {
        g_head = b;
    }
    g_tail = b;
    g_capacity += b->size;

    if (b->prev && b->prev->free) {
        b = b->prev;
        coalesce_forward(b);
        if (b->next == 0) {
            g_tail = b;
        }
    }
    return 1;
}

static void *payload_of(struct block *b) {
    return (void *)((unsigned char *)b + sizeof(struct block));
}

static struct block *block_of(void *p) {
    return (struct block *)((unsigned char *)p - sizeof(struct block));
}

/* Split `b` so it holds exactly `need` payload bytes, turning the remainder
 * into a new free block when there is room for a header + a little payload. */
static void split_block(struct block *b, umsize_t need) {
    umsize_t remain;
    if (b->size < need + sizeof(struct block) + UMALLOC_ALIGN) {
        return; /* not worth splitting */
    }
    remain = b->size - need - sizeof(struct block);
    {
        struct block *rest = (struct block *)((unsigned char *)payload_of(b) + need);
        rest->size = remain;
        rest->free = 1;
        rest->_pad = 0;
        rest->prev = b;
        rest->next = b->next;
        if (rest->next) rest->next->prev = rest;
        b->next = rest;
        b->size = need;
    }
}

/* Merge `b` with its next neighbour while that neighbour is free. */
static void coalesce_forward(struct block *b) {
    while (b->next && b->next->free) {
        struct block *n = b->next;
        b->size += sizeof(struct block) + n->size;
        b->next = n->next;
        if (b->next) b->next->prev = b;
        else g_tail = b;
    }
}

static void *umalloc_nolock(umsize_t size) {
    struct block *b;
    umsize_t need;

    if (size == 0) {
        return 0;
    }
    need = align_up(size);

    for (;;) {
        for (b = g_head; b != 0; b = b->next) {
            if (b->free && b->size >= need) {
                split_block(b, need);
                b->free = 0;
                g_used += b->size;
                return payload_of(b);
            }
        }
        if (!grow_heap(need)) {
            return 0;
        }
    }
}

static void ufree_nolock(void *ptr) {
    struct block *b;
    if (ptr == 0) {
        return;
    }
    b = block_of(ptr);
    if (b->free) {
        return; /* double free — ignore */
    }
    b->free = 1;
    g_used -= b->size;
    coalesce_forward(b);
    if (b->prev && b->prev->free) {
        b = b->prev;
        coalesce_forward(b);
    }
}

static void *urealloc_nolock(void *ptr, umsize_t size) {
    struct block *b;
    umsize_t need;
    void *np;

    if (ptr == 0) {
        return umalloc_nolock(size);
    }
    if (size == 0) {
        ufree_nolock(ptr);
        return 0;
    }
    b = block_of(ptr);
    need = align_up(size);

    if (b->size >= need) {
        return ptr; /* shrink-in-place; keep the slack */
    }
    /* Try to grow into a free following neighbour without copying. */
    if (b->next && b->next->free &&
        b->size + sizeof(struct block) + b->next->size >= need) {
        g_used -= b->size;
        coalesce_forward(b);
        split_block(b, need);
        b->free = 0;
        g_used += b->size;
        return payload_of(b);
    }
    /* Fall back to allocate-copy-free. */
    np = umalloc_nolock(size);
    if (np == 0) {
        return 0;
    }
    {
        umsize_t i;
        const unsigned char *src = (const unsigned char *)ptr;
        unsigned char *dst = (unsigned char *)np;
        for (i = 0; i < b->size && i < need; ++i) {
            dst[i] = src[i];
        }
    }
    ufree_nolock(ptr);
    return np;
}

/* ---- Public, lock-guarded entry points -------------------------------- */
void *umalloc(umsize_t size) {
    void *p;
    alloc_lock();
    p = umalloc_nolock(size);
    alloc_unlock();
    return p;
}
void ufree(void *ptr) {
    alloc_lock();
    ufree_nolock(ptr);
    alloc_unlock();
}
void *urealloc(void *ptr, umsize_t size) {
    void *p;
    alloc_lock();
    p = urealloc_nolock(ptr, size);
    alloc_unlock();
    return p;
}

umsize_t umalloc_used(void) { return g_used; }
umsize_t umalloc_capacity(void) { return g_capacity; }
