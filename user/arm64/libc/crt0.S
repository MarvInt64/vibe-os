/* VibeOS arm64 userspace crt0.
 *
 * ELF entry point. The kernel enters here at EL0 with:
 *   x0 = argc, x1 = argv (pointer array), sp = top of user stack.
 * We call main(argc, argv); whatever it returns becomes exit(code).
 *
 * Apps just write `int main(int argc, char **argv)` (or `int main(void)`).
 */
.section ".text._start", "ax"
.global _start

_start:
    /* x0/x1 already hold argc/argv as set up by the kernel exec path.
     * Align the stack to 16 bytes (AArch64 ABI requires it at calls). The
     * kernel already aligns user_sp, but be defensive. SP can't be an AND
     * destination directly, so round-trip through a temp register. */
    mov  x9, sp
    and  x9, x9, #0xfffffffffffffff0
    mov  sp, x9

    bl   main                    /* w0 = main(argc, argv) return code */

    /* exit(main_ret): x8=4 (SYS_EXIT), x0 = return value (already in w0/x0) */
    mov  x8, #4
    svc  #0

.hang:
    b    .hang
