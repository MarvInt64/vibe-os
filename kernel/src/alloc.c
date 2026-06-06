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

#include "alloc.h"
#include "string.h"
#include "paging.h"
#include "tty.h"
#include "serial.h"
#include "spinlock.h"
#include <stddef.h>

/* The kernel heap is shared by every CPU, so the free-list walk in kmalloc /
 * kfree must be serialised. irqsave is used so a heap access from an interrupt
 * handler on a CPU already holding the lock in process context cannot deadlock
 * against itself. */
static spinlock_t g_heap_lock = SPINLOCK_INIT;

/*
 * Simple header for each allocated block
 * Contains metadata about the block size and whether it's free
 */
typedef struct alloc_header {
    size_t size;           /* Size of this allocation (excluding header) */
    struct alloc_header *next; /* Next block in linked list */
    int is_free;          /* 1 if free, 0 if allocated */
} alloc_header_t;

static alloc_header_t *g_heap_start = NULL;
static uintptr_t g_heap_base = 0;
static size_t g_heap_size = 0;
static size_t g_used = 0;            /* payload bytes currently allocated */
static size_t g_physical_total = 0; /* total usable physical RAM (for sysinfo) */

static uintptr_t heap_end_addr(void) {
    return g_heap_base + g_heap_size;
}

static int block_in_heap(const alloc_header_t *block) {
    uintptr_t p = (uintptr_t)block;
    if (p < g_heap_base || p + sizeof(alloc_header_t) > heap_end_addr()) {
        return 0;
    }
    if (block->size > heap_end_addr() - p - sizeof(alloc_header_t)) {
        return 0;
    }
    return 1;
}

static alloc_header_t *block_phys_next(alloc_header_t *block) {
    return (alloc_header_t *)((uint8_t *)block + sizeof(alloc_header_t) + block->size);
}

/*
 * Initialize kernel heap memory allocator
 * @param heap_start: Virtual address where heap starts
 * @param heap_size: Total size of heap in bytes
 */
void kmalloc_init(uintptr_t heap_start, size_t heap_size) {
    g_heap_base = heap_start;
    g_heap_size = heap_size;
    g_used = 0;

    /* Create first free block occupying entire heap */
    g_heap_start = (alloc_header_t *)heap_start;
    g_heap_start->size = heap_size - sizeof(alloc_header_t);
    g_heap_start->next = NULL;
    g_heap_start->is_free = 1;
}

/*
 * Allocate memory from kernel heap using first-fit algorithm
 * @param size: Number of bytes to allocate
 * @return Pointer to allocated memory or NULL on failure
 */
static void *kmalloc_locked(size_t size) {
    alloc_header_t *curr;

    /* Align size to 8 bytes for better performance */
    size = (size + 7) & ~7;

    curr = g_heap_start;

    /* Find first sufficiently large free block (first-fit) */
    while (curr != NULL) {
        if (!block_in_heap(curr)) {
            break;
        }
        if (curr->is_free && curr->size >= size) {
            break;
        }
        curr = curr->next;
    }

    /* No suitable block found */
    if (curr == NULL) {
        return NULL;
    }

    /* If block is larger than needed, split it */
    if (curr->size > (size + sizeof(alloc_header_t) + 8)) {
        alloc_header_t *new_block;

        /* Create new block after allocated portion */
        new_block = (alloc_header_t *)((uint8_t *)curr + sizeof(alloc_header_t) + size);
        new_block->size = curr->size - size - sizeof(alloc_header_t);
        new_block->next = curr->next;
        new_block->is_free = 1;

        curr->size = size;
        curr->next = new_block;
    }

    curr->is_free = 0;
    g_used += curr->size;

    /* Return pointer after header */
    return (void *)((uint8_t *)curr + sizeof(alloc_header_t));
}

/*
 * Mark block as free and coalesce with adjacent free blocks
 * @param ptr: Pointer to memory to free
 */
static void kfree_locked(void *ptr) {
    alloc_header_t *block;
    alloc_header_t *curr;

    if (ptr == NULL) {
        return;
    }

    /* Get block header from user pointer */
    block = (alloc_header_t *)((uint8_t *)ptr - sizeof(alloc_header_t));
    if (!block_in_heap(block)) {
        return;
    }
    if (block->is_free) {
        return;   /* double free — ignore */
    }
    block->is_free = 1;
    if (g_used >= block->size) g_used -= block->size; else g_used = 0;

    /* Coalesce: merge adjacent free blocks */
    curr = g_heap_start;
    while (curr != NULL && curr->next != NULL) {
        if (!block_in_heap(curr) || !block_in_heap(curr->next)) {
            return;
        }
        if (curr->is_free && curr->next->is_free &&
            block_phys_next(curr) == curr->next) {
            /* Merge current and next block */
            curr->size += sizeof(alloc_header_t) + curr->next->size;
            curr->next = curr->next->next;
        } else {
            curr = curr->next;
        }
    }
}

