/* VibeOS arm64 MMU — minimal identity mapping for boot.
 *
 * We use 4KB granule with 2-level page tables (L1 block descriptors = 1 GB
 * blocks) covering the first 4 GB of physical address space:
 *
 *   0x00000000 – 0x3FFFFFFF   device / MMIO  (nGnRE, non-cacheable)
 *   0x40000000 – 0x7FFFFFFF   RAM            (Normal WB inner-shareable)
 *
 * TCR_EL1.T0SZ = 32 → 32-bit user VA, L1 table has 4 entries.
 * TTBR0_EL1 = physical address of the L1 table (identity-mapped boot use).
 * TTBR1_EL1 = same table (kernel VA = PA during early boot).
 *
 * MAIR_EL1 index 0 = Device nGnRE (0x00)
 * MAIR_EL1 index 1 = Normal WB inner+outer shareable (0xFF)
 */
#include "arch.h"

/* 4 × 8 bytes, 4 KB aligned (only 32 bytes used but alignment matters) */
static uint64_t boot_l1[4] __attribute__((aligned(4096)));

/* Build a 1 GB block descriptor for L1.
 * attr_idx 0 = device, 1 = normal WB.  af = access flag (must be 1). */
static uint64_t mk_block(uint64_t phys, unsigned attr_idx) {
    uint64_t entry = (phys & 0x0000FFFFC0000000ULL)  /* OA[47:30] */
                   | (1ULL  << 10)                    /* AF */
                   | (3ULL  << 8)                     /* SH = inner shareable */
                   | ((uint64_t)attr_idx << 2)         /* AttrIndx */
                   | 0x1ULL;                           /* block descriptor */
    if (attr_idx != 0)
        entry |= (1ULL << 54);  /* UXN for normal memory (not executable from EL0) */
    return entry;
}

void arm64_mmu_init(void) {
    /* MAIR_EL1: index 0 = Device nGnRE, index 1 = Normal WB */
    write_sysreg(mair_el1, 0x00ffULL);

    /* Fill L1 table:
     *   [0] 0x00000000 → device  (MMIO: GIC, UART, etc.)
     *   [1] 0x40000000 → normal  (RAM: kernel + heap)
     *   [2][3] unmapped (leave 0) */
    boot_l1[0] = mk_block(0x00000000ULL, 0);  /* device */
    boot_l1[1] = mk_block(0x40000000ULL, 1);  /* RAM    */
    boot_l1[2] = 0;
    boot_l1[3] = 0;

    /* TCR_EL1:
     *   T0SZ = 32  (4 GB VA for TTBR0)
     *   IRGN0 = 01 (inner WB)
     *   ORGN0 = 01 (outer WB)
     *   SH0   = 11 (inner shareable)
     *   TG0   = 00 (4 KB)
     *   T1SZ  = 32 (same for TTBR1)
     *   IPS   = 001 (36-bit PA)
     */
    uint64_t tcr = 32ULL               /* T0SZ */
                 | (1ULL << 8)         /* IRGN0=WB */
                 | (1ULL << 10)        /* ORGN0=WB */
                 | (3ULL << 12)        /* SH0=inner */
                 | (0ULL << 14)        /* TG0=4KB */
                 | (32ULL << 16)       /* T1SZ */
                 | (1ULL << 24)        /* IRGN1=WB */
                 | (1ULL << 26)        /* ORGN1=WB */
                 | (3ULL << 28)        /* SH1=inner */
                 | (2ULL << 30)        /* TG1=4KB (10) */
                 | (1ULL << 32);       /* IPS=36-bit */
    write_sysreg(tcr_el1, tcr);

    /* Point both TTBRs at our L1 table (identity mapping during boot) */
    uint64_t l1_pa = (uint64_t)(uintptr_t)boot_l1;
    write_sysreg(ttbr0_el1, l1_pa);
    write_sysreg(ttbr1_el1, l1_pa);

    /* Flush old TLB entries */
    __asm__ volatile("dsb sy; tlbi vmalle1; dsb sy; isb" ::: "memory");

    /* Enable MMU + I-cache + D-cache in SCTLR_EL1 */
    uint64_t sctlr = read_sysreg(sctlr_el1);
    sctlr |= (1ULL << 0)   /* M  — MMU */
          |  (1ULL << 2)   /* C  — D-cache */
          |  (1ULL << 12); /* I  — I-cache */
    write_sysreg(sctlr_el1, sctlr);
    __asm__ volatile("dsb sy; isb" ::: "memory");
}
