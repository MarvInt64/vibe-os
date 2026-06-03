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

#include "paging.h"

extern uint64_t pml4_table[512];
extern uint64_t pdp_table[512];
extern uint64_t pd_table0[512];
extern uint64_t pd_table1[512];
extern uint64_t pd_table2[512];
extern uint64_t pd_table3[512];
extern uint64_t framebuffer_pt0[512];
extern uint64_t framebuffer_pt1[512];
extern uint64_t framebuffer_pt2[512];
extern uint64_t framebuffer_pt3[512];

#define PAGE_TABLE_BYTES 0x200000u
#define FRAMEBUFFER_PAGE_TABLE_COUNT 4u

/* This function returns a pointer to the page directory table corresponding to the given PDP index. 
The PDP index is derived from the virtual address being mapped. The function uses a switch statement to return the appropriate page directory table based on the index.
If the index is out of range (greater than 3), it returns a null pointer. 
*/
static uint64_t *pd_table_for_index(uint64_t pdp_index) {
    switch (pdp_index) {
        case 0: return pd_table0;
        case 1: return pd_table1;
        case 2: return pd_table2;
        case 3: return pd_table3;
        default: return 0;
    }
}

/* This function fills a 4KB page table with entries mapping consecutive physical addresses starting from the given base address. 
Each entry is marked as present, writable, and user-accessible. */
static void fill_4k_page_table(uint64_t *page_table, uintptr_t base_addr, uint64_t flags) {
    size_t i;

    for (i = 0; i < 512; ++i) {
        page_table[i] = (uint64_t)(base_addr + (i * 0x1000u)) | flags;
    }
}

/* This function installs a 4KB page table for the given region base address. 
It calculates the PDP and PD indices from the region base address and updates the corresponding page directory entry. */
static void install_page_table(uintptr_t region_base, uint64_t *page_table) {
    uint64_t pdp_index = (region_base >> 30) & 0x1ffu;
    uint64_t pd_index = (region_base >> 21) & 0x1ffu;
    uint64_t *pd = pd_table_for_index(pdp_index);

    if (pd == 0) {
        return;
    }

    pd[pd_index] = ((uint64_t)(uintptr_t)page_table) | 0x03u;
}

static void flush_tlb(void) {
    uintptr_t cr3;

    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

/* This function remaps the framebuffer to a new set of page tables. 
It calculates the base and end addresses of the framebuffer region and iterates through the page tables, filling and installing them as needed. */
void paging_remap_framebuffer(uintptr_t framebuffer_addr, size_t framebuffer_size) {
    uintptr_t region_base = framebuffer_addr & ~((uintptr_t)0x1fffffu);
    uintptr_t region_end = framebuffer_addr + framebuffer_size;
    uintptr_t next_region = region_base;
    uint64_t *page_tables[FRAMEBUFFER_PAGE_TABLE_COUNT] = {
        framebuffer_pt0,
        framebuffer_pt1,
        framebuffer_pt2,
        framebuffer_pt3
    };
    size_t i;

    for (i = 0; i < FRAMEBUFFER_PAGE_TABLE_COUNT && next_region < region_end; ++i) {
        fill_4k_page_table(page_tables[i], next_region, 0x9bu);
        install_page_table(next_region, page_tables[i]);
        next_region += PAGE_TABLE_BYTES;
    }

    flush_tlb();
}

void paging_map_user_process(uintptr_t user_virtual_base, uintptr_t user_stack_top, uintptr_t code_physical, size_t code_size, uintptr_t stack_physical, size_t stack_size, uint64_t *page_tables, size_t page_table_count) {
    size_t i;
    size_t code_pages = (code_size + 0xfffu) / 0x1000u;
    size_t stack_pages = (stack_size + 0xfffu) / 0x1000u;
    size_t stack_base_page = ((user_stack_top - stack_size) - user_virtual_base) / 0x1000u;

    for (i = 0; i < page_table_count * 512u; ++i) {
        page_tables[i] = 0;
    }

    for (i = 0; i < code_pages; ++i) {
        size_t page = i;
        size_t table = page / 512u;
        size_t entry = page % 512u;
        if (table >= page_table_count) break;
        page_tables[(table * 512u) + entry] = (uint64_t)(code_physical + (i * 0x1000u)) | 0x07u;
    }

    for (i = 0; i < stack_pages; ++i) {
        size_t page = stack_base_page + i;
        size_t table = page / 512u;
        size_t entry = page % 512u;
        if (table >= page_table_count) break;
        page_tables[(table * 512u) + entry] = (uint64_t)(stack_physical + (i * 0x1000u)) | 0x07u;
    }

    /* Mark the user region's upper-level tables as user-accessible. The page
     * table is only made live in pd_table0 when the process is activated, so
     * multiple processes can share one virtual base without clobbering each
     * other at load time. */
    pml4_table[0] |= 0x04u;
    pdp_table[0] |= 0x04u;
}

uintptr_t paging_build_process_tables(uint64_t *pml4, uint64_t *pdp, uint64_t *pd0,
                                      uintptr_t user_virtual_base,
                                      uint64_t *user_page_tables, size_t page_table_count) {
    uint64_t pd_index = (user_virtual_base >> 21) & 0x1ffu;   /* 0x20000000 -> 256 */
    size_t i;

    if (pml4 == 0 || pdp == 0 || pd0 == 0 || user_page_tables == 0) {
        return 0;
    }

    /* Private low-1GB PD: start as a copy of the boot identity directory (2MB
     * huge pages, kernel-only) so the kernel image/heap below the user region
     * stay mapped, then splice the user region's own 4KB tables over it. */
    for (i = 0; i < 512u; ++i) {
        pd0[i] = pd_table0[i];
    }
    for (i = 0; i < page_table_count; ++i) {
        pd0[pd_index + i] = ((uint64_t)(uintptr_t)&user_page_tables[i * 512u]) | 0x07u;
    }

    /* PDP: private PD for 0..1GB (user bit so ring 3 can reach the user slice —
     * the kernel huge pages keep their own no-user flag, so only the user
     * region is actually reachable from user mode), shared kernel PDs for the
     * upper 3GB (these hold the kernel heap and the remapped framebuffer). */
    pdp[0] = ((uint64_t)(uintptr_t)pd0) | 0x07u;
    pdp[1] = ((uint64_t)(uintptr_t)pd_table1) | 0x03u;
    pdp[2] = ((uint64_t)(uintptr_t)pd_table2) | 0x03u;
    pdp[3] = ((uint64_t)(uintptr_t)pd_table3) | 0x03u;
    for (i = 4; i < 512u; ++i) {
        pdp[i] = 0;
    }

    /* PML4: a single entry covering the low 512GB (everything we use). */
    pml4[0] = ((uint64_t)(uintptr_t)pdp) | 0x07u;
    for (i = 1; i < 512u; ++i) {
        pml4[i] = 0;
    }

    /* Tables are identity-mapped, so the virtual address == physical == CR3. */
    return (uintptr_t)pml4;
}

void paging_load_cr3(uintptr_t cr3) {
    if (cr3 == 0) {
        return;
    }
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

uintptr_t paging_read_cr3(void) {
    uintptr_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}
