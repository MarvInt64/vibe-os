/* VibeOS arm64 exception vector table + handlers.
 *
 * AArch64 exception vectors (VBAR_EL1-based) must be 2048-byte aligned.
 * There are 16 vectors, each at a 128-byte (0x80) slot:
 *
 *  Offset  Level           Type
 *  0x000   cur-EL SP_EL0  Synchronous
 *  0x080   cur-EL SP_EL0  IRQ
 *  0x100   cur-EL SP_EL0  FIQ
 *  0x180   cur-EL SP_EL0  SError
 *  0x200   cur-EL SP_EL1  Synchronous
 *  0x280   cur-EL SP_EL1  IRQ
 *  0x300   cur-EL SP_EL1  FIQ
 *  0x380   cur-EL SP_EL1  SError
 *  0x400   lower EL AArch64 Synchronous  (syscall, user page fault)
 *  0x480   lower EL AArch64 IRQ
 *  0x500   lower EL AArch64 FIQ
 *  0x580   lower EL AArch64 SError
 *  0x600   lower EL AArch32 Synchronous  (unused)
 *  ...
 */

/* Save all GP registers + EL0 SP + ELR + SPSR to the current stack. */
.macro SAVE_REGS
    sub  sp, sp, #272          /* 34 * 8 = 272 bytes: x0-x30, sp_el0, elr, spsr */
    stp  x0,  x1,  [sp, #0]
    stp  x2,  x3,  [sp, #16]
    stp  x4,  x5,  [sp, #32]
    stp  x6,  x7,  [sp, #48]
    stp  x8,  x9,  [sp, #64]
    stp  x10, x11, [sp, #80]
    stp  x12, x13, [sp, #96]
    stp  x14, x15, [sp, #112]
    stp  x16, x17, [sp, #128]
    stp  x18, x19, [sp, #144]
    stp  x20, x21, [sp, #160]
    stp  x22, x23, [sp, #176]
    stp  x24, x25, [sp, #192]
    stp  x26, x27, [sp, #208]
    stp  x28, x29, [sp, #224]
    str  x30,      [sp, #240]
    mrs  x0, sp_el0
    str  x0,       [sp, #248]
    mrs  x0, elr_el1
    str  x0,       [sp, #256]
    mrs  x0, spsr_el1
    str  x0,       [sp, #264]
.endm

.macro RESTORE_REGS
    ldr  x0,       [sp, #264]
    msr  spsr_el1, x0
    ldr  x0,       [sp, #256]
    msr  elr_el1,  x0
    ldr  x0,       [sp, #248]
    msr  sp_el0,   x0
    ldp  x0,  x1,  [sp, #0]
    ldp  x2,  x3,  [sp, #16]
    ldp  x4,  x5,  [sp, #32]
    ldp  x6,  x7,  [sp, #48]
    ldp  x8,  x9,  [sp, #64]
    ldp  x10, x11, [sp, #80]
    ldp  x12, x13, [sp, #96]
    ldp  x14, x15, [sp, #112]
    ldp  x16, x17, [sp, #128]
    ldp  x18, x19, [sp, #144]
    ldp  x20, x21, [sp, #160]
    ldp  x22, x23, [sp, #176]
    ldp  x24, x25, [sp, #192]
    ldp  x26, x27, [sp, #208]
    ldp  x28, x29, [sp, #224]
    ldr  x30,      [sp, #240]
    add  sp, sp, #272
.endm

/* ---- Vector table (must be 2048-byte aligned = .align 11) ------------ */
.section ".text.vectors", "ax"
.align 11
.global exception_vectors
exception_vectors:

    /* Current EL with SP_EL0 (unusual — use for debug only) */
    .align 7; b exc_el1_sp0_sync
    .align 7; b exc_el1_sp0_irq
    .align 7; b exc_el1_sp0_fiq
    .align 7; b exc_el1_sp0_serror

    /* Current EL with SP_EL1 (kernel exceptions) */
    .align 7; b exc_el1_sync
    .align 7; b exc_el1_irq
    .align 7; b exc_el1_fiq
    .align 7; b exc_el1_serror

    /* Lower EL AArch64 (user exceptions + syscalls) */
    .align 7; b exc_el0_sync
    .align 7; b exc_el0_irq
    .align 7; b exc_el0_fiq
    .align 7; b exc_el0_serror

    /* Lower EL AArch32 (unused on VibeOS) */
    .align 7; b exc_invalid
    .align 7; b exc_invalid
    .align 7; b exc_invalid
    .align 7; b exc_invalid

/* ---- Handlers --------------------------------------------------------- */

/* Kernel IRQ — the main one we use (timer, etc.) */
exc_el1_irq:
    SAVE_REGS
    bl   bkl_acquire
    bl   arm64_irq_handler
    bl   bkl_release
    RESTORE_REGS
    eret

/* User IRQ */
exc_el0_irq:
    SAVE_REGS
    bl   bkl_acquire
    bl   arm64_irq_handler
    bl   bkl_release
    RESTORE_REGS
    eret

/* User synchronous (syscalls / page faults from EL0) */
exc_el0_sync:
    SAVE_REGS
    bl   bkl_acquire
    mrs  x0, esr_el1
    mrs  x1, elr_el1
    mrs  x2, far_el1
    mov  x3, sp
    bl   arm64_sync_handler_el0
    bl   bkl_release
    RESTORE_REGS
    eret

/* Kernel synchronous (kernel page faults / bugs) */
exc_el1_sync:
    SAVE_REGS
    bl   bkl_acquire
    mrs  x0, esr_el1
    mrs  x1, elr_el1
    mrs  x2, far_el1
    mov  x3, sp
    bl   arm64_sync_handler_el1
    bl   bkl_release
    RESTORE_REGS
    eret

/* Catch-all for unused/unexpected vectors */
exc_el1_sp0_sync:
exc_el1_sp0_irq:
exc_el1_sp0_fiq:
exc_el1_sp0_serror:
exc_el1_fiq:
exc_el1_serror:
exc_el0_fiq:
exc_el0_serror:
exc_invalid:
    mrs  x0, esr_el1
    mrs  x1, elr_el1
    mrs  x2, far_el1
    bl   arm64_unhandled_exception
.dead:
    wfi
    b    .dead
