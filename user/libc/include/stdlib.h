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

#define RAND_MAX 32767
int   rand(void);
void  srand(unsigned int seed);

void  exit(int code) __attribute__((noreturn));
void  abort(void) __attribute__((noreturn));

char *getenv(const char *name);
void  qsort(void *base, size_t nmemb, size_t size,
            int (*compar)(const void *, const void *));

/* mkstemp / mkstemps — create unique temp file (stub) */
int mkstemp(char *templ);
int mkstemps(char *templ, int suffixlen);

/* putenv — set environment variable (stub) */
int putenv(char *string);

#define alloca __builtin_alloca
size_t malloc_usable_size(void *ptr);

#define P_tmpdir  "/tmp"

static inline double atof(const char *s) {
    double r = 0, f = 1; int neg = 0; const char *p = s;
    while (*p == ' ') ++p;
    if (*p == '-') { neg = 1; ++p; } else if (*p == '+') ++p;
    while (*p >= '0' && *p <= '9') { r = r*10.0 + (double)(*p-'0'); ++p; }
    if (*p == '.') { ++p; while (*p >= '0' && *p <= '9') { f/=10.0; r+=(double)(*p-'0')*f; ++p; } }
    return neg ? -r : r;
}
static inline int  remove(const char *p) { extern int unlink(const char *); return unlink(p); }
static inline int  rename(const char *a, const char *b) { (void)a;(void)b; return -1; }
static inline int  system(const char *s) { (void)s; return -1; }

/* realpath — no symlinks on VibeOS, just return absolute-ish path */
static inline char *realpath(const char *path, char *resolved) {
    if (!resolved) return (char *)0;
    if (!path) return (char *)0;
    /* Simple: just copy (VibeOS has no symlinks) */
    int i;
    for (i = 0; path[i] && i < 4095; ++i) resolved[i] = path[i];
    resolved[i] = '\\0';
    return resolved;
}

#ifdef __cplusplus
}
#endif

#endif
