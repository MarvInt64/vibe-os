/* VibeOS arm64 — minimal userspace libc implementation. */
#include "vlibc.h"

/* Syscall numbers (match arm64_sync_handler_el0): 1=write 2=open 3=read
 * 4=exit 5=close. ABI: x8=number, x0..x2=args, svc #0. */
static long syscall3(long n, long a0, long a1, long a2) {
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
    return x0;
}

long sys_write(int fd, const void *buf, unsigned long len) {
    return syscall3(1, fd, (long)buf, (long)len);
}
long sys_open(const char *path)  { return syscall3(2, (long)path, 0, 0); }
long sys_read(int fd, void *buf, unsigned long len) {
    return syscall3(3, fd, (long)buf, (long)len);
}
long sys_close(int fd)           { return syscall3(5, fd, 0, 0); }
void sys_exit(int code)          { syscall3(4, code, 0, 0); __builtin_unreachable(); }

size_t strlen_(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

void puts_(const char *s) { sys_write(1, s, strlen_(s)); }

void putln(const char *s) {
    sys_write(1, s, strlen_(s));
    sys_write(1, "\r\n", 2);
}

void put_uint(unsigned long v) {
    char buf[21];
    int i = 0;
    if (v == 0) { sys_write(1, "0", 1); return; }
    while (v) { buf[i++] = (char)('0' + v % 10); v /= 10; }
    char out[21];
    int j = 0;
    while (i) out[j++] = buf[--i];
    sys_write(1, out, j);
}

void put_hex(unsigned long v) {
    const char *hx = "0123456789abcdef";
    char out[18];
    out[0] = '0'; out[1] = 'x';
    for (int i = 0; i < 16; i++)
        out[2 + i] = hx[(v >> ((15 - i) * 4)) & 0xf];
    sys_write(1, out, 18);
}
