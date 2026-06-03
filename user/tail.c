#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#define MAX_LINES 10000
#define LINE_MAX  512

static void tail_fd(int fd, int n) {
    /* Circular buffer of line pointers */
    static char pool[MAX_LINES * LINE_MAX];
    static int  offsets[MAX_LINES];
    int head = 0, count = 0;
    char linebuf[LINE_MAX];
    int lpos = 0;
    char c;

    while (read(fd, &c, 1) == 1) {
        if (lpos < LINE_MAX - 1) linebuf[lpos++] = c;
        if (c == '\n' || lpos == LINE_MAX - 1) {
            linebuf[lpos] = '\0';
            int slot = head % MAX_LINES;
            int off  = slot * LINE_MAX;
            int k = 0;
            while (k < lpos) { pool[off + k] = linebuf[k]; k++; }
            pool[off + k] = '\0';
            offsets[slot] = off;
            head++;
            if (count < MAX_LINES) count++;
            lpos = 0;
        }
    }
    if (lpos > 0) { /* last partial line without newline */
        linebuf[lpos] = '\0';
        int slot = head % MAX_LINES;
        int off  = slot * LINE_MAX;
        int k = 0;
        while (k < lpos) { pool[off + k] = linebuf[k]; k++; }
        pool[off + k] = '\0';
        offsets[slot] = off;
        head++;
        if (count < MAX_LINES) count++;
    }

    int start = (count < n) ? 0 : count - n;
    int base  = head - count;
    for (int i = start; i < count; i++) {
        int slot = (base + i) % MAX_LINES;
        fputs(pool + offsets[slot], stdout);
    }
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
    for (int i = first_file; i < argc; i++) {
        if (argc - first_file > 1)
            printf("==> %s <==\n", argv[i]);
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) { fprintf(stderr, "tail: %s: cannot open\n", argv[i]); continue; }
        tail_fd(fd, n);
        close(fd);
    }
    return 0;
}
