/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Marvin Kicha <https://github.com/MarvInt64/> */

/*
 * pwd — print the current working directory
 *
 * Usage: pwd
 *
 * Calls getcwd() and prints the result followed by a newline.
 * Exits 1 if the kernel cannot determine the working directory.
 */

#include <stdio.h>
#include <unistd.h>

int main(void) {
    char buf[256];
    if (getcwd(buf, sizeof(buf))) {
        puts(buf);
        return 0;
    }
    fputs("pwd: cannot get working directory\n", stderr);
    return 1;
}
