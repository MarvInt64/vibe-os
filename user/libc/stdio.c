/*
 * MIT License
 *
 * Copyright (c) 2026 Marvin Kicha
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/types.h>

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

/* ---- FILE stream: buffered I/O over kernel file descriptors ------------ *
 *
 * VibeOS has no fd-level truncation primitive (writing fewer bytes than a file
 * already holds does not shrink it), so "w" mode truncates by unlinking the
 * path before creating it — a fresh zero-length file. Reads and writes are
 * buffered through a single per-stream BUFSIZ buffer to avoid one syscall per
 * byte; the buffer is used for read-ahead or write-behind, never both at once,
 * and the direction is switched transparently when a "+" stream alternates.
 */
#include <stdlib.h>

/* fp->flags bits. */
#define _F_READ   0x01   /* stream is readable                 */
#define _F_WRITE  0x02   /* stream is writable                 */
#define _F_APPEND 0x04   /* every write goes to end-of-file    */
#define _F_EOF    0x08   /* end-of-file reached                */
#define _F_ERR    0x10   /* an I/O error occurred              */

/* fp->bufmode values. */
#define _B_IDLE   0      /* buffer empty / direction not chosen */
#define _B_READ   1      /* buffer holds read-ahead data        */
#define _B_WRITE  2      /* buffer holds pending writes         */

/* The standard streams are unbuffered (buf == NULL): output appears
 * immediately (important for interactive prompts) and input is read directly.
 * printf/puts/putchar bypass the FILE layer entirely (see above). */
static FILE s_stdin  = { .fd = STDIN_FILENO,  .flags = _F_READ };
static FILE s_stdout = { .fd = STDOUT_FILENO, .flags = _F_WRITE };
static FILE s_stderr = { .fd = STDERR_FILENO, .flags = _F_WRITE };
FILE *stdin  = &s_stdin;
FILE *stdout = &s_stdout;
FILE *stderr = &s_stderr;

/* Write the whole [data, data+len) range to fd, looping over short writes.
 * Returns 0 on success, -1 if any write fails. */
static int write_all_fd(int fd, const unsigned char *data, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, data + off, len - off);
        if (w <= 0) return -1;
        off += (size_t)w;
    }
    return 0;
}

/* Flush pending write-behind data to the file. No-op unless in write mode. */
static int flush_write(FILE *fp) {
    if (fp->bufmode != _B_WRITE || fp->bufpos == 0) return 0;
    int rc = write_all_fd(fp->fd, fp->buf, fp->bufpos);
    fp->bufpos = 0;
    if (rc != 0) { fp->flags |= _F_ERR; return EOF; }
    return 0;
}

/* Drop unread read-ahead data, rewinding the fd so the logical position
 * matches what the caller has actually consumed. No-op unless in read mode. */
static void discard_read(FILE *fp) {
    if (fp->bufmode == _B_READ && fp->buflen > fp->bufpos)
        lseek(fp->fd, -(off_t)(fp->buflen - fp->bufpos), SEEK_CUR);
    fp->bufpos = fp->buflen = 0;
}

FILE *fopen(const char *path, const char *mode) {
    if (!path || !mode) return (FILE *)0;
    extern int vos_open_path(const char *path);
    extern int vos_creat_path(const char *path);

    /* Parse the mode string: first letter plus optional '+' (read+write). */
    short flags = 0;
    int truncate = 0, append = 0;
    switch (mode[0]) {
        case 'r': flags = _F_READ;  break;
        case 'w': flags = _F_WRITE; truncate = 1; break;
        case 'a': flags = _F_WRITE; append   = 1; break;
        default:  return (FILE *)0;
    }
    for (const char *m = mode + 1; *m; ++m)
        if (*m == '+') flags |= (_F_READ | _F_WRITE);

    int fd;
    if (flags & _F_WRITE) {
        if (truncate) unlink(path);        /* "w": discard any existing file */
        fd = vos_creat_path(path);         /* create (or open) for writing    */
    } else {
        fd = vos_open_path(path);          /* "r": must already exist         */
    }
    if (fd < 0) return (FILE *)0;
    if (append) lseek(fd, 0, SEEK_END);

    FILE *fp = (FILE *)malloc(sizeof(FILE));
    if (!fp) { close(fd); return (FILE *)0; }
    fp->fd = fd; fp->flags = flags; fp->bufmode = _B_IDLE;
    fp->bufpos = fp->buflen = 0; fp->own_buf = 0;
    fp->mem = (const unsigned char *)0; fp->mem_len = fp->mem_pos = 0;

    /* Allocate the I/O buffer; falling back to unbuffered if memory is tight. */
    fp->buf = (unsigned char *)malloc(BUFSIZ);
    fp->bufcap = fp->buf ? BUFSIZ : 0;
    fp->own_buf = fp->buf ? 1 : 0;
    return fp;
}

