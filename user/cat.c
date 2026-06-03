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
 * cat — concatenate files and print to stdout
 *
 * Usage: cat [file ...]
 *
 * With no arguments (or '-' as a filename), reads from stdin.
 * Multiple files are concatenated in order.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

static void cat_fd(int fd) {
    char buf[1024];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        write(STDOUT_FILENO, buf, (size_t)n);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        cat_fd(STDIN_FILENO);
        return 0;
    }

    int status = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-") == 0) {
            cat_fd(STDIN_FILENO);
            continue;
        }

        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "cat: %s: cannot open\n", argv[i]);
            status = 1;
            continue;
        }
        cat_fd(fd);
        close(fd);
    }
    return status;
}
