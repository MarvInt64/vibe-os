#include <stdio.h>
#include <unistd.h>
#include <stdint.h>

/* One-char-at-a-time output sink so snprintf and the fd printers share the
 * same formatter. */
struct sink { void (*put)(struct sink *, char); int count;
              char *buf; size_t cap; size_t n;          /* buffer sink */
              int fd; char tmp[256]; int t; };           /* fd sink */

static void sink_buf(struct sink *s, char c) {
    if (s->n + 1 < s->cap) s->buf[s->n] = c;
    s->n++; s->count++;
}
static void fd_flush(struct sink *s) { if (s->t) { write(s->fd, s->tmp, (size_t)s->t); s->t = 0; } }
static void sink_fd(struct sink *s, char c) {
    s->tmp[s->t++] = c; s->count++;
    if (s->t == (int)sizeof(s->tmp)) fd_flush(s);
}

static void emit(struct sink *s, char c) { s->put(s, c); }
static void emit_str(struct sink *s, const char *p) { while (*p) emit(s, *p++); }

/* Format an unsigned magnitude into digits[] (reversed length returned). */
static int utoa(unsigned long v, unsigned base, int upper, char *digits) {
    const char *lo = "0123456789abcdef", *hi = "0123456789ABCDEF";
    const char *d = upper ? hi : lo;
    int n = 0;
    if (v == 0) { digits[n++] = '0'; return n; }
    while (v) { digits[n++] = d[v % base]; v /= base; }
    return n;
}

static int format(struct sink *s, const char *fmt, va_list ap) {
    for (; *fmt; ++fmt) {
        if (*fmt != '%') { emit(s, *fmt); continue; }
        ++fmt;
        {
            int left = 0, zero = 0, width = 0, size64 = 0;
            char prefix[3]; int plen = 0;
            char num[24]; int nlen = 0;
            int neg = 0;

            /* flags */
            for (;; ++fmt) {
                if (*fmt == '-') left = 1;
                else if (*fmt == '0') zero = 1;
                else break;
            }
            /* width */
            while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); ++fmt; }
            /* length modifiers (all 64-bit on LP64) */
            if (*fmt == 'l') { size64 = 1; ++fmt; if (*fmt == 'l') ++fmt; }
            else if (*fmt == 'z') { size64 = 1; ++fmt; }

            switch (*fmt) {
                case 'd': case 'i': {
                    long v = size64 ? va_arg(ap, long) : (long)va_arg(ap, int);
                    unsigned long mag;
                    if (v < 0) { neg = 1; mag = ~(unsigned long)v + 1UL; } else mag = (unsigned long)v;
                    nlen = utoa(mag, 10, 0, num);
                    if (neg) prefix[plen++] = '-';
                    break;
                }
                case 'u': {
                    unsigned long v = size64 ? va_arg(ap, unsigned long) : (unsigned long)va_arg(ap, unsigned int);
                    nlen = utoa(v, 10, 0, num);
                    break;
                }
                case 'x': case 'X': {
                    unsigned long v = size64 ? va_arg(ap, unsigned long) : (unsigned long)va_arg(ap, unsigned int);
                    nlen = utoa(v, 16, *fmt == 'X', num);
                    break;
                }
                case 'o': {
                    unsigned long v = size64 ? va_arg(ap, unsigned long) : (unsigned long)va_arg(ap, unsigned int);
                    nlen = utoa(v, 8, 0, num);
                    break;
                }
                case 'p': {
                    unsigned long v = (unsigned long)(uintptr_t)va_arg(ap, void *);
                    prefix[plen++] = '0'; prefix[plen++] = 'x';
                    nlen = utoa(v, 16, 0, num);
                    break;
                }
                case 'c': { char c = (char)va_arg(ap, int); emit(s, c); continue; }
                case 's': {
                    const char *str = va_arg(ap, const char *);
                    int len = 0; if (!str) str = "(null)";
                    { const char *q = str; while (*q) { ++len; ++q; } }
                    if (!left) { int pad = width - len; while (pad-- > 0) emit(s, ' '); }
                    emit_str(s, str);
                    if (left) { int pad = width - len; while (pad-- > 0) emit(s, ' '); }
                    continue;
                }
                case '%': emit(s, '%'); continue;
                case 0: return s->count;
                default: emit(s, '%'); emit(s, *fmt); continue;
            }

            /* assemble number with prefix + padding */
            {
                int total = plen + nlen;
                int pad = width - total;
                int i;
                if (!left && !zero) { while (pad-- > 0) emit(s, ' '); }
                for (i = 0; i < plen; ++i) emit(s, prefix[i]);
                if (!left && zero) { while (pad-- > 0) emit(s, '0'); }
                for (i = nlen - 1; i >= 0; --i) emit(s, num[i]);
                if (left) { while (pad-- > 0) emit(s, ' '); }
            }
        }
    }
    return s->count;
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    struct sink s; s.put = sink_buf; s.count = 0; s.buf = buf; s.cap = size; s.n = 0;
    format(&s, fmt, ap);
    if (size) buf[s.n < size ? s.n : size - 1] = 0;
    return s.count;
}

int snprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap; int r;
    va_start(ap, fmt); r = vsnprintf(buf, size, fmt, ap); va_end(ap);
    return r;
}

int vdprintf(int fd, const char *fmt, va_list ap) {
    struct sink s; s.put = sink_fd; s.count = 0; s.fd = fd; s.t = 0;
    format(&s, fmt, ap);
    fd_flush(&s);
    return s.count;
}

int printf(const char *fmt, ...) {
    va_list ap; int r;
    va_start(ap, fmt); r = vdprintf(STDOUT_FILENO, fmt, ap); va_end(ap);
    return r;
}

int fprintf(int fd, const char *fmt, ...) {
    va_list ap; int r;
    va_start(ap, fmt); r = vdprintf(fd, fmt, ap); va_end(ap);
    return r;
}

int fputs(const char *str, int fd) {
    size_t n = 0; while (str[n]) ++n;
    write(fd, str, n);
    return 0;
}
int puts(const char *str) { fputs(str, STDOUT_FILENO); write(STDOUT_FILENO, "\n", 1); return 0; }
int putchar(int c) { char ch = (char)c; write(STDOUT_FILENO, &ch, 1); return c; }
