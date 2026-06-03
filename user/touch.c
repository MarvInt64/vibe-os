/*
 * touch — create files if they do not exist; do nothing if they do
 *
 * Usage: touch <file1> [file2 ...]
 *
 * Note: VibeOS does not support timestamp updates, so touch only
 * guarantees existence.
 */

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fputs("Usage: touch <file1> [file2 ...]\n", stderr);
        return 1;
    }

    int status = 0;
    for (int i = 1; i < argc; i++) {
        /* O_CREAT without O_TRUNC: creates the file but leaves existing
           content untouched. */
        int fd = open(argv[i], O_CREAT | O_WRONLY);
        if (fd < 0) {
            fprintf(stderr, "touch: %s: cannot create\n", argv[i]);
            status = 1;
        } else {
            close(fd);
        }
    }
    return status;
}
