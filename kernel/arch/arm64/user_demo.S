/* VibeOS arm64 — embedded EL0 user demo program.
 *
 * A tiny userspace program that runs at EL0 (unprivileged) and talks to the
 * kernel only via the SVC (supervisor call) instruction. It proves the full
 * EL1→EL0→EL1 round trip and the syscall path.
 *
 * Syscall ABI (Linux-style, simplified):
 *   x8 = syscall number
 *   x0..x2 = arguments
 *   svc #0 traps to the kernel's exc_el0_sync handler
 *
 * Syscalls used:
 *   1 = write(fd, buf, len)
 *   4 = exit(code)
 *
 * This blob is copied into a user page and entered via eret. It must be
 * fully position-independent (PC-relative addressing only) since the kernel
 * may map it at any EL0 virtual address.
 */
.section ".text.user", "ax"
.global user_demo_start
.global user_demo_end

user_demo_start:
    /* write(1, msg, msg_len) */
    mov   x0, #1                 /* fd = stdout */
    adr   x1, .msg              /* PC-relative pointer to message */
    mov   x2, #(.msg_end - .msg) /* length */
    mov   x8, #1                 /* SYS_WRITE */
    svc   #0

    /* write a second line to prove repeated syscalls work */
    mov   x0, #1
    adr   x1, .msg2
    mov   x2, #(.msg2_end - .msg2)
    mov   x8, #1
    svc   #0

    /* exit(42) */
    mov   x0, #42
    mov   x8, #4                 /* SYS_EXIT */
    svc   #0

    /* should never reach here; loop just in case */
.hang:
    b     .hang

.msg:
    .ascii "Hello from EL0 userspace (arm64)!\r\n"
.msg_end:
.msg2:
    .ascii "Syscalls via SVC work end-to-end.\r\n"
.msg2_end:

user_demo_end:
