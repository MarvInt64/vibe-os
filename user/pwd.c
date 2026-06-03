#include <stdio.h>
#include <unistd.h>

int main(void) {
    char buf[256];
    if (getcwd(buf, sizeof(buf))) {
        puts(buf);
        return 0;
    }
    fputs("pwd: error\n", stderr);
    return 1;
}
