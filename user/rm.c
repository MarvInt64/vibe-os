#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

int main(int argc, char *argv[]) {
    int recursive = 0;
    int force = 0;
    int first_file_idx = 1;

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            for (int j = 1; argv[i][j]; j++) {
                if (argv[i][j] == 'r' || argv[i][j] == 'R') {
                    recursive = 1;
                } else if (argv[i][j] == 'f') {
                    force = 1;
                } else {
                    fprintf(stderr, "rm: invalid option -- '%c'\n", argv[i][j]);
                    return 1;
                }
            }
            first_file_idx = i + 1;
        } else {
            break;
        }
    }

    if (first_file_idx >= argc) {
        fprintf(stderr, "Usage: rm [-rf] <file1> <file2> ...\n");
        return 1;
    }

    int status = 0;
    for (int i = first_file_idx; i < argc; i++) {
        struct stat st;
        int res = stat(argv[i], &st);
        if (res < 0) {
            if (!force) {
                fprintf(stderr, "rm: %s: %s\n", argv[i], strerror(res));
                status = 1;
            }
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            if (!recursive) {
                fprintf(stderr, "rm: %s: Is a directory\n", argv[i]);
                status = 1;
                continue;
            }
            res = rmdir(argv[i]);
            if (res < 0) {
                if (!force) {
                    fprintf(stderr, "rm: cannot remove directory '%s': %s\n", argv[i], strerror(res));
                    status = 1;
                }
            }
        } else {
            res = unlink(argv[i]);
            if (res < 0) {
                if (!force) {
                    fprintf(stderr, "rm: %s: %s\n", argv[i], strerror(res));
                    status = 1;
                }
            }
        }
    }

    return status;
}
