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

/*
 * kernel/src/clipboard.c — system-wide clipboard implementation
 *
 * A single global buffer lives here.  Processes reach it through three
 * syscalls (SYS_CLIPBOARD_SET / _GET / _LEN) handled in process.c.
 *
 * No locking is needed: VibeOS runs on a single core and the scheduler
 * never preempts a process inside a syscall, so clipboard_set / _get
 * are always atomic from the caller's perspective.
 */

#include "clipboard.h"
#include "string.h"   /* memcpy */

/* The clipboard lives in BSS — zero-initialised, no heap allocation. */
static char     g_buf[CLIPBOARD_MAX_BYTES];
static uint32_t g_len = 0;

int clipboard_set(const char *data, uint32_t len) {
    if (!data || len > CLIPBOARD_MAX_BYTES) return -1;

    memcpy(g_buf, data, len);
    g_len = len;
    return 0;
}

uint32_t clipboard_get(char *buf, uint32_t cap) {
    if (!buf || cap == 0) return 0;

    /* Copy at most (cap - 1) bytes so there is always room for the NUL. */
    uint32_t n = (g_len < cap - 1) ? g_len : cap - 1;
    memcpy(buf, g_buf, n);
    buf[n] = '\0';
    return n;
}

uint32_t clipboard_len(void) {
    return g_len;
}
