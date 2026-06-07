/* VibeOS arm64 boot stub.
 *
 * EL2 → EL1 drop, exception vectors, BSS clear, MMU identity-map, C entry.
 *
 * IMPORTANT ordering:
 *   1. Stack + vectors (needed for any fault during setup)
 *   2. BSS clear            — boot_l1 page-table lives in BSS
 *   3. arm64_mmu_init()     — builds + enables identity mapping
 *   4. kernel_main_arm64()  — C init with caches + full address space
 *
 * Without MMU, HVF can't determine the access parameters (ISV=0) for
 * device MMIO (GIC at 0x08000000), so GIC must wait until after step 3.
 */
.section ".text.boot", "ax"
.global _start

_start:
    /* ---- Determine current EL ---------------------------------------- */
    mrs  x0, CurrentEL
    lsr  x0, x0, #2
    cmp  x0, #2
    b.eq .el2_setup
    b    .el1_entry

.el2_setup:
    /* HCR_EL2.RW=1 → EL1 is AArch64 */
    mov  x0, #(1 << 31)
    msr  hcr_el2, x0
    /* No EL2 coprocessor traps */
    msr  cptr_el2, xzr
    /* SPSR_EL2: EL1h, DAIF all masked */
    mov  x0, #0x3c5
    msr  spsr_el2, x0
    adr  x0, .el1_entry
    msr  elr_el2, x0
    eret

.el1_entry:
    /* ---- SP_EL1 --------------------------------------------------------- */
    msr  spsel, #1
    isb

    /* ---- Exception vectors (before anything can fault) ----------------- */
    adr  x0, exception_vectors
    msr  vbar_el1, x0
    isb

    /* ---- Boot stack ----------------------------------------------------- */
    adr  x0, boot_stack_top
    mov  sp, x0

    /* ---- FP/SIMD enabled at EL1 ---------------------------------------- */
    mrs  x0, cpacr_el1
    orr  x0, x0, #(3 << 20)
    msr  cpacr_el1, x0
    isb

    /* ---- Clear BSS first (boot_l1 page tables live in BSS) ------------- */
    adr  x0, __bss_start
    adr  x1, __bss_end
    cmp  x0, x1
    b.ge .bss_done
.bss_loop:
    str  xzr, [x0], #8
    cmp  x0, x1
    b.lt .bss_loop
.bss_done:

    /* ---- Enable identity-mapped MMU ------------------------------------- */
    /* Device MMIO (GIC, UART) at 0x00000000-0x3FFFFFFF → nGnRE, no cache  */
    /* RAM + kernel at 0x40000000-0x7FFFFFFF → Normal WB inner-shareable   */
    /* With MMU on, HVF can properly intercept device MMIO with ISV=1.     */
    bl   arm64_mmu_init

    /* ---- Enter C -------------------------------------------------------- */
    bl   kernel_main_arm64

.halt:
    wfi
    b    .halt

.global smp_secondary_entry
smp_secondary_entry:
    /* We are started in EL1 by PSCI CPU_ON. x0 contains context_id (cpu_id) */
    
    /* Save cpu_id in x19 to survive function calls */
    mov  x19, x0

    /* ---- SP_EL1 --------------------------------------------------------- */
    msr  spsel, #1
    isb

    /* ---- Exception vectors ---------------------------------------------- */
    adr  x0, exception_vectors
    msr  vbar_el1, x0
    isb

    /* ---- Boot stack ----------------------------------------------------- */
    /* Read g_ap_stack_top set by primary core */
    ldr  x0, =g_ap_stack_top
    ldr  x0, [x0]
    mov  sp, x0

    /* ---- FP/SIMD enabled at EL1 ---------------------------------------- */
    mrs  x0, cpacr_el1
    orr  x0, x0, #(3 << 20)
    msr  cpacr_el1, x0
    isb

    /* ---- Enable identity-mapped MMU for this core ----------------------- */
    /* Primary core already built boot_l1, so we just enable it */
    bl   arm64_mmu_init

    /* ---- Enter C (smp_ap_main) ------------------------------------------ */
    mov  x0, x19
    bl   smp_ap_main

    b    .halt

/* ---- Boot stack (64 KB, 16-byte aligned) ------------------------------- */
.section ".bss.boot", "aw"
.align 4
boot_stack:
    .space 65536
boot_stack_top:
