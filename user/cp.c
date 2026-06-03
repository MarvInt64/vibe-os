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
 * cp — copy a file
 *
 * Usage: cp <src> <dst>
 *
 * Copies src to dst, creating or truncating dst.  Exits 1 on any I/O
 * error; dst is left in a partial state if a write fails mid-stream.
 */

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fputs("Usage: cp <src> <dst>\n", stderr);
        return 1;
    }

    int fd_src = open(argv[1], O_RDONLY);
    if (fd_src < 0) {
        fprintf(stderr, "cp: %s: cannot open\n", argv[1]);
        return 1;
    }

    int fd_dst = open(argv[2], O_CREAT | O_WRONLY | O_TRUNC);
    if (fd_dst < 0) {
        fprintf(stderr, "cp: %s: cannot create\n", argv[2]);
        close(fd_src);
        return 1;
    }

    char buf[4096];
    ssize_t n;
    int status = 0;

    while ((n = read(fd_src, buf, sizeof(buf))) > 0) {
        if (write(fd_dst, buf, (size_t)n) < 0) {
            fprintf(stderr, "cp: %s: write error\n", argv[2]);
            status = 1;
            break;
        }
    }

    if (n < 0) {
        fprintf(stderr, "cp: %s: read error\n", argv[1]);
        status = 1;
    }

    close(fd_src);
    close(fd_dst);
    return status;
}
