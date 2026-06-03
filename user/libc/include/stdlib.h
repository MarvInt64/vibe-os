#ifndef VIBEOS_STDLIB_H
#define VIBEOS_STDLIB_H
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void *malloc(size_t size);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);
void  free(void *ptr);

int   atoi(const char *s);
long  strtol(const char *s, char **end, int base);
unsigned long strtoul(const char *s, char **end, int base);

int   abs(int v);

void  exit(int code) __attribute__((noreturn));
void  abort(void) __attribute__((noreturn));

static inline double atof(const char *s) {
    double r = 0, f = 1; int neg = 0; const char *p = s;
    while (*p == ' ') ++p;
    if (*p == '-') { neg = 1; ++p; } else if (*p == '+') ++p;
    while (*p >= '0' && *p <= '9') { r = r*10.0 + (double)(*p-'0'); ++p; }
    if (*p == '.') { ++p; while (*p >= '0' && *p <= '9') { f/=10.0; r+=(double)(*p-'0')*f; ++p; } }
    return neg ? -r : r;
}
static inline int  remove(const char *p) { (void)p; return -1; }
static inline int  rename(const char *a, const char *b) { (void)a;(void)b; return -1; }
static inline int  system(const char *s) { (void)s; return -1; }

#ifdef __cplusplus
}
#endif

#endif