FILE *fmemopen(void *buf, size_t size, const char *mode) {
    (void)mode;   /* only read-only memory streams are supported */
    FILE *fp = (FILE *)malloc(sizeof(FILE));
    if (!fp) return (FILE *)0;
    fp->fd = -1; fp->flags = _F_READ; fp->bufmode = _B_IDLE;
    fp->buf = (unsigned char *)0; fp->bufcap = fp->bufpos = fp->buflen = 0;
    fp->own_buf = 0;
    fp->mem = (const unsigned char *)buf; fp->mem_len = size; fp->mem_pos = 0;
    return fp;
}

int setvbuf(FILE *fp, char *buf, int mode, size_t size) {
    if (!fp || fp->fd < 0) return -1;
    /* Must be called before any I/O: flush/reset first to be safe. */
    flush_write(fp); discard_read(fp); fp->bufmode = _B_IDLE;
    if (fp->own_buf && fp->buf) { free(fp->buf); }
    fp->own_buf = 0; fp->buf = (unsigned char *)0; fp->bufcap = 0;
    if (mode == _IONBF) return 0;                 /* unbuffered           */
    if (size < 64) size = BUFSIZ;
    if (buf) { fp->buf = (unsigned char *)buf; fp->bufcap = size; }
    else     { fp->buf = (unsigned char *)malloc(size);
               fp->bufcap = fp->buf ? size : 0; fp->own_buf = fp->buf ? 1 : 0; }
    return fp->buf ? 0 : -1;
}

void setbuf(FILE *fp, char *buf) {
    setvbuf(fp, buf, buf ? _IOFBF : _IONBF, BUFSIZ);
}

int fclose(FILE *fp) {
    if (!fp) return EOF;
    int rc = flush_write(fp);
    if (fp->fd > STDERR_FILENO) close(fp->fd);
    if (fp->own_buf && fp->buf) free(fp->buf);
    free(fp);
    return rc;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *fp) {
    if (!fp || !(fp->flags & _F_READ) || (fp->flags & _F_ERR)) return 0;
    size_t want = size * nmemb;
    if (want == 0) return 0;

    unsigned char *out = (unsigned char *)ptr;
    size_t done = 0;

    /* In-memory stream: copy straight out of the backing buffer. */
    if (fp->fd < 0) {
        size_t left = fp->mem_len - fp->mem_pos;
        size_t n = want < left ? want : left;
        __builtin_memcpy(out, fp->mem + fp->mem_pos, n);
        fp->mem_pos += n;
        if (n < want) fp->flags |= _F_EOF;
        return n / size;
    }

    if (fp->bufmode == _B_WRITE) flush_write(fp);
    fp->bufmode = _B_READ;

    while (done < want) {
        /* Serve from the buffer first. */
        if (fp->bufpos < fp->buflen) {
            size_t avail = fp->buflen - fp->bufpos;
            size_t n = (want - done) < avail ? (want - done) : avail;
            __builtin_memcpy(out + done, fp->buf + fp->bufpos, n);
            fp->bufpos += n; done += n;
            continue;
        }
        /* Buffer empty: refill (or, when unbuffered, read straight through). */
        if (fp->buf) {
            ssize_t got = read(fp->fd, fp->buf, fp->bufcap);
            if (got <= 0) { fp->flags |= (got == 0 ? _F_EOF : _F_ERR); break; }
            fp->buflen = (size_t)got; fp->bufpos = 0;
        } else {
            ssize_t got = read(fp->fd, out + done, want - done);
            if (got <= 0) { fp->flags |= (got == 0 ? _F_EOF : _F_ERR); break; }
            done += (size_t)got;
        }
    }
    return done / size;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *fp) {
    if (!fp || !(fp->flags & _F_WRITE) || fp->fd < 0) return 0;
    size_t want = size * nmemb;
    if (want == 0) return 0;

    if (fp->bufmode == _B_READ) discard_read(fp);
    fp->bufmode = _B_WRITE;

    const unsigned char *in = (const unsigned char *)ptr;

    /* Unbuffered: write straight through. */
    if (!fp->buf) {
        if (write_all_fd(fp->fd, in, want) != 0) { fp->flags |= _F_ERR; return 0; }
        return nmemb;
    }

    size_t done = 0;
    while (done < want) {
        size_t space = fp->bufcap - fp->bufpos;
        if (space == 0) {
            if (flush_write(fp) != 0) break;   /* error already flagged */
            space = fp->bufcap;
        }
        size_t n = (want - done) < space ? (want - done) : space;
        __builtin_memcpy(fp->buf + fp->bufpos, in + done, n);
        fp->bufpos += n; done += n;
    }
    return done / size;
}

