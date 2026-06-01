#include "string.h"

void *memcpy(void *dest, const void *src, size_t count) {
    uint8_t *out = (uint8_t *)dest;
    const uint8_t *in = (const uint8_t *)src;
    size_t i;

    for (i = 0; i < count; ++i) {
        out[i] = in[i];
    }

    return dest;
}

void *memcpy_fast(void *dest, const void *src, size_t count) {
    uint64_t *out64 = (uint64_t *)dest;
    const uint64_t *in64 = (const uint64_t *)src;
    size_t count64 = count / 8;
    uint8_t *out8;
    const uint8_t *in8;
    size_t i;

    for (i = 0; i < count64; ++i) {
        out64[i] = in64[i];
    }

    out8 = (uint8_t *)(out64 + count64);
    in8 = (const uint8_t *)(in64 + count64);
    count = count & 7;
    for (i = 0; i < count; ++i) {
        out8[i] = in8[i];
    }

    return dest;
}

void *memmove(void *dest, const void *src, size_t count) {
    uint8_t *out = (uint8_t *)dest;
    const uint8_t *in = (const uint8_t *)src;
    size_t i;

    if (out == in || count == 0) {
        return dest;
    }

    if (out < in) {
        for (i = 0; i < count; ++i) {
            out[i] = in[i];
        }
    } else {
        for (i = count; i > 0; --i) {
            out[i - 1] = in[i - 1];
        }
    }

    return dest;
}

void *memset(void *dest, int value, size_t count) {
    uint8_t *out = (uint8_t *)dest;
    uint8_t byte = (uint8_t)value;
    size_t i;

    for (i = 0; i < count; ++i) {
        out[i] = byte;
    }

    return dest;
}

void *memset32(void *dest, uint32_t value, size_t count) {
    uint32_t *out = (uint32_t *)dest;
    size_t i;

    for (i = 0; i < count; ++i) {
        out[i] = value;
    }

    return dest;
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
