/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Marvin Kicha <https://github.com/MarvInt64/> */

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
                else if (*fmt == '+' || *fmt == ' ') ; /* ignore */
                else break;
            }
            /* width */
            while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); ++fmt; }
            /* .precision — for integers: minimum digits (zero-pad); for strings: max chars */
            int prec = -1;
            if (*fmt == '.') {
                ++fmt; prec = 0;
                while (*fmt >= '0' && *fmt <= '9') { prec = prec * 10 + (*fmt - '0'); ++fmt; }
                /* %.Nd on integers: treat as zero-filled width if wider than current */
                if (prec > width) { width = prec; zero = 1; }
            }
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
                    if (prec >= 0 && len > prec) len = prec;
                    if (!left) { int pad = width - len; while (pad-- > 0) emit(s, ' '); }
                    { int k; for (k = 0; k < len; ++k) emit(s, str[k]); }
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

int dprintf(int fd, const char *fmt, ...) {
    va_list ap; int r;
    va_start(ap, fmt); r = vdprintf(fd, fmt, ap); va_end(ap);
    return r;
}

int puts(const char *str) {
    size_t n = 0; while (str[n]) ++n;
    write(STDOUT_FILENO, str, n); write(STDOUT_FILENO, "\n", 1); return 0;
}
int putchar(int c) { char ch = (char)c; write(STDOUT_FILENO, &ch, 1); return c; }

/* ---- FILE shim --------------------------------------------------------- */
#include <stdlib.h>

static FILE s_stdin  = { .fd = STDIN_FILENO };
static FILE s_stdout = { .fd = STDOUT_FILENO };
static FILE s_stderr = { .fd = STDERR_FILENO };
FILE *stdin  = &s_stdin;
FILE *stdout = &s_stdout;
FILE *stderr = &s_stderr;

FILE *fopen(const char *path, const char *mode) {
    int fd = -1;
    /* Open: 'r'/'rb' → read-only, 'w'/'wb'/'a' → write (create/trunc). */
    if (mode && (mode[0] == 'r')) {
        /* SYS_OPEN always opens for read */
        extern int vos_open_path(const char *path);
        fd = vos_open_path(path);
    } else {
        /* write/append: use SYS_CREAT */
        extern int vos_creat_path(const char *path);
        fd = vos_creat_path(path);
    }
    if (fd < 0) return (FILE *)0;
    FILE *fp = (FILE *)malloc(sizeof(FILE));
    if (!fp) { extern int close(int); close(fd); return (FILE *)0; }
    fp->fd = fd; fp->mem = (void *)0; fp->mem_len = 0; fp->mem_pos = 0;
    fp->eof = 0; fp->error = 0;
    if (mode && mode[0] == 'a') { lseek(fd, 0, SEEK_END); }
    return fp;
}

FILE *fmemopen(void *buf, size_t size, const char *mode) {
    (void)mode;
    FILE *fp = (FILE *)malloc(sizeof(FILE));
    if (!fp) return (FILE *)0;
    fp->fd = -1; fp->mem = (const unsigned char *)buf; fp->mem_len = size;
    fp->mem_pos = 0; fp->eof = 0; fp->error = 0;
    return fp;
}

int fclose(FILE *fp) {
    if (!fp) return EOF;
    if (fp->fd >= 0 && fp->fd > STDERR_FILENO) close(fp->fd);
    free(fp);
    return 0;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *fp) {
    if (!fp || fp->error || fp->eof) return 0;
    size_t want = size * nmemb;
    if (want == 0) return 0;
    ssize_t got;
    if (fp->fd >= 0) {
        got = read(fp->fd, ptr, want);
    } else {
        size_t left = fp->mem_len - fp->mem_pos;
        got = (ssize_t)(want < left ? want : left);
        __builtin_memcpy(ptr, fp->mem + fp->mem_pos, (size_t)got);
        fp->mem_pos += (size_t)got;
    }
    if (got <= 0) { if (got == 0) fp->eof = 1; else fp->error = 1; return 0; }
    return (size_t)got / size;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *fp) {
    if (!fp || fp->fd < 0) return 0;
    size_t want = size * nmemb;
    ssize_t r = write(fp->fd, ptr, want);
    return r > 0 ? (size_t)r / size : 0;
}

