#ifndef VIBEOS_REGEX_H
#define VIBEOS_REGEX_H
#include <stddef.h>
typedef struct { int dummy; size_t re_nsub; } regex_t;
typedef struct { int rm_so; int rm_eo; } regmatch_t;
#define REG_EXTENDED 1
#define REG_ICASE 2
#define REG_NOSUB 4
#define REG_NOMATCH 1
static inline int regcomp(regex_t *r, const char *p, int f) { (void)r;(void)p;(void)f; return REG_NOMATCH; }
static inline int regexec(const regex_t *r, const char *s, size_t n, void *m, int f) { (void)r;(void)s;(void)n;(void)m;(void)f; return REG_NOMATCH; }
static inline void regfree(regex_t *r) { (void)r; }
static inline size_t regerror(int e, const regex_t *r, char *b, size_t s) { (void)e;(void)r;(void)b;(void)s; return 0; }
#endif
