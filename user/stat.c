#include <stdio.h>
#include <sys/stat.h>
#include <string.h>

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: stat <path>\n");
        return 1;
    }

    int status = 0;
    for (int i = 1; i < argc; i++) {
        struct stat st;
        int res = stat(argv[i], &st);
        if (res < 0) {
            fprintf(stderr, "stat: %s: %s\n", argv[i], strerror(res));
            status = 1;
            continue;
        }
        printf("  File: %s\n", argv[i]);
        printf("  Size: %d\n", (int)st.st_size);
        printf("  Type: ");
        if (S_ISREG(st.st_mode)) {
            printf("Regular File\n");
        } else if (S_ISDIR(st.st_mode)) {
            printf("Directory\n");
        } else {
            printf("Unknown\n");
        }
    }
    return status;
}
