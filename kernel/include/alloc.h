#ifndef VIBEOS_ALLOC_H
#define VIBEOS_ALLOC_H

#include "types.h"
#include <stddef.h>

/* 
 * Initialize the kernel memory allocator
 * @param heap_start: Start address of heap
 * @param heap_size: Size of heap in bytes
 */
void kmalloc_init(uintptr_t heap_start, size_t heap_size);

/* 
 * Allocate memory from kernel heap
 * @param size: Number of bytes to allocate
 * @return Pointer to allocated memory, NULL on failure
 */
void *kmalloc(size_t size);

/* 
 * Free previously allocated memory
 * @param ptr: Pointer to memory block to free
 */
void kfree(void *ptr);

/* 
 * Get total free memory in heap
 * @return Number of free bytes
 */
size_t kmalloc_get_free(void);
size_t kmalloc_get_total(void);
size_t kmalloc_get_used(void);
/* Walk the whole main-heap free list and dump diagnostics to serial: total
 * blocks, reachable free/used bytes, and the first block whose header fails the
 * sanity check (which truncates the free-list walk and strands all memory after
 * it). Used to tell genuine exhaustion apart from free-list corruption. */
void kmalloc_debug_dump(void);
void kmalloc_set_physical_total(size_t total_bytes);
size_t kmalloc_get_physical_total(void);
size_t kmalloc_get_physical_used(void);

/*
 * Compositor (graphics) heap: a separate, physically-disjoint region used for
 * window backing stores so they can never share physical memory with a process
 * image (which the compositor would otherwise corrupt). Initialize once with a
 * region carved out of RAM; allocate/free window buffers with gfx_alloc/gfx_free.
 */
void gfx_heap_init(uintptr_t base, size_t size);
void *gfx_alloc(size_t size);
void gfx_free(void *ptr);

/*
 * Process-image heap: another separate, physically-disjoint region used for
 * process images, so ELF read buffers / sbrk growth (main heap) can never be
 * placed over a running image. image_heap_ready() reports whether it was set
 * up (callers fall back to the main heap if not).
 */
void image_heap_init(uintptr_t base, size_t size);
void *image_alloc(size_t size);
void image_free(void *ptr);
int image_heap_ready(void);

#endif
