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

    /* Args: x0=entry, x1=user_sp, x2=argc, x3=argv.
     * Configure the EL0 return:
     *   ELR_EL1  = entry point (x0)
     *   SP_EL0   = user stack (x1)
     *   SPSR_EL1 = 0 → EL0t, DAIF clear (interrupts enabled at EL0)
     */
    msr  elr_el1, x0
    msr  sp_el0, x1
    msr  spsr_el1, xzr

    /* Hand argc/argv to the entry point in x0/x1 (crt0 expects them). */
    mov  x0, x2                  /* argc */
    mov  x1, x3                  /* argv */
    /* Zero the remaining GP registers (don't leak kernel data) */
    mov  x2,  xzr
    mov  x3,  xzr
    mov  x4,  xzr
    mov  x5,  xzr
    mov  x6,  xzr
    mov  x7,  xzr
    mov  x8,  xzr

    isb
    eret                         /* → EL0 at entry */

/* void arm64_resume_user(void *frame [x0])
 *
 * Resume a previously-suspended EL0 process from a saved 272-byte exception
 * frame (same layout SAVE_REGS in exceptions.S produces: x0-x30, sp_el0, elr,
 * spsr).  Saves the kernel's callee-saved state into g_kernel_ctx first (so a
 * later arm64_return_to_kernel longjmps back here), then restores the full EL0
 * register state and erets.  Mirrors arm64_enter_user but for resume. */
.global arm64_resume_user
arm64_resume_user:
    mov  x9, x0                  /* x9 = frame ptr (not visible to EL0) */
    /* Save kernel callee-saved state. */
    adrp x0, g_kernel_ctx
    add  x0, x0, :lo12:g_kernel_ctx
    stp  x19, x20, [x0, #0]
    stp  x21, x22, [x0, #16]
    stp  x23, x24, [x0, #32]
    stp  x25, x26, [x0, #48]
    stp  x27, x28, [x0, #64]
    stp  x29, x30, [x0, #80]
    mov  x10, sp
    str  x10, [x0, #96]
    /* Restore EL0 system registers from the frame. */
    ldr  x0, [x9, #264]
    msr  spsr_el1, x0
    ldr  x0, [x9, #256]
    msr  elr_el1, x0
    ldr  x0, [x9, #248]
    msr  sp_el0, x0
    /* Restore GP registers (x9 is the base; restore x0,x1,x8,x9 last). */
    ldp  x2,  x3,  [x9, #16]
    ldp  x4,  x5,  [x9, #32]
    ldp  x6,  x7,  [x9, #48]
    ldp  x10, x11, [x9, #80]
    ldp  x12, x13, [x9, #96]
    ldp  x14, x15, [x9, #112]
    ldp  x16, x17, [x9, #128]
    ldp  x18, x19, [x9, #144]
    ldp  x20, x21, [x9, #160]
    ldp  x22, x23, [x9, #176]
    ldp  x24, x25, [x9, #192]
    ldp  x26, x27, [x9, #208]
    ldp  x28, x29, [x9, #224]
    ldr  x30,      [x9, #240]
    ldr  x8,       [x9, #64]
    ldp  x0,  x1,  [x9, #0]
    ldr  x9,       [x9, #72]
    isb
    eret                         /* → EL0, resuming after the svc */

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
