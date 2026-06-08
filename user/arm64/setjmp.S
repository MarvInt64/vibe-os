/* setjmp / longjmp for AArch64 (VibeOS freestanding libc).
 *
 * On AArch64, the integer jmp_buf saves:
 *   [0..7]   x19–x26  (callee-saved)
 *   [8]      x27      (callee-saved)
 *   [9]      x28      (callee-saved? actually platform reg, but save it)
 *   [10]     x29 (fp) (callee-saved frame pointer)
 *   [11]     x30 (lr) (return address)
 *   [12]     sp       (stack pointer)
 *   [13..20] d8–d15   (callee-saved SIMD/FP registers, 64 bits each)
 *
 * Total: 22 × 8 = 176 bytes.
 *
 * longjmp(buf, val): if val == 0, return 1.
 */

    .section .text
    .global setjmp
    .global _setjmp
    .type setjmp, %function
setjmp:
_setjmp:
    /* x0 = jmp_buf pointer */
    stp  x19, x20, [x0, #0]
    stp  x21, x22, [x0, #16]
    stp  x23, x24, [x0, #32]
    stp  x25, x26, [x0, #48]
    stp  x27, x28, [x0, #64]
    stp  x29, x30, [x0, #80]
    mov  x1, sp
    str  x1,  [x0, #96]
    /* Save d8-d15 (64-bit each) */
    stp  d8,  d9,  [x0, #104]
    stp  d10, d11, [x0, #120]
    stp  d12, d13, [x0, #136]
    stp  d14, d15, [x0, #152]
    /* Return 0 */
    mov  x0, #0
    ret

    .global longjmp
    .global _longjmp
    .type longjmp, %function
longjmp:
_longjmp:
    /* x0 = jmp_buf, x1 = val */
    ldp  x19, x20, [x0, #0]
    ldp  x21, x22, [x0, #16]
    ldp  x23, x24, [x0, #32]
    ldp  x25, x26, [x0, #48]
    ldp  x27, x28, [x0, #64]
    ldp  x29, x30, [x0, #80]
    ldr  x2,  [x0, #96]
    mov  sp, x2
    /* Restore d8-d15 */
    ldp  d8,  d9,  [x0, #104]
    ldp  d10, d11, [x0, #120]
    ldp  d12, d13, [x0, #136]
    ldp  d14, d15, [x0, #152]
    /* Return val (or 1 if val == 0) */
    cmp  x1, #0
    csinc x0, x1, xzr, ne
    /* Jump to saved x30 via ret (x30 was restored above) */
    ret
