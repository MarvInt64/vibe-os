/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Marvin Kicha <https://github.com/MarvInt64/> */

/*
 * rm — remove files and directories
 *
 * Usage: rm [-rf] <path> [path2 ...]
 *
 * Flags:
 *   -r / -R   recursive: remove directories and their contents
 *   -f        force: suppress errors for missing paths
 *
 * Directories are removed bottom-up: children are deleted before the
 * parent so that rmdir() sees an empty directory.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

/* Forward declaration for mutual recursion through find_dir path. */
static int rm_recursive(const char *path, int force);

/*
 * Recursively remove all children of 'path', then rmdir 'path' itself.
 * Returns 0 on success, 1 on any failure.
 */
static int rm_recursive(const char *path, int force) {
    char entry[256];
    char child[512];
    int type, idx = 0, status = 0;

    while (readdir_at(path, idx++, entry, sizeof(entry), &type) > 0) {
        /* Skip the standard self and parent entries. */
        if (entry[0] == '.' &&
            (!entry[1] || (entry[1] == '.' && !entry[2])))
            continue;

        /* Build the child path: path + '/' + entry. */
        int plen = (int)strlen(path);
        int elen = (int)strlen(entry);
        if (plen + elen + 2 > (int)sizeof(child)) {
            fprintf(stderr, "rm: path too long: %s/%s\n", path, entry);
            status = 1;
            continue;
        }
        memcpy(child, path, plen);
        child[plen] = '/';
        memcpy(child + plen + 1, entry, elen + 1);

        if (type == 2) {
            /* Subdirectory — descend first. */
            if (rm_recursive(child, force) != 0)
                status = 1;
        } else {
            if (unlink(child) < 0) {
                if (!force) {
                    fprintf(stderr, "rm: %s: cannot remove\n", child);
                    status = 1;
                }
            }
        }
    }

    /* All children gone; remove the (now-empty) directory. */
    if (rmdir(path) < 0) {
        if (!force) {
            fprintf(stderr, "rm: %s: cannot remove directory\n", path);
            status = 1;
        }
    }

    return status;
}

int main(int argc, char *argv[]) {
    int recursive = 0;
    int force = 0;
    int first = 1;

    /* Parse flag arguments before the first non-flag. */
    for (; first < argc && argv[first][0] == '-'; first++) {
        for (int j = 1; argv[first][j]; j++) {
            switch (argv[first][j]) {
                case 'r': case 'R': recursive = 1; break;
                case 'f':           force     = 1; break;
                default:
                    fprintf(stderr, "rm: unknown option: -%c\n",
                            argv[first][j]);
                    return 1;
            }
        }
    }

    if (first >= argc) {
        fputs("Usage: rm [-rf] <path> [path2 ...]\n", stderr);
        return 1;
    }

    int status = 0;
    for (int i = first; i < argc; i++) {
        struct stat st;
        if (stat(argv[i], &st) < 0) {
            if (!force) {
                fprintf(stderr, "rm: %s: no such file or directory\n",
                        argv[i]);
                status = 1;
            }
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            if (!recursive) {
                fprintf(stderr, "rm: %s: is a directory (use -r)\n",
                        argv[i]);
                status = 1;
                continue;
            }
            if (rm_recursive(argv[i], force) != 0)
                status = 1;
        } else {
            if (unlink(argv[i]) < 0) {
                if (!force) {
                    fprintf(stderr, "rm: %s: cannot remove\n", argv[i]);
                    status = 1;
                }
            }
        }
    }

    return status;
}
