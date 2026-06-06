#ifndef VIBEOS_STDIO_H
#define VIBEOS_STDIO_H
#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

int printf(const char *fmt, ...);
int dprintf(int fd, const char *fmt, ...);
int snprintf(char *buf, size_t size, const char *fmt, ...);
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int vdprintf(int fd, const char *fmt, va_list ap);
int puts(const char *s);
int putchar(int c);

typedef struct {
    int   fd;
    short flags;
    short bufmode;
    unsigned char *buf;
    size_t bufcap;
    size_t bufpos;
    size_t buflen;
    int    own_buf;
    const unsigned char *mem;
    size_t mem_len;
    size_t mem_pos;
} FILE;

#define EOF          (-1)
#define SEEK_SET       0
#define SEEK_CUR       1
#define SEEK_END       2
#define BUFSIZ      4096
#define _IOFBF         0
#define _IOLBF         1
#define _IONBF         2

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

FILE   *fopen(const char *path, const char *mode);
FILE   *fmemopen(void *buf, size_t size, const char *mode);
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
#define putc(c, fp)  fputc(c, fp)
int     fputs(const char *s, FILE *fp);
int     fprintf(FILE *fp, const char *fmt, ...);
int     vfprintf(FILE *fp, const char *fmt, va_list ap);
int sscanf(const char *str, const char *fmt, ...);

int sprintf(char *buf, const char *fmt, ...);
ssize_t getline(char **lineptr, size_t *n, FILE *stream);
FILE *fdopen(int fd, const char *mode);
FILE *freopen(const char *path, const char *mode, FILE *fp);

#ifdef __cplusplus
}
#endif
#endif
