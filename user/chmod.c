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
 * chmod — change file permission bits
 *
 * Usage: chmod <octal-mode> <file> [file2 ...]
 *
 * Mode is an octal number in the range 0–777, e.g.:
 *   chmod 644 readme.txt   → rw-r--r--
 *   chmod 755 /bin/myprog  → rwxr-xr-x
 *
 * The calling process must be the file owner or root (uid 0).
 * Returns 0 if all changes succeeded, 1 if any failed.
 */

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fputs("Usage: chmod <octal-mode> <file> [file2 ...]\n", stderr);
        return 1;
    }

    /* Parse the mode as an octal integer (strtol with base 8). */
    char *end;
    long mode = strtol(argv[1], &end, 8);
    if (*end != '\0' || mode < 0 || mode > 0777) {
        fprintf(stderr, "chmod: invalid mode: %s\n", argv[1]);
        return 1;
    }

    int status = 0;
    for (int i = 2; i < argc; i++) {
        if (chmod(argv[i], (int)mode) < 0) {
            fprintf(stderr, "chmod: %s: permission denied or not found\n", argv[i]);
            status = 1;
        }
    }
    return status;
}
