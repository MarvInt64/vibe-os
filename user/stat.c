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
 * stat — display file or directory metadata
 *
 * Usage: stat <path> [path2 ...]
 *
 * Prints size and type for each path.  Exits 1 if any path cannot be
 * stat'd; processes remaining paths regardless.
 */

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fputs("Usage: stat <path> [path2 ...]\n", stderr);
        return 1;
    }

    int status = 0;
    for (int i = 1; i < argc; i++) {
        struct stat st;
        if (stat(argv[i], &st) < 0) {
            fprintf(stderr, "stat: %s: no such file or directory\n", argv[i]);
            status = 1;
            continue;
        }

        const char *type =
            S_ISREG(st.st_mode)  ? "regular file" :
            S_ISDIR(st.st_mode)  ? "directory"    : "unknown";

        printf("  File: %s\n", argv[i]);
        printf("  Size: %d\n", (int)st.st_size);
        printf("  Type: %s\n", type);
    }
    return status;
}
