#ifndef VIBEOS_STDIO_H
#define VIBEOS_STDIO_H
#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>

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

/* ---- FILE stream (thin fd wrapper, no in-memory buffering) ------------- */
typedef struct {
    int  fd;        /* -1 for in-memory; >=0 for real fd */
    /* in-memory (read-only) variant */
    const unsigned char *mem;
    size_t mem_len;
    size_t mem_pos;
    int  eof;
    int  error;
} FILE;

#define EOF          (-1)
#define SEEK_SET       0
#define SEEK_CUR       1
#define SEEK_END       2

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

FILE   *fopen(const char *path, const char *mode);
FILE   *fmemopen(void *buf, size_t size, const char *mode);  /* read-only mem stream */
int     fclose(FILE *fp);
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

#ifdef __cplusplus
}
#endif

#endif

