/*
 * mkdir — create directories
 *
 * Usage: mkdir <dir> [dir2 ...]
 *
 * Creates each named directory with mode 0777 (subject to the kernel's
 * umask).  Exits 1 if any creation fails; remaining arguments are still
 * attempted.
 */

#include <stdio.h>
#include <sys/stat.h>

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fputs("Usage: mkdir <dir> [dir2 ...]\n", stderr);
        return 1;
    }

    int status = 0;
    for (int i = 1; i < argc; i++) {
        if (mkdir(argv[i], 0777) < 0) {
            fprintf(stderr, "mkdir: %s: cannot create directory\n", argv[i]);
            status = 1;
        }
    }
    return status;
}
