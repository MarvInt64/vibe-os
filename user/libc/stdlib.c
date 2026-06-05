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

void exit(int code) { _exit(code); }
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

