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
 * <clipboard.h> — system-wide text clipboard API
 *
 * Any process can read and write the shared clipboard.  Content is a raw
 * byte sequence; by convention it is UTF-8 / ASCII text.
 *
 * Typical usage:
 *
 *   // Copy selected text to clipboard:
 *   clipboard_set(selected_text, strlen(selected_text));
 *
 *   // Paste clipboard content into a buffer:
 *   char buf[CLIPBOARD_MAX + 1];
 *   size_t n = clipboard_get(buf, sizeof(buf));
 *   // buf[0..n-1] contains the text, buf[n] == '\0'
 */

#ifndef VIBEOS_CLIPBOARD_H
#define VIBEOS_CLIPBOARD_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum clipboard content size — matches the kernel limit. */
#define CLIPBOARD_MAX 65536

/*
 * clipboard_set — replace the clipboard with data[0..len).
 *
 * Returns 0 on success, or -1 when len > CLIPBOARD_MAX.
 * Passing a NUL-terminated string: clipboard_set(str, strlen(str)).
 */
int clipboard_set(const char *data, size_t len);

/*
 * clipboard_get — copy clipboard content into buf[0..cap-1].
 *
 * Always NUL-terminates buf when cap > 0, even if the clipboard is empty.
 * Returns the number of bytes copied, not counting the terminating NUL.
 * When cap == 0 or buf is NULL, returns 0 without touching buf.
 */
size_t clipboard_get(char *buf, size_t cap);

/*
 * clipboard_len — return the current clipboard content length in bytes.
 *
 * Returns 0 when the clipboard is empty.
 */
size_t clipboard_len(void);

#ifdef __cplusplus
}
#endif

#endif /* VIBEOS_CLIPBOARD_H */