int fseek(FILE *fp, long offset, int whence) {
    if (!fp) return -1;
    if (fp->fd >= 0) {
        off_t r = lseek(fp->fd, (off_t)offset, whence);
        if (r < 0) { fp->error = 1; return -1; }
        fp->eof = 0;
        return 0;
    }
    /* in-memory */
    long new_pos;
    if (whence == SEEK_SET) new_pos = offset;
    else if (whence == SEEK_CUR) new_pos = (long)fp->mem_pos + offset;
    else new_pos = (long)fp->mem_len + offset;
    if (new_pos < 0) new_pos = 0;
    if (new_pos > (long)fp->mem_len) new_pos = (long)fp->mem_len;
    fp->mem_pos = (size_t)new_pos; fp->eof = 0;
    return 0;
}

long ftell(FILE *fp) {
    if (!fp) return -1;
    if (fp->fd >= 0) return (long)lseek(fp->fd, 0, SEEK_CUR);
    return (long)fp->mem_pos;
}

void rewind(FILE *fp) { fseek(fp, 0, SEEK_SET); if (fp) fp->error = 0; }
int  feof(FILE *fp)    { return fp && fp->eof; }
int  ferror(FILE *fp)  { return fp && fp->error; }
void clearerr(FILE *fp){ if (fp) { fp->eof = 0; fp->error = 0; } }
int  fflush(FILE *fp)  { (void)fp; return 0; }

char *fgets(char *buf, int n, FILE *fp) {
    if (!fp || n <= 1) return (char *)0;
    int i = 0;
    while (i < n - 1) {
        int c = fgetc(fp);
        if (c == EOF) { if (i == 0) return (char *)0; break; }
        buf[i++] = (char)c;
        if (c == '\n') break;
    }
    buf[i] = '\0';
    return buf;
}

int fgetc(FILE *fp) {
    unsigned char c;
    if (fread(&c, 1, 1, fp) != 1) return EOF;
    return (int)c;
}

int fputc(int c, FILE *fp) {
    if (!fp) return EOF;
    unsigned char ch = (unsigned char)c;
    return (fwrite(&ch, 1, 1, fp) == 1) ? c : EOF;
}

int fputs(const char *s, FILE *fp) {
    if (!s || !fp) return EOF;
    size_t n = 0; while (s[n]) ++n;
    return (fwrite(s, 1, n, fp) == n) ? 0 : EOF;
}

int vfprintf(FILE *fp, const char *fmt, va_list ap) {
    if (!fp) return -1;
    if (fp->fd >= 0) return vdprintf(fp->fd, fmt, ap);
    return -1;
}

int fprintf(FILE *fp, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vfprintf(fp, fmt, ap);
    va_end(ap); return r;
}

/* Minimal sscanf: supports %d %u %f %s (no width limits). */
int sscanf(const char *str, const char *fmt, ...) {
    __builtin_va_list ap; __builtin_va_start(ap, fmt);
    int count = 0;
    const char *s = str;
    while (*fmt && *s) {
        if (*fmt == ' ') { while (*s == ' ' || *s == '\t') ++s; ++fmt; continue; }
        if (*fmt != '%') { if (*s == *fmt) { ++s; ++fmt; } else break; continue; }
        ++fmt;
        if (*fmt == 'd' || *fmt == 'i') {
            int neg = 0, val = 0;
            while (*s == ' ') ++s;
            if (*s == '-') { neg=1; ++s; } else if (*s=='+') ++s;
            while (*s>='0'&&*s<='9') { val=val*10+(*s-'0'); ++s; }
            *__builtin_va_arg(ap,int*) = neg?-val:val; ++count;
        } else if (*fmt == 'u') {
            unsigned val=0; while(*s==' ')++s;
            while(*s>='0'&&*s<='9'){val=val*10+(*s-'0');++s;}
            *__builtin_va_arg(ap,unsigned*)=val; ++count;
        } else if (*fmt == 'f') {
            double r=0,f=1; int neg=0; while(*s==' ')++s;
            if(*s=='-'){neg=1;++s;} while(*s>='0'&&*s<='9'){r=r*10+(*s-'0');++s;}
            if(*s=='.'){++s;while(*s>='0'&&*s<='9'){f/=10;r+=(*s-'0')*f;++s;}}
            *__builtin_va_arg(ap,float*)=(float)(neg?-r:r); ++count;
        } else if (*fmt == 's') {
            char *out=__builtin_va_arg(ap,char*); while(*s==' ')++s;
            while(*s&&*s!=' '&&*s!='\t'&&*s!='\n') *out++=*s++;
            *out='\0'; ++count;
        }
        ++fmt;
    }
    __builtin_va_end(ap);
    return count;
}
