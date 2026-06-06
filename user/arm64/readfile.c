/* VibeOS arm64 — userspace file reader (proves open/read/write/close).
 *
 * Opens /journal.log, reads it in chunks, writes each chunk to stdout, then
 * closes and exits. All via SVC syscalls — no libc.
 *
 * Syscall numbers: 1=write, 2=open, 3=read, 4=exit, 5=close.
 */
#include <stdint.h>

static long syscall3(long n, long a0, long a1, long a2) {
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
    return x0;
}

static long sys_write(int fd, const void *b, unsigned long n) { return syscall3(1, fd, (long)b, (long)n); }
static long sys_open (const char *p)                         { return syscall3(2, (long)p, 0, 0); }
static long sys_read (int fd, void *b, unsigned long n)      { return syscall3(3, fd, (long)b, (long)n); }
static void sys_exit (int code)                              { syscall3(4, code, 0, 0); __builtin_unreachable(); }
static long sys_close(int fd)                                { return syscall3(5, fd, 0, 0); }

static unsigned long slen(const char *s){ unsigned long n=0; while(s[n]) n++; return n; }
static void puts_(const char *s){ sys_write(1, s, slen(s)); }

void _start(void) {
    const char *path = "/journal.log";
    puts_("readfile: opening ");
    puts_(path);
    puts_("\r\n----\r\n");

    long fd = sys_open(path);
    if (fd < 0) {
        puts_("readfile: open failed\r\n");
        sys_exit(1);
    }

    static char buf[512];
    long total = 0;
    for (;;) {
        long n = sys_read((int)fd, buf, sizeof(buf));
        if (n <= 0) break;
        sys_write(1, buf, (unsigned long)n);
        total += n;
    }
    sys_close((int)fd);

    puts_("\r\n----\r\n");
    puts_("readfile: done\r\n");
    sys_exit(0);
}
