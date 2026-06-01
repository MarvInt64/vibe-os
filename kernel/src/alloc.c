#include "alloc.h"
#include "string.h"
#include "paging.h"
#include "tty.h"
#include <stddef.h>

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

/*
 * Initialize kernel heap memory allocator
 * @param heap_start: Virtual address where heap starts
 * @param heap_size: Total size of heap in bytes
 */
void kmalloc_init(uintptr_t heap_start, size_t heap_size) {
    g_heap_base = heap_start;
    g_heap_size = heap_size;
    
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
void *kmalloc(size_t size) {
    alloc_header_t *curr;
    alloc_header_t *prev = NULL;
    
    /* Align size to 8 bytes for better performance */
    size = (size + 7) & ~7;
    
    curr = g_heap_start;
    
    /* Find first sufficiently large free block (first-fit) */
    while (curr != NULL) {
        if (curr->is_free && curr->size >= size) {
            break;
        }
        prev = curr;
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
    
    /* Return pointer after header */
    return (void *)((uint8_t *)curr + sizeof(alloc_header_t));
}

/*
 * Mark block as free and coalesce with adjacent free blocks
 * @param ptr: Pointer to memory to free
 */
void kfree(void *ptr) {
    alloc_header_t *block;
    alloc_header_t *curr;
    
    if (ptr == NULL) {
        return;
    }
    
    /* Get block header from user pointer */
    block = (alloc_header_t *)((uint8_t *)ptr - sizeof(alloc_header_t));
    block->is_free = 1;
    
    /* Coalesce: merge adjacent free blocks */
    curr = g_heap_start;
    while (curr != NULL && curr->next != NULL) {
        if (curr->is_free && curr->next->is_free) {
            /* Merge current and next block */
            curr->size += sizeof(alloc_header_t) + curr->next->size;
            curr->next = curr->next->next;
        } else {
            curr = curr->next;
        }
    }
}

/*
 * Get total free memory in heap
 * @return Number of free bytes
 */
size_t kmalloc_get_free(void) {
    alloc_header_t *curr;
    size_t free_bytes = 0;
    
    curr = g_heap_start;
    while (curr != NULL) {
        if (curr->is_free) {
            free_bytes += curr->size + sizeof(alloc_header_t);
        }
        curr = curr->next;
    }
    
    return free_bytes;
}
