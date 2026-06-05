#ifndef VIBEOS_STRINGS_H
#define VIBEOS_STRINGS_H
#include <string.h>
static inline int strcasecmp(const char *a, const char *b) {
    while (*a && *b) {
        int d = (*a|32) - (*b|32); if (d) return d; ++a; ++b;
    }
    return (*a|32) - (*b|32);
}
static inline int strncasecmp(const char *a, const char *b, size_t n) {
    while (n && *a && *b) {
        int d = (*a|32) - (*b|32); if (d) return d; ++a; ++b; --n;
    }
    return n ? ((*a|32) - (*b|32)) : 0;
}
static inline void bzero(void *s, unsigned long n) { __builtin_memset(s, 0, n); }
static inline void bcopy(const void *s, void *d, unsigned long n) { __builtin_memmove(d, s, n); }

/* strcasestr — case-insensitive substring search */
static inline char *strcasestr(const char *haystack, const char *needle) {
    if (!*needle) return (char *)haystack;
    while (*haystack) {
        const char *h = haystack, *n = needle;
        while (*h && *n && ((*h | 32) == (*n | 32))) { ++h; ++n; }
        if (!*n) return (char *)haystack;
        ++haystack;
    }
    return (char *)0;
}
#endif
