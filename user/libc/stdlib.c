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

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "umalloc.h"

void *malloc(size_t size) { return umalloc((umsize_t)size); }
void  free(void *ptr) { ufree(ptr); }
void *realloc(void *ptr, size_t size) { return urealloc(ptr, (umsize_t)size); }

void *calloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    void *p;
    if (nmemb && total / nmemb != size) return 0;   /* overflow */
    p = umalloc((umsize_t)total);
    if (p) memset(p, 0, total);
    return p;
}

int abs(int v) { return v < 0 ? -v : v; }

/* ---- atexit / exit -------------------------------------------------------
 * POSIX: atexit() registers handlers called in reverse order at exit().
 * exit() runs all handlers, then calls _exit().  Handlers registered during
 * handler execution are called in the current round (not deferred).
 */
#define ATEXIT_MAX 64

static void (*atexit_handlers[ATEXIT_MAX])(void);
static int atexit_count = 0;

int atexit(void (*func)(void)) {
    if (!func) return -1;
    if (atexit_count >= ATEXIT_MAX) return -1;
    atexit_handlers[atexit_count++] = func;
    return 0;
}

void exit(int code) {
    /* Call handlers in reverse registration order.  A handler may register
     * more handlers — process them in the same round by checking the growing
     * count each iteration. */
    int i = atexit_count;
    while (i > 0) {
        --i;
        if (atexit_handlers[i])
            atexit_handlers[i]();
    }
    _exit(code);
}

void abort(void) { _exit(134); }

static int digit_val(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return -1;
}

unsigned long strtoul(const char *s, char **end, int base) {
    unsigned long v = 0;
    const char *p = s;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
    if (*p == '+') ++p;
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { p += 2; base = 16; }
    else if (base == 0 && p[0] == '0') { base = 8; }
    else if (base == 0) { base = 10; }
    for (;;) {
        int d = digit_val((unsigned char)*p);
        if (d < 0 || d >= base) break;
        v = v * (unsigned long)base + (unsigned long)d;
        ++p;
    }
    if (end) *end = (char *)p;
    return v;
}

long strtol(const char *s, char **end, int base) {
    const char *p = s;
    int neg = 0;
    unsigned long v;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
    if (*p == '-') { neg = 1; ++p; }
    else if (*p == '+') { ++p; }
    v = strtoul(p, end, base);
    return neg ? -(long)v : (long)v;
}

int atoi(const char *s) { return (int)strtol(s, 0, 10); }

static unsigned long g_rand_seed = 1;

int rand(void) {
    g_rand_seed = g_rand_seed * 1103515245 + 12345;
    return (int)(g_rand_seed / 65536) % 32768;
}

void srand(unsigned int seed) {
    g_rand_seed = seed;
}

size_t malloc_usable_size(void *ptr) {
    return (size_t)umalloc_usable_size(ptr);
}

/* ---- Environment variables -----------------------------------------------
 * A simple char** array of "KEY=VALUE" strings, stored on the heap.
 * environ is the public pointer; getenv/setenv/putenv manipulate it.
 */
char **environ = (char **)0;
static int environ_count = 0;
static int environ_cap = 0;

char *getenv(const char *name) {
    if (!name || !environ) return (char *)0;
    int nlen = 0;
    while (name[nlen]) ++nlen;
    for (int i = 0; i < environ_count; ++i) {
        char *entry = environ[i];
        if (!entry) continue;
        /* Check if entry starts with name followed by '=' */
        int j;
        for (j = 0; j < nlen && entry[j] && entry[j] == name[j]; ++j);
        if (j == nlen && entry[j] == '=')
            return entry + nlen + 1;  /* pointer to value after '=' */
    }
    return (char *)0;
}

int setenv(const char *name, const char *value, int overwrite) {
    if (!name || !value || !name[0]) return -1;
    if (overwrite == 0 && getenv(name)) return 0;

    /* Build "NAME=VALUE" string */
    int nlen = 0, vlen = 0;
    while (name[nlen]) ++nlen;
    while (value[vlen]) ++vlen;
    char *entry = (char *)malloc((unsigned long)(nlen + vlen + 2));
    if (!entry) return -1;
    for (int i = 0; i < nlen; ++i) entry[i] = name[i];
    entry[nlen] = '=';
    for (int i = 0; i < vlen; ++i) entry[nlen + 1 + i] = value[i];
    entry[nlen + vlen + 1] = '\0';

    /* Replace existing entry if name matches */
    for (int i = 0; i < environ_count; ++i) {
        char *e = environ[i];
        if (!e) continue;
        int j;
        for (j = 0; j < nlen && e[j] && e[j] == name[j]; ++j);
        if (j == nlen && e[j] == '=') {
            free(e);
            environ[i] = entry;
            return 0;
        }
    }

    /* Not found — append. Grow array if needed. */
    if (environ_count >= environ_cap) {
        int ncap = environ_cap ? environ_cap * 2 : 8;
        char **nenv = (char **)realloc(environ, (unsigned long)ncap * sizeof(char *));
        if (!nenv) { free(entry); return -1; }
        environ = nenv;
        /* Zero new slots */
        for (int i = environ_cap; i < ncap; ++i) environ[i] = (char *)0;
        environ_cap = ncap;
    }
    environ[environ_count++] = entry;
    return 0;
}

