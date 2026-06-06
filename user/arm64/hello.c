/* VibeOS arm64 — userspace hello, loaded from disk by `exec`.
 *
 * Freestanding EL0 program. Talks to the kernel only via SVC syscalls:
 *   x8 = number (1=write, 4=exit), x0..x2 = args.
 * Linked for VA 0x90000000 (see link.ld). No libc, no crt0 — _start is the
 * ELF entry point and never returns (it calls exit()).
 */
#include <stdint.h>

static long sys_write(int fd, const void *buf, unsigned long len) {
    register long x8 __asm__("x8") = 1;
    register long x0 __asm__("x0") = fd;
    register long x1 __asm__("x1") = (long)buf;
    register long x2 __asm__("x2") = (long)len;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
    return x0;
}

static void sys_exit(int code) {
    register long x8 __asm__("x8") = 4;
    register long x0 __asm__("x0") = code;
    __asm__ volatile("svc #0" :: "r"(x8), "r"(x0) : "memory");
    __builtin_unreachable();
}

static unsigned long slen(const char *s) {
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

static void puts_(const char *s) {
    sys_write(1, s, slen(s));
}

void _start(void) {
    puts_("Hello from a disk-loaded aarch64 ELF!\r\n");
    puts_("Running at EL0, linked for 0x90000000.\r\n");

    /* Print numbers 1..5 to prove computation + repeated syscalls work. */
    for (int i = 1; i <= 5; i++) {
        char c = (char)('0' + i);
        char line[4] = { ' ', ' ', c, '\n' };
        line[3] = '\n';
        sys_write(1, "  count ", 8);
        sys_write(1, &c, 1);
        sys_write(1, "\r\n", 2);
    }

    puts_("Done — exiting with code 7.\r\n");
    sys_exit(7);
}