int fflush(FILE *fp) {
    if (!fp) return 0;
    return flush_write(fp);
}

int fseek(FILE *fp, long offset, int whence) {
    if (!fp) return -1;
    if (fp->fd >= 0) {
        flush_write(fp);            /* commit pending writes ... */
        discard_read(fp);           /* ... and drop read-ahead   */
        fp->bufmode = _B_IDLE;
        off_t r = lseek(fp->fd, (off_t)offset, whence);
        if (r < 0) { fp->flags |= _F_ERR; return -1; }
        fp->flags &= ~_F_EOF;
        return 0;
    }
    /* in-memory stream */
    long new_pos;
    if (whence == SEEK_SET) new_pos = offset;
    else if (whence == SEEK_CUR) new_pos = (long)fp->mem_pos + offset;
    else new_pos = (long)fp->mem_len + offset;
    if (new_pos < 0) new_pos = 0;
    if (new_pos > (long)fp->mem_len) new_pos = (long)fp->mem_len;
    fp->mem_pos = (size_t)new_pos; fp->flags &= ~_F_EOF;
    return 0;
}

long ftell(FILE *fp) {
    if (!fp) return -1;
    if (fp->fd < 0) return (long)fp->mem_pos;
    long pos = (long)lseek(fp->fd, 0, SEEK_CUR);
    if (pos < 0) return -1;
    /* Account for data sitting in the buffer the caller hasn't seen/committed. */
    if (fp->bufmode == _B_READ)  pos -= (long)(fp->buflen - fp->bufpos);
    else if (fp->bufmode == _B_WRITE) pos += (long)fp->bufpos;
    return pos;
}

void rewind(FILE *fp) { fseek(fp, 0, SEEK_SET); if (fp) fp->flags &= ~_F_ERR; }
int  feof(FILE *fp)    { return fp && (fp->flags & _F_EOF); }
int  ferror(FILE *fp)  { return fp && (fp->flags & _F_ERR); }
void clearerr(FILE *fp){ if (fp) fp->flags &= ~(_F_EOF | _F_ERR); }

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
    if (!fp || fp->fd < 0) return -1;
    /* Commit any buffered data so the formatted output stays correctly
     * ordered, then write it straight to the descriptor. */
    if (fp->bufmode == _B_WRITE) flush_write(fp);
    else if (fp->bufmode == _B_READ) discard_read(fp);
    fp->bufmode = _B_IDLE;
    return vdprintf(fp->fd, fmt, ap);
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

/* ---- sprintf: output to string (unbounded) ---------------------------- */
int sprintf(char *buf, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, (size_t)-1, fmt, ap);
    va_end(ap);
    return r;
}

