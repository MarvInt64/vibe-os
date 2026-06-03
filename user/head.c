#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

static void head_fd(int fd, int n) {
    char buf[1];
    int lines = 0;
    while (lines < n) {
        ssize_t r = read(fd, buf, 1);
        if (r <= 0) break;
        write(STDOUT_FILENO, buf, 1);
        if (buf[0] == '\n') lines++;
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
        head_fd(STDIN_FILENO, n);
        return 0;
    }
    for (int i = first_file; i < argc; i++) {
        if (argc - first_file > 1)
            printf("==> %s <==\n", argv[i]);
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) { fprintf(stderr, "head: %s: cannot open\n", argv[i]); continue; }
        head_fd(fd, n);
        close(fd);
    }
    return 0;
}
