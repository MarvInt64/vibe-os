/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Marvin Kicha <https://github.com/MarvInt64/> */

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