int putenv(char *string) {
    if (!string) return -1;
    /* Find '=' to extract name */
    const char *eq = string;
    while (*eq && *eq != '=') ++eq;
    if (!*eq) return -1;  /* no '=' in string */
    /* Use setenv with overwrite. We don't own 'string' so copy it. */
    int nlen = (int)(eq - string);
    char *name = (char *)malloc((unsigned long)(nlen + 1));
    if (!name) return -1;
    for (int i = 0; i < nlen; ++i) name[i] = string[i];
    name[nlen] = '\0';
    int ret = setenv(name, eq + 1, 1);
    free(name);
    return ret;
}

int unsetenv(const char *name) {
    if (!name || !name[0] || !environ) return -1;
    int nlen = 0;
    while (name[nlen]) ++nlen;
    for (int i = 0; i < environ_count; ++i) {
        char *e = environ[i];
        if (!e) continue;
        int j;
        for (j = 0; j < nlen && e[j] && e[j] == name[j]; ++j);
        if (j == nlen && e[j] == '=') {
            free(e);
            /* Shift remaining entries down. */
            for (int k = i; k < environ_count - 1; ++k)
                environ[k] = environ[k + 1];
            environ[--environ_count] = (char *)0;
            return 0;
        }
    }
    return 0;  /* not found = success */
}

/* ---- qsort: standard quicksort ---------------------------------------- */
static void qs_swap(char *a, char *b, size_t size) {
    size_t i;
    for (i = 0; i < size; ++i) {
        char t = a[i]; a[i] = b[i]; b[i] = t;
    }
}

static char *qs_partition(char *lo, char *hi, size_t size,
                          int (*cmp)(const void *, const void *)) {
    char *pivot = lo;
    char *i = lo + size;
    char *j;
    for (j = lo + size; j <= hi; j += size) {
        if (cmp(j, pivot) < 0) {
            qs_swap(i, j, size);
            i += size;
        }
    }
    qs_swap(lo, i - size, size);
    return i - size;
}

static void qs_rec(char *lo, char *hi, size_t size,
                   int (*cmp)(const void *, const void *)) {
    if (lo >= hi) return;
    char *p = qs_partition(lo, hi, size, cmp);
    qs_rec(lo, p - size, size, cmp);
    qs_rec(p + size, hi, size, cmp);
}

void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *)) {
    if (nmemb <= 1 || size == 0) return;
    qs_rec((char *)base, (char *)base + (nmemb - 1) * size, size, compar);
}

/* ---- strtoll / strtoull — long long variants ------------------------- */
static int digit_val_ll(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return -1;
}

static unsigned long long parse_ull(const char *p, char **end, int base) {
    unsigned long long v = 0;
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { p += 2; base = 16; }
    else if (base == 0 && p[0] == '0') { base = 8; }
    else if (base == 0) { base = 10; }
    for (;;) {
        int d = digit_val_ll((unsigned char)*p);
        if (d < 0 || d >= base) break;
        v = v * (unsigned long long)base + (unsigned long long)d;
        ++p;
    }
    if (end) *end = (char *)p;
    return v;
}

unsigned long long strtoull(const char *s, char **end, int base) {
    const char *p = s;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
    if (*p == '+') ++p;
    return parse_ull(p, end, base);
}

long long strtoll(const char *s, char **end, int base) {
    const char *p = s;
    int neg = 0;
    unsigned long long v;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
    if (*p == '-') { neg = 1; ++p; }
    else if (*p == '+') { ++p; }
    v = parse_ull(p, end, base);
    return neg ? -(long long)v : (long long)v;
}

/* ---- strtod / strtof / strtold — floating-point parsing --------------- */
double strtod(const char *s, char **end) {
    double r = 0.0, f = 1.0;
    int neg = 0;
    const char *p = s;
    while (*p == ' ' || *p == '\t') ++p;
    if (*p == '-') { neg = 1; ++p; } else if (*p == '+') ++p;
    /* Integer part */
    while (*p >= '0' && *p <= '9') { r = r * 10.0 + (double)(*p - '0'); ++p; }
    /* Fractional part */
    if (*p == '.') {
        ++p;
        while (*p >= '0' && *p <= '9') { f /= 10.0; r += (double)(*p - '0') * f; ++p; }
    }
    /* Exponent part (e/E) */
    if (*p == 'e' || *p == 'E') {
        ++p;
        int esign = 1, eval = 0;
        if (*p == '-') { esign = -1; ++p; } else if (*p == '+') ++p;
        while (*p >= '0' && *p <= '9') { eval = eval * 10 + (*p - '0'); ++p; }
        double ep = 1.0;
        int i;
        for (i = 0; i < eval; ++i) ep *= 10.0;
        if (esign > 0) r *= ep; else r /= ep;
    }
    if (end) *end = (char *)p;
    return neg ? -r : r;
}

float strtof(const char *s, char **end) {
    return (float)strtod(s, end);
}

long double strtold(const char *s, char **end) {
    return (long double)strtod(s, end);
}

/* ---- mkstemp / mkstemps / futimens stubs ------------------- */
int mkstemp(char *templ) { (void)templ; return -1; }
int mkstemps(char *templ, int suffixlen) { (void)templ; (void)suffixlen; return -1; }
int futimens(int fd, const struct timespec *ts) { (void)fd; (void)ts; return 0; }

