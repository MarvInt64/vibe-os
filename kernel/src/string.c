/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Marvin Kicha <https://github.com/MarvInt64/> */

#include "string.h"

/* These are the hot path for every blit, fill and surface copy, so they use
 * the x86 string instructions (rep movs/stos) instead of scalar C loops. On
 * modern CPUs `rep movsb` is the recommended, ERMSB-optimized bulk copy, and
 * the sequential burst it emits is exactly what a write-combining framebuffer
 * wants. (The kernel runs with the direction flag clear by ABI convention.) */

void *memcpy(void *dest, const void *src, size_t count) {
    void *ret = dest;
    __asm__ volatile("rep movsb"
                     : "+D"(dest), "+S"(src), "+c"(count)
                     :
                     : "memory");
    return ret;
}

void *memmove(void *dest, const void *src, size_t count) {
    uint8_t *out = (uint8_t *)dest;
    const uint8_t *in = (const uint8_t *)src;

    if (out == in || count == 0) {
        return dest;
    }
    /* Forward copy is correct unless dest overlaps the tail of src; only then
     * do we copy backwards (high to low) to avoid clobbering unread bytes. */
    if (out < in || out >= in + count) {
        return memcpy(dest, src, count);
    }
    {
        uint8_t *d = out + count - 1;
        const uint8_t *s = in + count - 1;
        size_t n = count;
        __asm__ volatile("std; rep movsb; cld"
                         : "+D"(d), "+S"(s), "+c"(n)
                         :
                         : "memory");
    }
    return dest;
}

void *memset(void *dest, int value, size_t count) {
    void *ret = dest;
    __asm__ volatile("rep stosb"
                     : "+D"(dest), "+c"(count)
                     : "a"((uint8_t)value)
                     : "memory");
    return ret;
}

void *memset32(void *dest, uint32_t value, size_t count) {
    void *ret = dest;
    __asm__ volatile("rep stosl"
                     : "+D"(dest), "+c"(count)
                     : "a"(value)
                     : "memory");
    return ret;
}

int memcmp(const void *left, const void *right, size_t count) {
    const uint8_t *a = (const uint8_t *)left;
    const uint8_t *b = (const uint8_t *)right;
    size_t i;

    for (i = 0; i < count; ++i) {
        if (a[i] != b[i]) {
            return (int)a[i] - (int)b[i];
        }
    }

    return 0;
}

size_t string_length(const char *text) {
    size_t length = 0;

    if (text == 0) {
        return 0;
    }

    while (text[length] != '\0') {
        ++length;
    }

    return length;
}

size_t strlen(const char *text) {
    return string_length(text);
}

int strcmp(const char *left, const char *right) {
    if (left == 0 && right == 0) return 0;
    if (left == 0) return -1;
    if (right == 0) return 1;
    
    while (*left && *right && *left == *right) {
        left++;
        right++;
    }
    
    return (int)(unsigned char)*left - (int)(unsigned char)*right;
}

char *strcpy(char *dest, const char *src) {
    char *ret = dest;
    if (dest == 0 || src == 0) return dest;
    while ((*dest++ = *src++)) {}
    return ret;
}

char *strncpy(char *dest, const char *src, size_t count) {
    char *ret = dest;
    if (dest == 0 || src == 0) return dest;
    while (count > 0 && (*dest++ = *src++)) {
        count--;
    }
    while (count > 0) {
        *dest++ = '\0';
        count--;
    }
    return ret;
}

char *strrchr(const char *str, int c) {
    const char *last = 0;
    if (str == 0) return 0;
    while (*str) {
        if (*str == (char)c) {
            last = str;
        }
        str++;
    }
    return (char *)last;
}