/* ---- getline: read a line into malloc'd buffer ------------------------ */
#include <stdlib.h>
#include <errno.h>
ssize_t getline(char **lineptr, size_t *n, FILE *stream) {
    if (!lineptr || !n || !stream) { errno = EINVAL; return -1; }
    if (!*lineptr || *n == 0) {
        *n = 128;
        *lineptr = (char *)malloc(*n);
        if (!*lineptr) return -1;
    }
    size_t pos = 0;
    int c;
    while ((c = fgetc(stream)) != EOF) {
        if (pos + 1 >= *n) {
            size_t newsz = *n * 2;
            char *newbuf = (char *)realloc(*lineptr, newsz);
            if (!newbuf) return -1;
            *lineptr = newbuf;
            *n = newsz;
        }
        (*lineptr)[pos++] = (char)c;
        if (c == '\n') break;
    }
    if (pos == 0) return -1;
    (*lineptr)[pos] = '\0';
    return (ssize_t)pos;
}

/* ---- fdopen: wrap a file descriptor in a FILE* ----------------------- */
FILE *fdopen(int fd, const char *mode) {
    FILE *fp = (FILE *)malloc(sizeof(FILE));
    if (!fp) return (FILE *)0;
    fp->fd = fd; fp->flags = 0; fp->bufmode = 0;
    fp->bufpos = fp->buflen = 0; fp->own_buf = 0;
    fp->mem = (const unsigned char *)0; fp->mem_len = fp->mem_pos = 0;
    fp->buf = (unsigned char *)0; fp->bufcap = 0;
    while (*mode) {
        switch (*mode++) {
            case 'r': fp->flags |= 0x01; break;  /* _F_READ  */
            case 'w': fp->flags |= 0x02; break;  /* _F_WRITE */
            case 'a': fp->flags |= 0x06; break;  /* _F_WRITE|_F_APPEND */
        }
    }
    return fp;
}

/* ---- freopen: close existing stream, open new file on the same FILE ----
 * POSIX: freopen() first closes the file associated with fp (ignoring
 * errors), then opens 'path' with 'mode' and associates it with fp.
 * Returns fp on success, NULL on failure (original fp is closed either way).
 */
FILE *freopen(const char *path, const char *mode, FILE *fp) {
    if (!fp || !path || !mode) return (FILE *)0;

    /* Flush and close the existing stream. */
    fflush(fp);
    if (fp->fd >= 0) {
        close(fp->fd);
        fp->fd = -1;
    }
    if (fp->own_buf && fp->buf) {
        free(fp->buf);
        fp->buf = (unsigned char *)0;
        fp->bufcap = 0;
    }
    fp->bufpos = fp->buflen = 0;
    fp->flags = 0;

    /* Open the new file. */
    int nfd = -1;
    /* Decode mode string: r=read, w=write, a=append (same as fopen). */
    int mflags = 0;
    const char *m = mode;
    while (*m) {
        switch (*m++) {
            case 'r': mflags |= 0x01; break;            /* _F_READ */
            case 'w': mflags |= 0x02; break;            /* _F_WRITE */
            case 'a': mflags |= 0x06; break;            /* _F_WRITE|_F_APPEND */
            case 'b': break;                            /* binary (no-op) */
            case '+': mflags |= 0x03; break;            /* read+write */
        }
    }

    extern int vos_open_path(const char *);
    extern int vos_creat_path(const char *);
    if (mflags & 0x02) {
        /* Write/append: create if not exists. */
        nfd = vos_open_path(path);
        if (nfd < 0) nfd = vos_creat_path(path);
        if (nfd >= 0 && !(mflags & 0x04)) lseek(nfd, 0, SEEK_SET); /* truncate */
        if (nfd >= 0 && (mflags & 0x04)) lseek(nfd, 0, SEEK_END);  /* append */
    } else {
        nfd = vos_open_path(path);
    }
    if (nfd < 0) return (FILE *)0;

    fp->fd = nfd;
    fp->flags = mflags;
    fp->bufmode = _IOFBF;
    fp->own_buf = 1;
    fp->buf = (unsigned char *)malloc(BUFSIZ);
    fp->bufcap = fp->buf ? BUFSIZ : 0;
    fp->bufpos = fp->buflen = 0;
    fp->mem = (const unsigned char *)0;
    fp->mem_len = fp->mem_pos = 0;
    return fp;
}
