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
#include "../../include/alloc.h"

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
    /* Normal WB RAM for the kernel: EL1-only (AP=00), EL0 execute-never.
     * Keeping this EL1-private is required — making it EL0-writable would make
     * it implicitly non-executable at EL1, breaking kernel code execution. */
    return (pa & 0x0000FFFFC0000000ULL)
         | (1ULL << 54)  /* UXN — no EL0 execute */
         | (3ULL << 8)   /* SH = inner shareable */
         | (1ULL << 10)  /* AF */
         | (1ULL << 2)   /* AttrIndx = 1 */
         | 0x1ULL;       /* block */
}

/* Normal WB RAM accessible AND executable from EL0 (for userspace code/stack).
 *
 * AP=01 (bit[6]=1) → RW at both EL0 and EL1.
 * UXN=0            → executable at EL0.
 * PXN=1            → NOT executable at EL1 (kernel never runs user pages; and
 *                    an EL0-writable page is treated as PXN at EL1 anyway).
 *
 * Coarse 1 GB block — enough to prove the EL0 + SVC round trip. Real per-
 * process protection needs 4 KB page tables; a later milestone. */
static uint64_t block_user(uint64_t pa) {
    return (pa & 0x0000FFFFC0000000ULL)
         | (1ULL << 53)  /* PXN — not executable at EL1 */
         | (1ULL << 6)   /* AP[1]=1 → EL0 access (AP=01: RW both) */
         | (3ULL << 8)   /* SH = inner shareable */
         | (1ULL << 10)  /* AF */
         | (1ULL << 2)   /* AttrIndx = 1 */
         | 0x1ULL;       /* block */
}

/* ---------------------------------------------------------------------------
 * Per-process address spaces.
 *
 * The boot map uses a single L1 with three 1 GB blocks; block 2 (the EL0
 * alias of RAM) is shared by every process, so all apps see the same memory
 * at VA 0x90000000 — only one can be resident at a time.  To run several
 * processes concurrently each needs its OWN view of the user VA window while
 * still sharing the device MMIO, the EL1 kernel RAM, and the framebuffer
 * alias.
 *
 * We give each process a private L1 + L2 pair:
 *   L1[0] = device   (shared)
 *   L1[1] = kernel RAM, EL1-only (shared — the kernel runs under TTBR0 too,
 *           since TTBR1 is disabled, so this must be present in every L1)
 *   L1[2] = table descriptor → private L2 (covers VA 0x80000000-0xBFFFFFFF)
 *   L1[3] = 0
 *
 * The L2 maps its 1 GB in 512 × 2 MB blocks.  By default every entry is the
 * identity EL0 alias (VA 0x80000000+i*2MB → PA 0x40000000+i*2MB) so the
 * framebuffer and any kmalloc'd buffer handed to userspace stay reachable.
 * The entries covering the app's slot (VA 0x90000000 .. +ARM64_ASPACE_SLOT)
 * are overridden to point at the process's private PA region instead, so each
 * process gets distinct physical memory behind the same virtual addresses.
 * ------------------------------------------------------------------------- */
#define ARM64_USER_VA      0x90000000ULL
#define ARM64_ASPACE_SLOT  (64UL * 1024 * 1024)   /* 64 MB image+stack+heap window */

/* 2 MB Level-2 block descriptor, EL0 RW+X (same attrs as block_user). */
static uint64_t block_user_l2(uint64_t pa) {
    return (pa & 0x0000FFFFFFE00000ULL)  /* OA[47:21], 2 MB aligned */
         | (1ULL << 53)  /* PXN — not executable at EL1 */
         | (1ULL << 6)   /* AP[1]=1 → EL0 RW */
         | (3ULL << 8)   /* SH = inner shareable */
         | (1ULL << 10)  /* AF */
         | (1ULL << 2)   /* AttrIndx = 1 (normal WB) */
         | 0x1ULL;       /* block */
}

/* Allocate a 4 KB page-table page (zeroed, 4 KB aligned) from the kernel heap.
 * kmalloc has no alignment guarantee, so over-allocate and align up; the raw
 * pointer is recorded by the caller for kfree. */
