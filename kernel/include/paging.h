#ifndef VIBEOS_PAGING_H
#define VIBEOS_PAGING_H

#include "types.h"

void paging_remap_framebuffer(uintptr_t framebuffer_addr, size_t framebuffer_size);
void paging_map_user_process(uintptr_t user_virtual_base, uintptr_t user_stack_top, uintptr_t code_physical, size_t code_size, uintptr_t stack_physical, size_t stack_size, uint64_t *page_tables, size_t page_table_count);

/* Install the given process page table at the user region described by
 * user_virtual_base and flush the TLB. Because all processes share one
 * address space but live at the same virtual base, this must be called
 * before running a process or touching its user memory from the kernel. */
void paging_activate_user_table(uintptr_t user_virtual_base, uint64_t *page_tables, size_t page_table_count);

#endif