/* ---- public, SMP-safe entry points -------------------------------------- *
 * Thin wrappers that take the heap lock around the (unchanged) allocator core,
 * so any CPU may allocate or free concurrently. */

void *kmalloc(size_t size) {
    unsigned long flags = spin_lock_irqsave(&g_heap_lock);
    void *p = kmalloc_locked(size);
    spin_unlock_irqrestore(&g_heap_lock, flags);
    return p;
}

void kfree(void *ptr) {
    unsigned long flags = spin_lock_irqsave(&g_heap_lock);
    kfree_locked(ptr);
    spin_unlock_irqrestore(&g_heap_lock, flags);
}

/*
 * Get total free memory in heap
 * @return Number of free bytes
 */
size_t kmalloc_get_free(void) {
    alloc_header_t *curr;
    size_t free_bytes = 0;
    unsigned long flags = spin_lock_irqsave(&g_heap_lock);

    curr = g_heap_start;
    while (curr != NULL) {
        if (!block_in_heap(curr)) {
            break;
        }
        if (curr->is_free) {
            free_bytes += curr->size + sizeof(alloc_header_t);
        }
        curr = curr->next;
    }

    spin_unlock_irqrestore(&g_heap_lock, flags);
    return free_bytes;
}

size_t kmalloc_get_total(void) {
    return g_heap_size;
}

size_t kmalloc_get_used(void) {
    return g_used;
}

void kmalloc_debug_dump(void) {
    alloc_header_t *curr;
    size_t blocks = 0, free_bytes = 0, used_bytes = 0;
    int truncated = 0;
    unsigned long flags = spin_lock_irqsave(&g_heap_lock);

    serial_write("HEAPDUMP base=");
    serial_write_hex_u64((uint64_t)g_heap_base);
    serial_write(" size=");
    serial_write_hex_u64((uint64_t)g_heap_size);
    serial_write(" end=");
    serial_write_hex_u64((uint64_t)heap_end_addr());
    serial_write("\n");

    {
        /* Track the four largest live blocks so a single giant allocation is
         * obvious among many small ones (average size hides that). */
        uintptr_t top_at[4] = {0,0,0,0};
        size_t top_sz[4] = {0,0,0,0};
        size_t big1m = 0; /* count of live blocks >= 1 MB */

        curr = g_heap_start;
        while (curr != NULL) {
            if (!block_in_heap(curr)) {
                serial_write("HEAPDUMP corrupt block at=");
                serial_write_hex_u64((uint64_t)(uintptr_t)curr);
                serial_write(" off=");
                serial_write_hex_u64((uint64_t)((uintptr_t)curr - g_heap_base));
                serial_write(" stranded=");
                serial_write_hex_u64((uint64_t)(heap_end_addr() - (uintptr_t)curr));
                serial_write("\n");
                truncated = 1;
                break;
            }
            blocks++;
            if (curr->is_free) {
                free_bytes += curr->size;
            } else {
                used_bytes += curr->size;
                if (curr->size >= (1u << 20)) big1m++;
                if (curr->size > top_sz[3]) {
                    int j = 3;
                    top_sz[3] = curr->size;
                    top_at[3] = (uintptr_t)curr;
                    while (j > 0 && top_sz[j] > top_sz[j-1]) {
                        size_t ts = top_sz[j-1]; uintptr_t ta = top_at[j-1];
                        top_sz[j-1] = top_sz[j]; top_at[j-1] = top_at[j];
                        top_sz[j] = ts; top_at[j] = ta;
                        j--;
                    }
                }
            }
            curr = curr->next;
        }

        serial_write("HEAPDUMP big>=1M=");
        serial_write_hex_u64((uint64_t)big1m);
        serial_write(" top4_sz=");
        serial_write_hex_u64((uint64_t)top_sz[0]); serial_write(",");
        serial_write_hex_u64((uint64_t)top_sz[1]); serial_write(",");
        serial_write_hex_u64((uint64_t)top_sz[2]); serial_write(",");
        serial_write_hex_u64((uint64_t)top_sz[3]);
        serial_write(" top1_at=");
        serial_write_hex_u64((uint64_t)top_at[0]);
        serial_write("\n");
    }

    serial_write("HEAPDUMP blocks=");
    serial_write_hex_u64((uint64_t)blocks);
    serial_write(" reachable_free=");
    serial_write_hex_u64((uint64_t)free_bytes);
    serial_write(" reachable_used=");
    serial_write_hex_u64((uint64_t)used_bytes);
    serial_write(" g_used=");
    serial_write_hex_u64((uint64_t)g_used);
    serial_write(truncated ? " TRUNCATED\n" : " ok\n");

    spin_unlock_irqrestore(&g_heap_lock, flags);
}

