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

#endif
