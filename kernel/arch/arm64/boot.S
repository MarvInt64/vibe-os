/* VibeOS arm64 boot stub.
 *
 * QEMU -machine virt starts in EL2. We drop to EL1, install exception
 * vectors, set up an identity-mapped MMU, clear BSS, and call
 * kernel_main_arm64() in C.
 *
 * Register conventions during boot (before C runtime):
 *   x0  scratch / argument
 *   x1  scratch
 *   sp  boot_stack_top (set early)
 */
.section ".text.boot", "ax"
.global _start

_start:
    /* ---- Determine current EL ---------------------------------------- */
    mrs  x0, CurrentEL
    lsr  x0, x0, #2
    cmp  x0, #2
    b.eq .el2_setup
    /* Already in EL1 — go straight to setup */
    b    .el1_entry

.el2_setup:
    /* Running in EL2.  Configure for AArch64 EL1. */

    /* HCR_EL2: RW=1 → EL1 is AArch64, VM=0 (no stage-2 MMU) */
    mov  x0, #(1 << 31)
    msr  hcr_el2, x0

    /* Disable EL2 coprocessor traps so EL1 can use FP/SIMD freely */
    msr  cptr_el2, xzr

    /* SCTLR_EL2: leave default (MMU off at EL2) */

    /* SPSR_EL2: EL1h (0b0101), all interrupts masked (DAIF=1111) */
    mov  x0, #0x3c5
    msr  spsr_el2, x0

    /* ELR_EL2: return address after eret → .el1_entry */
    adr  x0, .el1_entry
    msr  elr_el2, x0

    /* Drop to EL1 */
    eret
    dsb  sy
    isb

.el1_entry:
    /* ---- Use SP_EL1 -------------------------------------------------- */
    msr  spsel, #1
    isb

    /* ---- Install exception vectors before anything can fault ---------- */
    adr  x0, exception_vectors
    msr  vbar_el1, x0
    isb

    /* ---- Set up boot stack (one CPU only during boot) ---------------- */
    adr  x0, boot_stack_top
    mov  sp, x0

    /* ---- Allow FP/SIMD at EL1 (no CPACR_EL1.FPEN trap) -------------- */
    mrs  x0, cpacr_el1
    orr  x0, x0, #(3 << 20)   /* FPEN=11 */
    msr  cpacr_el1, x0
    isb

    /* ---- MMU identity mapping (skip for now to simplify debugging) --- */
    /* MMU is left OFF during initial port. The QEMU virt machine provides
     * a flat address space so physical == virtual. We enable MMU later
     * once the identity mapping is verified. */
    /* bl   arm64_mmu_init */

    /* ---- Clear BSS ---------------------------------------------------- */
    adr  x0, __bss_start
    adr  x1, __bss_end
    cmp  x0, x1
    b.ge .bss_done
.bss_loop:
    str  xzr, [x0], #8
    cmp  x0, x1
    b.lt .bss_loop
.bss_done:

    /* ---- Enter C -------------------------------------------------------- */
    bl   kernel_main_arm64

    /* Should not return. */
.halt:
    wfi
    b    .halt

/* ---- Boot stack (16 KB, 16-byte aligned per AArch64 ABI) -------------- */
.section ".bss.boot", "aw"
.align 4                       /* 2^4 = 16 bytes */
boot_stack:
    .space 16384
boot_stack_top:
