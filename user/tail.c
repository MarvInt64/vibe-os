/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Marvin Kicha <https://github.com/MarvInt64/> */

/*
 * tail — output the last N lines of a file
 *
 * Usage: tail [-n<count>] [file ...]
 *
 * Default line count is 10.  Uses a circular buffer of 512 lines
 * (256 chars each, 128 KB total) — large enough for practical use
 * without exhausting the userspace heap.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#define MAX_LINES 512
#define LINE_MAX  256

static void tail_fd(int fd, int n) {
    /* Ring buffer: each slot holds one line (possibly truncated). */
    static char pool[MAX_LINES][LINE_MAX];
    char linebuf[LINE_MAX];
    int lpos = 0;
    int head = 0, total = 0;
    char c;

    while (read(fd, &c, 1) == 1) {
        if (lpos < LINE_MAX - 1) linebuf[lpos++] = c;
        if (c == '\n' || lpos == LINE_MAX - 1) {
            linebuf[lpos] = '\0';
            memcpy(pool[head % MAX_LINES], linebuf, (size_t)lpos + 1);
            head++;
            if (total < MAX_LINES) total++;
            lpos = 0;
        }
    }

    /* Handle a final partial line that had no trailing newline. */
    if (lpos > 0) {
        linebuf[lpos] = '\0';
        memcpy(pool[head % MAX_LINES], linebuf, (size_t)lpos + 1);
        head++;
        if (total < MAX_LINES) total++;
    }

    int start = (total < n) ? 0 : total - n;
    int base  = head - total;
    for (int i = start; i < total; i++)
        fputs(pool[(base + i) % MAX_LINES], stdout);
}

int main(int argc, char *argv[]) {
    int n = 10;
    int first_file = 1;

    if (argc > 1 && argv[1][0] == '-' && argv[1][1] == 'n') {
        n = atoi(argv[1] + 2);
        if (n <= 0) n = 10;
        first_file = 2;
    }

    if (first_file >= argc) {
        tail_fd(STDIN_FILENO, n);
        return 0;
    }

    int multi = (argc - first_file > 1);
    for (int i = first_file; i < argc; i++) {
        if (multi) printf("==> %s <==\n", argv[i]);

        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "tail: %s: cannot open\n", argv[i]);
            continue;
        }
        tail_fd(fd, n);
        close(fd);
    }
    return 0;
}
