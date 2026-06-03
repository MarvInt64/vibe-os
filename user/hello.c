#include <stdio.h>

int main(int argc, char *argv[]) {
    printf("Hello from VibeOS!\n");
    if (argc > 1) {
        printf("Arguments passed: %d\n", argc - 1);
        for (int i = 1; i < argc; i++) {
            printf("  argv[%d]: %s\n", i, argv[i]);
        }
    } else {
        printf("No arguments passed.\n");
    }
    return 0;
}