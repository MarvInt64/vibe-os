#ifndef VIBEOS_STRING_H
#define VIBEOS_STRING_H

#include "types.h"

void *memcpy(void *dest, const void *src, size_t count);
void *memcpy_fast(void *dest, const void *src, size_t count);
void *memmove(void *dest, const void *src, size_t count);
void *memset(void *dest, int value, size_t count);
void *memset32(void *dest, uint32_t value, size_t count);
int memcmp(const void *left, const void *right, size_t count);
size_t strlen(const char *text);
size_t string_length(const char *text);
int strcmp(const char *left, const char *right);
char *strcpy(char *dest, const char *src);
char *strncpy(char *dest, const char *src, size_t count);
char *strrchr(const char *str, int c);

#endif
