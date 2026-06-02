/* setjmp / longjmp for x86-64 System V ABI (VibeOS freestanding libc).
 *
 * jmp_buf layout (8 x uint64):
 *   [0] rbx   [1] rbp   [2] r12   [3] r13
 *   [4] r14   [5] r15   [6] rsp   [7] rip
 *
 * setjmp saves the callee-saved registers + return address + stack pointer.
 * longjmp restores them and jumps to the saved rip with the given return value.
 * Per POSIX: longjmp(env, 0) is treated as longjmp(env, 1). */

    .text

    .global setjmp
    .global _setjmp
setjmp:
_setjmp:
    movq  %rbx,  0(%rdi)
    movq  %rbp,  8(%rdi)
    movq  %r12, 16(%rdi)
    movq  %r13, 24(%rdi)
    movq  %r14, 32(%rdi)
    movq  %r15, 40(%rdi)
    leaq  8(%rsp), %rax        /* rsp as it will be after the caller's ret */
    movq  %rax,  48(%rdi)
    movq  (%rsp), %rax         /* return address = saved rip */
    movq  %rax,  56(%rdi)
    xorl  %eax, %eax           /* return 0 */
    ret

    .global longjmp
    .global _longjmp
longjmp:
_longjmp:
    movq   0(%rdi), %rbx
    movq   8(%rdi), %rbp
    movq  16(%rdi), %r12
    movq  24(%rdi), %r13
    movq  32(%rdi), %r14
    movq  40(%rdi), %r15
    movq  48(%rdi), %rsp
    movq  56(%rdi), %rcx       /* saved rip */
    movl  %esi, %eax           /* return value */
    testl %eax, %eax
    jnz   1f
    movl  $1, %eax             /* longjmp(env, 0) returns 1 */
1:  jmpq  *%rcx
