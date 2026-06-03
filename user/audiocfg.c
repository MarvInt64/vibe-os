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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../kernel/include/syscall.h"

/* Syscall wrapper */
static int64_t syscall(uint64_t number, uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    int64_t ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(number), "D"(arg0), "S"(arg1), "d"(arg2) : "memory");
    return ret;
}

int main(int argc, char *argv[]) {
    // Basic debugging
    if (argc < 3) {
        printf("Audiocfg: argc=%d\n", argc);
        printf("Usage: audiocfg <command> <value>\n");
        printf("Commands: set_rate, set_buffer_size, set_buffer_count, set_volume\n");
        return 1;
    }

    int request = -1;
    if (strcmp(argv[1], "set_rate") == 0) request = 0;
    else if (strcmp(argv[1], "set_buffer_size") == 0) request = 1;
    else if (strcmp(argv[1], "set_buffer_count") == 0) request = 2;
    else if (strcmp(argv[1], "set_volume") == 0) request = 3;

    if (request == -1) {
        printf("Unknown command: %s\n", argv[1]);
        return 1;
    }

    uint32_t val = (uint32_t)atoi(argv[2]);
    printf("Audiocfg: calling syscall 51, request=%d, val=%d\n", request, val);
    if (syscall(51, request, (uint64_t)&val, 0) == 0) {
        printf("Set %s to %d\n", argv[1], val);
    } else {
        printf("Failed to set %s\n", argv[1]);
    }
    return 0;
}
