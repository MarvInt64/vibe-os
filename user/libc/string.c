#include <string.h>

void *memcpy(void *dst, const void *src, size_t n) {
    unsigned char *d = dst; const unsigned char *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    unsigned char *d = dst; const unsigned char *s = src;
    if (d == s || n == 0) return dst;
    if (d < s) { while (n--) *d++ = *s++; }
    else { d += n; s += n; while (n--) *--d = *--s; }
    return dst;
}

void *memset(void *dst, int c, size_t n) {
    unsigned char *d = dst;
    while (n--) *d++ = (unsigned char)c;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = a, *y = b;
    for (; n--; ++x, ++y) if (*x != *y) return (int)*x - (int)*y;
    return 0;
}

void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = s;
    for (; n--; ++p) if (*p == (unsigned char)c) return (void *)p;
    return 0;
}

size_t strlen(const char *s) { size_t n = 0; while (s[n]) ++n; return n; }
size_t strnlen(const char *s, size_t max) { size_t n = 0; while (n < max && s[n]) ++n; return n; }

char *strcpy(char *dst, const char *src) {
    char *d = dst;
    while ((*d++ = *src++)) {}
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; ++i) dst[i] = src[i];
    for (; i < n; ++i) dst[i] = 0;
    return dst;
}

char *strcat(char *dst, const char *src) {
    char *d = dst;
    while (*d) ++d;
    while ((*d++ = *src++)) {}
    return dst;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { ++a; ++b; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    while (n && *a && *a == *b) { ++a; ++b; --n; }
    if (n == 0) return 0;
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

char *strchr(const char *s, int c) {
    for (; *s; ++s) if (*s == (char)c) return (char *)s;
    return (c == 0) ? (char *)s : 0;
}

char *strrchr(const char *s, int c) {
    const char *last = 0;
    for (; ; ++s) { if (*s == (char)c) last = s; if (!*s) break; }
    return (char *)last;
}

char *strstr(const char *hay, const char *needle) {
    if (!*needle) return (char *)hay;
    for (; *hay; ++hay) {
        const char *h = hay, *n = needle;
        while (*h && *n && *h == *n) { ++h; ++n; }
        if (!*n) return (char *)hay;
    }
    return 0;
}

size_t strlcpy(char *dst, const char *src, size_t size) {
    size_t n = 0;
    if (size) { for (; n + 1 < size && src[n]; ++n) dst[n] = src[n]; dst[n] = 0; }
    while (src[n]) ++n;   /* count the full source length */
    return n;
}

size_t strspn(const char *s, const char *accept) {
    const char *p = s;
    while (*p) {
        const char *a = accept;
        while (*a && *a != *p) ++a;
        if (!*a) break;
        ++p;
    }
    return (size_t)(p - s);
}

size_t strcspn(const char *s, const char *reject) {
    const char *p = s;
    while (*p) {
        const char *r = reject;
        while (*r && *r != *p) ++r;
        if (*r) break;
        ++p;
    }
    return (size_t)(p - s);
}

char *strpbrk(const char *s, const char *accept) {
    for (; *s; ++s) {
        const char *a = accept;
        while (*a) {
            if (*a == *s) return (char *)s;
            ++a;
        }
    }
    return 0;
}

char *strtok_r(char *s, const char *delim, char **saveptr) {
    char *token;
    if (s == 0) s = *saveptr;
    if (s == 0) return 0;

    s += strspn(s, delim);
    if (*s == '\0') {
        *saveptr = 0;
        return 0;
    }

    token = s;
    s += strcspn(s, delim);
    if (*s == '\0') {
        *saveptr = 0;
    } else {
        *s = '\0';
        *saveptr = s + 1;
    }
    return token;
}

char *strtok(char *s, const char *delim) {
    static char *saveptr;
    return strtok_r(s, delim, &saveptr);
}
