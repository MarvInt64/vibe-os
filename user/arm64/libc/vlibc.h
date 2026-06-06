/* VibeOS arm64 — minimal userspace libc.
 *
 * A tiny freestanding C library for EL0 programs: syscall wrappers plus a few
 * string/IO helpers, so apps can be written as `int main(...)` instead of
 * hand-rolling SVC sequences.
 */
#ifndef VIBEOS_ARM64_VLIBC_H
#define VIBEOS_ARM64_VLIBC_H

#include <stdint.h>
#include <stddef.h>

/* ---- raw syscalls ----------------------------------------------------- */
long sys_write(int fd, const void *buf, unsigned long len);
long sys_open(const char *path);
long sys_read(int fd, void *buf, unsigned long len);
long sys_close(int fd);
void sys_exit(int code) __attribute__((noreturn));

/* ---- convenience ------------------------------------------------------ */
size_t strlen_(const char *s);
void   puts_(const char *s);          /* write to stdout, no newline added */
void   putln(const char *s);          /* write to stdout + CRLF */
void   put_uint(unsigned long v);     /* decimal */
void   put_hex(unsigned long v);      /* 0x… */

#endif
