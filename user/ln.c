#include <stdio.h>

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    fprintf(stderr, "ln: links are not supported by the VFS filesystem on VibeOS.\n");
    return 1;
}
