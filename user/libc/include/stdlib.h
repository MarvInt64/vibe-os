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

#ifdef __cplusplus
}
#endif

#endif
