#ifndef VIBEOS_STDIO_H
#define VIBEOS_STDIO_H
#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Formatted output -------------------------------------------------- */
int printf(const char *fmt, ...);
int dprintf(int fd, const char *fmt, ...);    /* fd-based output */
int snprintf(char *buf, size_t size, const char *fmt, ...);
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int vdprintf(int fd, const char *fmt, va_list ap);
int puts(const char *s);
int putchar(int c);

/* ---- FILE stream ------------------------------------------------------- *
 * Buffered byte stream over a kernel file descriptor (or a read-only in-memory
 * region created with fmemopen). The struct is opaque: callers must only use
 * the f* functions below — the layout may change between releases.
 *
 * A single buffer is used either for read-ahead or write-behind, never both at
 * once (tracked internally); switching direction on a "r+"/"w+" stream is
 * handled transparently by flushing/discarding the buffer first. */
typedef struct {
    int   fd;          /* underlying descriptor; -1 for in-memory streams       */
    short flags;       /* permission + state bits (private)                     */
    short bufmode;     /* current buffer use: idle/reading/writing (private)    */
    unsigned char *buf;/* I/O buffer; NULL means the stream is unbuffered       */
    size_t bufcap;     /* capacity of buf                                       */
    size_t bufpos;     /* next byte to read from / write into buf               */
    size_t buflen;     /* number of valid bytes in buf (read direction)         */
    int    own_buf;    /* non-zero if buf was allocated by the library          */
    /* read-only in-memory backing (fmemopen) */
    const unsigned char *mem;
    size_t mem_len;
    size_t mem_pos;
} FILE;

#define EOF          (-1)
#define SEEK_SET       0
#define SEEK_CUR       1
#define SEEK_END       2

/* Default buffer size and setvbuf() buffering modes (C standard names). */
#define BUFSIZ      4096
#define _IOFBF         0   /* fully buffered                            */
#define _IOLBF         1   /* line buffered (treated as fully buffered) */
#define _IONBF         2   /* unbuffered                                */

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

FILE   *fopen(const char *path, const char *mode);
FILE   *fmemopen(void *buf, size_t size, const char *mode);  /* read-only mem stream */
int     fclose(FILE *fp);
int     setvbuf(FILE *fp, char *buf, int mode, size_t size);
void    setbuf(FILE *fp, char *buf);
size_t  fread(void *ptr, size_t size, size_t nmemb, FILE *fp);
size_t  fwrite(const void *ptr, size_t size, size_t nmemb, FILE *fp);
int     fseek(FILE *fp, long offset, int whence);
long    ftell(FILE *fp);
void    rewind(FILE *fp);
int     feof(FILE *fp);
int     ferror(FILE *fp);
void    clearerr(FILE *fp);
int     fflush(FILE *fp);
char   *fgets(char *buf, int n, FILE *fp);
int     fgetc(FILE *fp);
int     fputc(int c, FILE *fp);
int     fputs(const char *s, FILE *fp);
int     fprintf(FILE *fp, const char *fmt, ...);
int     vfprintf(FILE *fp, const char *fmt, va_list ap);
/* sscanf — minimal stub: %d %s %f only */
int sscanf(const char *str, const char *fmt, ...);

/* sprintf — output to string (unbounded, caller must ensure buffer is large enough) */
int sprintf(char *buf, const char *fmt, ...);

/* getline — read a line from a FILE stream into a malloc'd buffer */
ssize_t getline(char **lineptr, size_t *n, FILE *stream);

#ifdef __cplusplus
}
#endif

#endif

