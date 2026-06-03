#ifndef VIBEOS_STRING_H
#define VIBEOS_STRING_H
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void  *memcpy(void *dst, const void *src, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
void  *memset(void *dst, int c, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
void  *memchr(const void *s, int c, size_t n);

size_t strlen(const char *s);
size_t strnlen(const char *s, size_t max);
char  *strcpy(char *dst, const char *src);
char  *strncpy(char *dst, const char *src, size_t n);
char  *strcat(char *dst, const char *src);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);
char  *strstr(const char *hay, const char *needle);

/* Bounded copy (size of dst buffer); always NUL-terminates. Returns strlen(src). */
size_t strlcpy(char *dst, const char *src, size_t size);
size_t strspn(const char *s, const char *accept);
size_t strcspn(const char *s, const char *reject);
char  *strpbrk(const char *s, const char *accept);
char  *strtok(char *s, const char *delim);
char  *strtok_r(char *s, const char *delim, char **saveptr);

/* POSIX extensions (inside the guard) */
static inline char *strdup(const char *s) {
    extern void *malloc(unsigned long);
    unsigned long n = 0; while (s[n]) ++n;
    char *p = (char *)malloc(n + 1);
    if (p) { unsigned long i; for(i=0;i<=n;++i) p[i]=s[i]; }
    return p;
}
static inline char *strndup(const char *s, unsigned long n) {
    extern void *malloc(unsigned long);
    unsigned long len = 0; while (len < n && s[len]) ++len;
    char *p = (char *)malloc(len + 1);
    if (p) { unsigned long i; for(i=0;i<len;++i) p[i]=s[i]; p[len]='\0'; }
    return p;
}

#ifdef __cplusplus
}
#endif

#endif
