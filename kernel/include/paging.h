#ifndef VIBEOS_PAGING_H
#define VIBEOS_PAGING_H

#include "types.h"

void paging_remap_framebuffer(uintptr_t framebuffer_addr, size_t framebuffer_size);
void paging_map_user_process(uintptr_t user_virtual_base, uintptr_t user_stack_top, uintptr_t code_physical, size_t code_size, uintptr_t stack_physical, size_t stack_size, uint64_t *page_tables, size_t page_table_count);

/* Build a process's PRIVATE top-level paging hierarchy. Each process owns a
 * PML4, a PDP and a low-1GB PD (pml4/pdp/pd0, each a 512-entry table). The user
 * region (16 4KB tables already filled by paging_map_user_process) is spliced
 * into a copy of the boot identity PD for 0..1GB; the upper 3GB (kernel heap +
 * framebuffer in pd_table1..3) are shared with the boot tables. Returns the CR3
 * value to load for this address space (== physical, since tables are identity
 * mapped). This replaces the old "swap pd_table0 entries on every switch"
 * model: switching is now a single load_cr3, which is SMP-safe and cannot leave
 * a process running against another's mappings. */
uintptr_t paging_build_process_tables(uint64_t *pml4, uint64_t *pdp, uint64_t *pd0,
                                      uintptr_t user_virtual_base,
                                      uint64_t *user_page_tables, size_t page_table_count);

/* Load CR3 (a value returned by paging_build_process_tables). The CPU flushes
 * non-global TLB entries on CR3 write, so no separate flush is needed. */
void paging_load_cr3(uintptr_t cr3);

/* Current CR3 — used to save/restore around a temporary address-space switch
 * (e.g. the kernel writing into a blocked process's user buffer). */
uintptr_t paging_read_cr3(void);

#endif
