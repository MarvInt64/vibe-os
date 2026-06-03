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
 * clipboard — command-line clipboard utility
 *
 * Usage:
 *   clipboard copy  <text>   Write text to the system clipboard
 *   clipboard paste          Print the current clipboard content
 *   clipboard clear          Empty the clipboard
 *   clipboard len            Print the current content length in bytes
 *
 * This program is also the reference example for using <clipboard.h> from
 * any VibeOS app — the API is just three calls: clipboard_set / _get / _len.
 */

#include <stdio.h>
#include <string.h>
#include <clipboard.h>

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fputs("Usage: clipboard <copy <text> | paste | clear | len>\n", stderr);
        return 1;
    }

    if (strcmp(argv[1], "copy") == 0) {
        if (argc < 3) {
            fputs("clipboard copy: missing text argument\n", stderr);
            return 1;
        }

        /* Join all remaining arguments with spaces into one string. */
        char buf[CLIPBOARD_MAX];
        size_t pos = 0;
        for (int i = 2; i < argc && pos < CLIPBOARD_MAX - 1; i++) {
            if (i > 2 && pos < CLIPBOARD_MAX - 1) buf[pos++] = ' ';
            const char *src = argv[i];
            while (*src && pos < CLIPBOARD_MAX - 1) buf[pos++] = *src++;
        }
        buf[pos] = '\0';

        if (clipboard_set(buf, pos) == 0) {
            printf("clipboard: copied %zu bytes\n", pos);
        } else {
            fputs("clipboard: copy failed (content too large?)\n", stderr);
            return 1;
        }

    } else if (strcmp(argv[1], "paste") == 0) {
        size_t len = clipboard_len();
        if (len == 0) {
            puts("(clipboard is empty)");
            return 0;
        }

        /* Allocate one extra byte for the NUL that clipboard_get adds. */
        char buf[CLIPBOARD_MAX + 1];
        size_t n = clipboard_get(buf, sizeof(buf));
        /* Write exactly n bytes — clipboard content may not end with '\n'. */
        fwrite(buf, 1, n, stdout);
        if (n == 0 || buf[n - 1] != '\n') putchar('\n');

    } else if (strcmp(argv[1], "clear") == 0) {
        clipboard_set("", 0);
        puts("clipboard: cleared");

    } else if (strcmp(argv[1], "len") == 0) {
        printf("%zu\n", clipboard_len());

    } else {
        fprintf(stderr, "clipboard: unknown command '%s'\n", argv[1]);
        return 1;
    }

    return 0;
}
