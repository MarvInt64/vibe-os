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

/* crt0 — C/C++ runtime entry. The kernel jumps to _start with rsp = top of the
 * user stack and no return address; returning from _start would `ret` into
 * garbage. So _start runs static constructors, calls main(), runs registered
 * destructors and then _exit() and never returns.
 *
 * Apps just write `int main(void)`. */
#include <unistd.h>
#include <vibeos.h>

typedef void (*init_fn)(void);

extern init_fn __init_array_start[];
extern init_fn __init_array_end[];
extern init_fn __fini_array_start[];
extern init_fn __fini_array_end[];
extern int main(int argc, char *argv[]) __attribute__((weak));
extern int _Z4mainv(void) __attribute__((weak));  /* clang++ freestanding: int main() */
extern void __cxa_finalize(void *dso);

static void run_init_array(void) {
    init_fn *fn;
    for (fn = __init_array_start; fn != __init_array_end; ++fn) {
        if (*fn) (*fn)();
    }
}

static void run_fini_array(void) {
    init_fn *fn = __fini_array_end;
    while (fn != __fini_array_start) {
        --fn;
        if (*fn) (*fn)();
    }
}

#define MAX_ARGS 32

void __attribute__((noreturn)) _start(void) {
    int code;
    int argc = 1;
    static char *argv[MAX_ARGS];
    static char arg_buf[256];

    argv[0] = "app";

    int len = vos_getarg(arg_buf, sizeof(arg_buf) - 1);
    if (len > 0) {
        arg_buf[len] = '\0';
        char *p = arg_buf;
        while (*p && argc < MAX_ARGS - 1) {
            // skip spaces
            while (*p == ' ' || *p == '\t') {
                p++;
            }
            if (!*p) {
                break;
            }
            argv[argc++] = p;
            // find end of argument
            while (*p && *p != ' ' && *p != '\t') {
                p++;
            }
            if (*p) {
                *p = '\0';
                p++;
            }
        }
    }
    argv[argc] = NULL;

    run_init_array();
    if (main) code = main(argc, argv);
    else if (_Z4mainv) code = _Z4mainv();
    else code = 127;
    __cxa_finalize(0);
    run_fini_array();
    _exit(code);
    for (;;) { sched_yield_(); }   /* unreachable */
}

