/*
 * ln — create hard or symbolic links
 *
 * Usage: ln <target> <link>
 *
 * Hard links are not supported by the VibeOS ext2 VFS layer.
 */

#include <stdio.h>

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    fputs("ln: links are not supported by the VibeOS filesystem\n", stderr);
    return 1;
}
