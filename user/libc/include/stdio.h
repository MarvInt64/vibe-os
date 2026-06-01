#ifndef VIBEOS_STDIO_H
#define VIBEOS_STDIO_H
#include <stddef.h>
#include <stdarg.h>

/* Minimal stdio: formatted output to a file descriptor or a buffer. No FILE
 * buffering, no input scanf, no floats (userspace has no FPU). Supported
 * conversions: %d %i %u %x %X %o %c %s %p %% with width, '0'/'-' flags, and the
 * l / ll / z length modifiers. */

#ifdef __cplusplus
extern "C" {
#endif

int printf(const char *fmt, ...);
int fprintf(int fd, const char *fmt, ...);     /* fd-based (use STDOUT/STDERR_FILENO) */
int snprintf(char *buf, size_t size, const char *fmt, ...);
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int vdprintf(int fd, const char *fmt, va_list ap);

int puts(const char *s);
int putchar(int c);
int fputs(const char *s, int fd);

#ifdef __cplusplus
}
#endif

#endif
