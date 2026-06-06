/* VibeOS arm64 MMU — identity mapping for boot.
 *
 * 4 KB granule, L1 block descriptors (1 GB each).
 * T0SZ=32 → 32-bit VA, 4 L1 entries covering 4 GB.
 *
 * Map:
 *   VA 0x00000000 – 0x3FFFFFFF → device nGnRE (GIC, UART, …)
 *   VA 0x40000000 – 0x7FFFFFFF → normal WB inner-shareable (RAM, kernel)
 *
 * MAIR_EL1 index:
 *   0 = Device nGnRE   (0x00)
 *   1 = Normal WB-WA   (0xFF — outer+inner write-back, write-allocate)
 *
 * Without the MMU, HVF cannot determine the access parameters (ISV=0) for
 * device MMIO accesses, causing an assertion failure in qemu-hvf. With the
 * MMU on and device memory mapped nGnRE, QEMU traps correctly (ISV=1).
 */
#include "arch.h"

/* L1 page table: 4 entries × 8 bytes, must be 4 KB aligned.
 * Lives in BSS — boot.S clears BSS before calling us. */
static uint64_t boot_l1[4] __attribute__((aligned(4096)));

/* Build a 1 GB Level-1 block descriptor.
 *
 * Bit layout:
 *   [1:0]    = 01  (block)
 *   [4:2]    = AttrIndx
 *   [5]      = NS  (0 = secure world)
 *   [7:6]    = AP  (00 = RW from EL1, EL0 no access)
 *   [9:8]    = SH  (00=none, 10=outer, 11=inner)
 *   [10]     = AF  (access flag, must be 1 to avoid AF fault)
 *   [11]     = nG  (0 = global)
 *   [47:30]  = OA  (output address, 1 GB aligned)
 *   [54]     = UXN (EL0 execute-never; only for normal memory)
 *   [55]     = PXN (EL1 execute-never)
 */
static uint64_t block_device(uint64_t pa) {
    /* Device nGnRE: AttrIndx=0, SH=non-shareable(00), AF=1 */
    return (pa & 0x0000FFFFC0000000ULL)
         | (0ULL << 8)   /* SH = non-shareable */
         | (1ULL << 10)  /* AF */
         | (0ULL << 2)   /* AttrIndx = 0 */
         | 0x1ULL;       /* block */
}

static uint64_t block_normal(uint64_t pa) {
    /* Normal WB: AttrIndx=1, SH=inner-shareable(11), AF=1, UXN=1 */
    return (pa & 0x0000FFFFC0000000ULL)
         | (1ULL << 54)  /* UXN */
         | (3ULL << 8)   /* SH = inner shareable */
         | (1ULL << 10)  /* AF */
         | (1ULL << 2)   /* AttrIndx = 1 */
         | 0x1ULL;       /* block */
}

void arm64_mmu_init(void) {
    /* MAIR_EL1: index 0 = Device nGnRE, index 1 = Normal WB */
    write_sysreg(mair_el1, 0x00FFULL);

    /* Fill L1 table (4 entries for 4 GB with T0SZ=32) */
    boot_l1[0] = block_device(0x00000000ULL);   /* 0 – 1 GB: MMIO */
    boot_l1[1] = block_normal(0x40000000ULL);   /* 1 – 2 GB: RAM  */
    boot_l1[2] = 0;
    boot_l1[3] = 0;

    /* TCR_EL1:
     *   T0SZ=32     → 32-bit user VA (4 GB)
     *   IRGN0=01    → inner write-back
     *   ORGN0=01    → outer write-back
     *   SH0=11      → inner shareable
     *   TG0=00      → 4 KB granule
     *   T1SZ=63     → TTBR1 covers only 1 address (effectively disabled)
     *   TG1=10      → 4 KB granule (must be set even if T1 unused)
     *   IPS=001     → 36-bit physical address
     */
    uint64_t tcr = 32ULL               /* T0SZ */
                 | (1ULL << 8)         /* IRGN0=WB */
                 | (1ULL << 10)        /* ORGN0=WB */
                 | (3ULL << 12)        /* SH0=inner */
                 | (0ULL << 14)        /* TG0=4KB */
                 | (63ULL << 16)       /* T1SZ=63 (TTBR1 disabled) */
                 | (2ULL << 30)        /* TG1=4KB */
                 | (1ULL << 32);       /* IPS=36-bit */
    write_sysreg(tcr_el1, tcr);

    /* TTBR0_EL1 = physical address of L1 table (identity-mapped, so VA=PA) */
    write_sysreg(ttbr0_el1, (uint64_t)(uintptr_t)boot_l1);

    /* Flush TLB before enabling */
    __asm__ volatile("dsb sy; isb; tlbi vmalle1; dsb sy; isb" ::: "memory");

    /* Enable MMU (M), D-cache (C), I-cache (I) in SCTLR_EL1.
     * Also clear A (alignment check, bit 1) so unaligned accesses in C code
     * (e.g. packed structs in ext2 headers) don't fault — same as Linux. */
    uint64_t sctlr = read_sysreg(sctlr_el1);
    sctlr |= (1ULL << 0)    /* M — MMU on */
          |  (1ULL << 2)    /* C — D-cache */
          |  (1ULL << 12);  /* I — I-cache */
    /* Clear: A (alignment check), WXN (W→X-never, keeps .text executable) */
    sctlr &= ~(1ULL << 1);   /* A  — no alignment fault on unaligned accesses */
    sctlr &= ~(1ULL << 19);  /* WXN */
    write_sysreg(sctlr_el1, sctlr);
    __asm__ volatile("dsb sy; isb" ::: "memory");
}