static uint64_t *alloc_table_page(void **raw_out) {
    void *raw = kmalloc(4096 + 4096);
    if (!raw) { *raw_out = 0; return 0; }
    *raw_out = raw;
    uintptr_t aligned = ((uintptr_t)raw + 4095) & ~(uintptr_t)4095;
    uint64_t *t = (uint64_t *)aligned;
    for (int i = 0; i < 512; i++) t[i] = 0;
    return t;
}

/* Build a private address space whose user slot is backed by `user_pa`
 * (a 2 MB-aligned PA region of at least ARM64_ASPACE_SLOT bytes, inside the
 * kernel RAM identity range 0x40000000-0x7FFFFFFF).
 *
 * Returns the TTBR0 value (PA of the new L1) or 0 on failure.  The two raw
 * kmalloc pointers backing the L1 and L2 pages are returned via *l1_raw /
 * *l2_raw so the caller can free them when the process exits. */
uint64_t arm64_aspace_create(uint64_t user_pa, void **l1_raw, void **l2_raw) {
    uint64_t *l1 = alloc_table_page(l1_raw);
    uint64_t *l2 = alloc_table_page(l2_raw);
    if (!l1 || !l2) {
        if (*l1_raw) kfree(*l1_raw);
        if (*l2_raw) kfree(*l2_raw);
        *l1_raw = *l2_raw = 0;
        return 0;
    }

    l1[0] = block_device(0x00000000ULL);   /* MMIO            (shared) */
    l1[1] = block_normal(0x40000000ULL);   /* kernel RAM, EL1 (shared) */
    /* Table descriptor → L2 (bits[1:0]=11, next-level table PA in [47:12]). */
    l1[2] = ((uint64_t)(uintptr_t)l2 & 0x0000FFFFFFFFF000ULL) | 0x3ULL;
    l1[3] = 0;

    /* Default: identity EL0 alias of RAM across the whole 1 GB. */
    for (int i = 0; i < 512; i++)
        l2[i] = block_user_l2(0x40000000ULL + (uint64_t)i * 0x200000ULL);

    /* Override the app slot to point at the process's private PA. */
    int base = (int)((ARM64_USER_VA - 0x80000000ULL) / 0x200000ULL);  /* 128 */
    int n    = (int)(ARM64_ASPACE_SLOT / 0x200000ULL);                /* 8   */
    for (int i = 0; i < n; i++)
        l2[base + i] = block_user_l2(user_pa + (uint64_t)i * 0x200000ULL);

    __asm__ volatile("dsb ish" ::: "memory");
    return (uint64_t)(uintptr_t)l1;
}

/* Switch the active address space and flush the TLB. */
void arm64_aspace_switch(uint64_t ttbr0) {
    write_sysreg(ttbr0_el1, ttbr0);
    __asm__ volatile("dsb ish; tlbi vmalle1is; dsb ish; isb" ::: "memory");
}

/* Return to the shared boot address space (used when no process is running). */
void arm64_aspace_switch_boot(void) {
    arm64_aspace_switch((uint64_t)(uintptr_t)boot_l1);
}

void arm64_mmu_init(void) {
    /* MAIR_EL1: index 0 = Device nGnRE (0x00), index 1 = Normal WB (0xFF) */
    write_sysreg(mair_el1, 0xFF00ULL);

    /* Fill L1 table (4 entries for 4 GB with T0SZ=32).
     *
     * Block 2 (VA 0x80000000) is an EL0-accessible ALIAS of the same physical
     * kernel RAM at PA 0x40000000. This lets us hand userspace an EL0 view of
     * a kmalloc'd buffer without a second physical region (QEMU -m 512M only
     * backs PA 0x40000000..0x5FFFFFFF). A kernel buffer at PA p is reachable
     * from EL0 at virtual address (p - 0x40000000) + 0x80000000 = p + 0x40000000. */
    boot_l1[0] = block_device(0x00000000ULL);   /* 0 – 1 GB: MMIO         */
    boot_l1[1] = block_normal(0x40000000ULL);   /* 1 – 2 GB: kernel RAM   */
    boot_l1[2] = block_user  (0x40000000ULL);   /* 2 – 3 GB: EL0 alias of RAM */
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
