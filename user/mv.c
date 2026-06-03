#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: mv <src> <dst>\n");
        return 1;
    }

    int fd_src = open(argv[1], O_RDONLY);
    if (fd_src < 0) {
        fprintf(stderr, "mv: cannot open '%s': %s\n", argv[1], strerror(fd_src));
        return 1;
    }

    int fd_dst = open(argv[2], O_CREAT | O_WRONLY | O_TRUNC);
    if (fd_dst < 0) {
        fprintf(stderr, "mv: cannot create '%s': %s\n", argv[2], strerror(fd_dst));
        close(fd_src);
        return 1;
    }

    char buf[4096];
    ssize_t n;
    int status = 0;
    while ((n = read(fd_src, buf, sizeof(buf))) > 0) {
        ssize_t written = write(fd_dst, buf, (size_t)n);
        if (written < 0) {
            fprintf(stderr, "mv: write error on '%s': %s\n", argv[2], strerror((int)written));
            status = 1;
            break;
        }
    }

    if (n < 0) {
        fprintf(stderr, "mv: read error on '%s': %s\n", argv[1], strerror((int)n));
        status = 1;
    }

    close(fd_src);
    close(fd_dst);

    if (status == 0) {
        int res = unlink(argv[1]);
        if (res < 0) {
            fprintf(stderr, "mv: cannot remove original '%s': %s\n", argv[1], strerror(res));
            status = 1;
        }
    }

    return status;
}
