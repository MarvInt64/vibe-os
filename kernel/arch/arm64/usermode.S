/* VibeOS arm64 — EL1↔EL0 transition.
 *
 * arm64_enter_user(entry, user_sp):
 *   Saves the kernel's callee-saved registers + SP + LR into g_kernel_ctx,
 *   then drops to EL0 at `entry` with stack `user_sp` via eret.
 *
 * arm64_return_to_kernel(code):
 *   Restores the saved kernel context and returns from arm64_enter_user as
 *   if it had returned `code`.  Called by the SVC exit handler — this is a
 *   longjmp back into the kernel after a user process exits.
 *
 * g_kernel_ctx layout (13 × 8 bytes = 104, rounded to 112):
 *   +0   x19  +8  x20  +16 x21  +24 x22  +32 x23  +40 x24
 *   +48  x25  +56 x26  +64 x27  +72 x28  +80 x29(fp)  +88 x30(lr)
 *   +96  sp
 */
.section ".bss", "aw"
.align 4
.global g_kernel_ctx
g_kernel_ctx:
    .space 112

.section ".text", "ax"

/* void arm64_enter_user(uint64_t entry [x0], uint64_t user_sp [x1]) */
.global arm64_enter_user
arm64_enter_user:
    /* Save kernel callee-saved state so the SVC exit path can longjmp back */
    adrp x9, g_kernel_ctx
    add  x9, x9, :lo12:g_kernel_ctx
    stp  x19, x20, [x9, #0]
    stp  x21, x22, [x9, #16]
    stp  x23, x24, [x9, #32]
    stp  x25, x26, [x9, #48]
    stp  x27, x28, [x9, #64]
    stp  x29, x30, [x9, #80]
    mov  x10, sp
    str  x10, [x9, #96]

    /* Configure the EL0 return:
     *   ELR_EL1  = entry point (x0)
     *   SP_EL0   = user stack (x1)
     *   SPSR_EL1 = 0 → EL0t, DAIF clear (interrupts enabled at EL0)
     */
    msr  elr_el1, x0
    msr  sp_el0, x1
    msr  spsr_el1, xzr

    /* Zero the GP registers handed to userspace (don't leak kernel data) */
    mov  x0,  xzr
    mov  x1,  xzr
    mov  x2,  xzr
    mov  x3,  xzr
    mov  x4,  xzr
    mov  x5,  xzr
    mov  x6,  xzr
    mov  x7,  xzr
    mov  x8,  xzr

    isb
    eret                         /* → EL0 at entry */

/* void arm64_return_to_kernel(uint64_t code [x0]) — never returns to caller */
.global arm64_return_to_kernel
arm64_return_to_kernel:
    adrp x9, g_kernel_ctx
    add  x9, x9, :lo12:g_kernel_ctx
    ldp  x19, x20, [x9, #0]
    ldp  x21, x22, [x9, #16]
    ldp  x23, x24, [x9, #32]
    ldp  x25, x26, [x9, #48]
    ldp  x27, x28, [x9, #64]
    ldp  x29, x30, [x9, #80]
    ldr  x10, [x9, #96]
    mov  sp, x10
    /* x0 already holds the exit code → becomes arm64_enter_user's return value */
    ret
