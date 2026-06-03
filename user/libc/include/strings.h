#ifndef VIBEOS_STRINGS_H
#define VIBEOS_STRINGS_H
#include <string.h>
static inline int strcasecmp(const char *a, const char *b) {
    while (*a && *b) {
        int d = (*a|32) - (*b|32); if (d) return d; ++a; ++b;
    }
    return (*a|32) - (*b|32);
}
static inline int strncasecmp(const char *a, const char *b, unsigned long n) {
    while (n && *a && *b) {
        int d = (*a|32) - (*b|32); if (d) return d; ++a; ++b; --n;
    }
    return n ? ((*a|32) - (*b|32)) : 0;
}
static inline void bzero(void *s, unsigned long n) { __builtin_memset(s, 0, n); }
static inline void bcopy(const void *s, void *d, unsigned long n) { __builtin_memmove(d, s, n); }
#endif
