/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Marvin Kicha <https://github.com/MarvInt64/> */

/*
 * stat — display file or directory metadata
 *
 * Usage: stat <path> [path2 ...]
 *
 * Prints size and type for each path.  Exits 1 if any path cannot be
 * stat'd; processes remaining paths regardless.
 */

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fputs("Usage: stat <path> [path2 ...]\n", stderr);
        return 1;
    }

    int status = 0;
    for (int i = 1; i < argc; i++) {
        struct stat st;
        if (stat(argv[i], &st) < 0) {
            fprintf(stderr, "stat: %s: no such file or directory\n", argv[i]);
            status = 1;
            continue;
        }

        const char *type =
            S_ISREG(st.st_mode)  ? "regular file" :
            S_ISDIR(st.st_mode)  ? "directory"    : "unknown";

        printf("  File: %s\n", argv[i]);
        printf("  Size: %d\n", (int)st.st_size);
        printf("  Type: %s\n", type);
    }
    return status;
}
