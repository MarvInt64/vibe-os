#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: touch <file1> <file2> ...\n");
        return 1;
    }

    int status = 0;
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_CREAT | O_WRONLY);
        if (fd < 0) {
            fprintf(stderr, "touch: %s: %s\n", argv[i], strerror(fd));
            status = 1;
        } else {
            close(fd);
        }
    }
    return status;
}
