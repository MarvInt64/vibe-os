/* umalloc — first-fit free-list allocator over a static arena. See umalloc.h. */
#include "umalloc.h"

#define UMALLOC_ARENA_BYTES (16u * 1024u * 1024u)  /* 16 MB */
#define UMALLOC_ALIGN 16u

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

static unsigned char g_arena[UMALLOC_ARENA_BYTES] __attribute__((aligned(UMALLOC_ALIGN)));
static struct block *g_head;   /* first block; 0 until initialised */
static umsize_t g_used;

static umsize_t align_up(umsize_t n) {
    return (n + (UMALLOC_ALIGN - 1u)) & ~((umsize_t)UMALLOC_ALIGN - 1u);
}

static void umalloc_init(void) {
    g_head = (struct block *)g_arena;
    g_head->size = UMALLOC_ARENA_BYTES - sizeof(struct block);
    g_head->prev = 0;
    g_head->next = 0;
    g_head->free = 1;
    g_head->_pad = 0;
    g_used = 0;
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
    }
}

void *umalloc(umsize_t size) {
    struct block *b;
    umsize_t need;

    if (size == 0) {
        return 0;
    }
    if (g_head == 0) {
        umalloc_init();
    }
    need = align_up(size);

    for (b = g_head; b != 0; b = b->next) {
        if (b->free && b->size >= need) {
            split_block(b, need);
            b->free = 0;
            g_used += b->size;
            return payload_of(b);
        }
    }
    return 0; /* out of arena */
}

void ufree(void *ptr) {
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

void *urealloc(void *ptr, umsize_t size) {
    struct block *b;
    umsize_t need;
    void *np;

    if (ptr == 0) {
        return umalloc(size);
    }
    if (size == 0) {
        ufree(ptr);
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
    np = umalloc(size);
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
    ufree(ptr);
    return np;
}

umsize_t umalloc_used(void) { return g_used; }
umsize_t umalloc_capacity(void) { return UMALLOC_ARENA_BYTES; }