void kmalloc_set_physical_total(size_t total_bytes) {
    g_physical_total = total_bytes;
}

size_t kmalloc_get_physical_total(void) {
    return g_physical_total != 0 ? g_physical_total : g_heap_size;
}

size_t kmalloc_get_physical_used(void) {
    /* Approximate physical RAM in use as everything up to the end of the heap's
     * currently-allocated region: the kernel image + heap base sit below the
     * heap, and g_used tracks live heap payload. */
    if (g_physical_total == 0) {
        return g_used;
    }
    if (g_heap_base > g_physical_total) {
        return g_physical_total;
    }
    if (g_used > g_physical_total - g_heap_base) {
        return g_physical_total;
    }
    return (size_t)g_heap_base + g_used;
}

/* ===================================================================
 *  Auxiliary physically-disjoint heaps.
 *
 *  Two kinds of large, long-lived buffers must never share physical memory
 *  with anything else, because they are written under arbitrary page tables
 *  or hold a whole process's address space:
 *    - the GFX heap backs window/compositor surfaces (the compositor writes
 *      them under any process's CR3);
 *    - the IMAGE heap backs process images (a stray overlap would corrupt a
 *      running program).
 *  Each lives in its own region carved off RAM at boot, disjoint from the
 *  main heap (which serves ELF read buffers, sbrk growth, misc) and from each
 *  other. Disjoint regions make overlap impossible by construction, so no
 *  retry/quarantine hack is needed. Same simple first-fit as the main heap,
 *  parameterized over a heap instance. */
struct kheap {
    alloc_header_t *start;
    uintptr_t base;
    size_t size;
};
static struct kheap g_gfx_heap;
static struct kheap g_img_heap;

static int kheap_block_ok(const struct kheap *h, const alloc_header_t *block) {
    uintptr_t p = (uintptr_t)block;
    uintptr_t end = h->base + h->size;
    if (p < h->base || p + sizeof(alloc_header_t) > end) return 0;
    if (block->size > end - p - sizeof(alloc_header_t)) return 0;
    return 1;
}

static void kheap_init(struct kheap *h, uintptr_t base, size_t size) {
    h->base = base;
    h->size = size;
    h->start = (alloc_header_t *)base;
    h->start->size = size - sizeof(alloc_header_t);
    h->start->next = NULL;
    h->start->is_free = 1;
}

static void *kheap_alloc(struct kheap *h, size_t size) {
    alloc_header_t *curr = h->start;
    if (curr == NULL) return NULL;
    size = (size + 7) & ~7u;
    while (curr != NULL) {
        if (!kheap_block_ok(h, curr)) return NULL;
        if (curr->is_free && curr->size >= size) break;
        curr = curr->next;
    }
    if (curr == NULL) return NULL;
    if (curr->size > (size + sizeof(alloc_header_t) + 8)) {
        alloc_header_t *nb = (alloc_header_t *)((uint8_t *)curr + sizeof(alloc_header_t) + size);
        nb->size = curr->size - size - sizeof(alloc_header_t);
        nb->next = curr->next;
        nb->is_free = 1;
        curr->size = size;
        curr->next = nb;
    }
    curr->is_free = 0;
    return (void *)((uint8_t *)curr + sizeof(alloc_header_t));
}

static void kheap_free(struct kheap *h, void *ptr) {
    alloc_header_t *block, *curr;
    if (ptr == NULL) return;
    block = (alloc_header_t *)((uint8_t *)ptr - sizeof(alloc_header_t));
    if (!kheap_block_ok(h, block) || block->is_free) return;
    block->is_free = 1;
    curr = h->start;
    while (curr != NULL && curr->next != NULL) {
        if (!kheap_block_ok(h, curr) || !kheap_block_ok(h, curr->next)) return;
        if (curr->is_free && curr->next->is_free && block_phys_next(curr) == curr->next) {
            curr->size += sizeof(alloc_header_t) + curr->next->size;
            curr->next = curr->next->next;
        } else {
            curr = curr->next;
        }
    }
}

void gfx_heap_init(uintptr_t base, size_t size) { kheap_init(&g_gfx_heap, base, size); }
void *gfx_alloc(size_t size) { return kheap_alloc(&g_gfx_heap, size); }
void gfx_free(void *ptr) { kheap_free(&g_gfx_heap, ptr); }

void image_heap_init(uintptr_t base, size_t size) { kheap_init(&g_img_heap, base, size); }
void *image_alloc(size_t size) { return kheap_alloc(&g_img_heap, size); }
void image_free(void *ptr) { kheap_free(&g_img_heap, ptr); }
int image_heap_ready(void) { return g_img_heap.start != NULL; }
