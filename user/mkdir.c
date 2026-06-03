#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: mkdir <dir>\n");
        return 1;
    }

    int status = 0;
    for (int i = 1; i < argc; i++) {
        int res = mkdir(argv[i], 0777);
        if (res < 0) {
            fprintf(stderr, "mkdir: %s: %s\n", argv[i], strerror(res));
            status = 1;
        }
    }
    return status;
}
