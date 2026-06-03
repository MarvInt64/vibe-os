/*
 * head — output the first N lines of a file
 *
 * Usage: head [-n<count>] [file ...]
 *
 * Default line count is 10.  With multiple files, a header banner is
 * printed before each file's output.  Reads one byte at a time so that
 * we stop exactly at the Nth newline without over-reading.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

static void head_fd(int fd, int n) {
    char c;
    int lines = 0;
    while (lines < n) {
        if (read(fd, &c, 1) <= 0) break;
        write(STDOUT_FILENO, &c, 1);
        if (c == '\n') lines++;
    }
}

int main(int argc, char *argv[]) {
    int n = 10;
    int first_file = 1;

    /* Accept -n<digits> or -n <digits> style. */
    if (argc > 1 && argv[1][0] == '-' && argv[1][1] == 'n') {
        n = atoi(argv[1] + 2);
        if (n <= 0) n = 10;
        first_file = 2;
    }

    if (first_file >= argc) {
        head_fd(STDIN_FILENO, n);
        return 0;
    }

    int multi = (argc - first_file > 1);
    for (int i = first_file; i < argc; i++) {
        if (multi) printf("==> %s <==\n", argv[i]);

        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "head: %s: cannot open\n", argv[i]);
            continue;
        }
        head_fd(fd, n);
        close(fd);
    }
    return 0;
}
