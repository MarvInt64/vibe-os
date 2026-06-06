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

#include "string.h"

/* These are the hot path for every blit, fill and surface copy, so they use
 * the x86 string instructions (rep movs/stos) instead of scalar C loops. On
 * modern CPUs `rep movsb` is the recommended, ERMSB-optimized bulk copy, and
 * the sequential burst it emits is exactly what a write-combining framebuffer
 * wants. (The kernel runs with the direction flag clear by ABI convention.) */

#ifndef ARCH_ARM64
/* x86_64: use string instructions for maximum throughput. */
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
    if (out == in || count == 0) return dest;
    if (out < in || out >= in + count) return memcpy(dest, src, count);
    {
        uint8_t *d = out + count - 1;
        const uint8_t *s = in + count - 1;
        size_t n = count;
        __asm__ volatile("std; rep movsb; cld"
                         : "+D"(d), "+S"(s), "+c"(n)
                         : : "memory");
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

#else /* ARCH_ARM64 — plain C, compiler will emit efficient NEON/LDP-STP */

void *memcpy(void *dest, const void *src, size_t count) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < count; i++) d[i] = s[i];
    return dest;
}

void *memmove(void *dest, const void *src, size_t count) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    if (d == s || count == 0) return dest;
    if (d < s) { for (size_t i = 0; i < count; i++) d[i] = s[i]; }
    else       { for (size_t i = count; i-- > 0;) d[i] = s[i]; }
    return dest;
}

void *memset(void *dest, int value, size_t count) {
    uint8_t *d = (uint8_t *)dest;
    for (size_t i = 0; i < count; i++) d[i] = (uint8_t)value;
    return dest;
}

void *memset32(void *dest, uint32_t value, size_t count) {
    uint32_t *d = (uint32_t *)dest;
    for (size_t i = 0; i < count; i++) d[i] = value;
    return dest;
}

#endif /* ARCH_ARM64 */

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
