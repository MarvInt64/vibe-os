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
 * kernel/include/clipboard.h — system-wide text clipboard
 *
 * A single static buffer shared across all processes.  Any process may
 * read or write it via the SYS_CLIPBOARD_* syscalls.  Content is always
 * treated as a raw byte sequence (caller decides encoding; convention is
 * UTF-8 / plain ASCII text).
 *
 * The 64 KB limit is intentional: large clipboard pastes in a hobby OS
 * would stall the kernel copy and cause noticeable latency.
 */

#ifndef VIBEOS_CLIPBOARD_H
#define VIBEOS_CLIPBOARD_H

#include "types.h"

/* Maximum number of bytes the clipboard can hold. */
#define CLIPBOARD_MAX_BYTES 65536

/*
 * Replace the clipboard contents with a copy of data[0..len).
 * Returns 0 on success, -1 if len > CLIPBOARD_MAX_BYTES.
 */
int clipboard_set(const char *data, uint32_t len);

/*
 * Copy up to 'cap' bytes of the current clipboard into buf.
 * Always NUL-terminates buf when cap > 0.
 * Returns the number of bytes actually copied (excluding the NUL).
 */
uint32_t clipboard_get(char *buf, uint32_t cap);

/* Return the current clipboard content length in bytes. */
uint32_t clipboard_len(void);

#endif /* VIBEOS_CLIPBOARD_H */
