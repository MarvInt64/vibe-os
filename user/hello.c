/* hello — smallest possible app built on the VibeOS libc. Proves the new
 * runtime: crt0 supplies _start (which calls main and exits), and the standard
 * headers/functions work. Run it from the shell: `hello`. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    char *msg = malloc(64);
    int sum = 0, i;

    if (!msg) { fputs("hello: malloc failed\n", 2); return 1; }
    strcpy(msg, "VibeOS");

    for (i = 1; i <= 10; ++i) sum += i;

    printf("Hello from %s libc!\n", msg);
    printf("  sum(1..10) = %d, ptr = %p, hex = 0x%x\n", sum, (void *)msg, 255u);
    printf("  argv-less main, exit code follows.\n");

    free(msg);
    return 0;
}
