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
long long strtoll(const char *s, char **end, int base);
unsigned long long strtoull(const char *s, char **end, int base);
double strtod(const char *s, char **end);
float strtof(const char *s, char **end);
long double strtold(const char *s, char **end);

int   abs(int v);

#define RAND_MAX 32767
int   rand(void);
void  srand(unsigned int seed);

void  exit(int code) __attribute__((noreturn));
void  abort(void) __attribute__((noreturn));
int   atexit(void (*func)(void));

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

/* environ — process environment */
extern char **environ;

char *getenv(const char *name);
int   setenv(const char *name, const char *value, int overwrite);
int   putenv(char *string);
int   unsetenv(const char *name);
void  qsort(void *base, size_t nmemb, size_t size,
            int (*compar)(const void *, const void *));

/* mkstemp / mkstemps — create unique temp file (stub) */
int mkstemp(char *templ);
int mkstemps(char *templ, int suffixlen);

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

/* realpath — no symlinks on VibeOS, just return absolute-ish path.
 * POSIX: when `resolved` is NULL the function must allocate a buffer the caller
 * frees. tcc's #pragma once handling (normalized_PATHCMP) calls realpath(f,NULL)
 * and frees the result; returning NULL there broke #pragma once entirely, so a
 * header that relies on it recursed without bound and tcc exhausted memory.
 * The previous body also wrote '\\0' (a two-char constant, value '0') instead
 * of the NUL terminator, leaving the path unterminated. */
static inline char *realpath(const char *path, char *resolved) {
    char *out;
    int i;
    if (!path) return (char *)0;
    out = resolved ? resolved : (char *)malloc(4096);
    if (!out) return (char *)0;
    for (i = 0; path[i] && i < 4095; ++i) out[i] = path[i];
    out[i] = '\0';
    return out;
}

#ifdef __cplusplus
}
#endif

#endif
