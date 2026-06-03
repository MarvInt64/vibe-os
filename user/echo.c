/*
 * MIT License
 *
 * Copyright (c) 2026 Marvin Kicha
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/*
 * echo — print arguments to stdout, with optional file redirection
 *
 * Usage: echo [args...] [> file | >> file]
 *
 * Supports shell-style redirection tokens '>' (truncate) and '>>' (append)
 * as part of the argument list, since the VibeOS shell passes them as argv.
 */

#include <stdio.h>
#include <string.h>

int main(int argc, char *argv[]) {
    /* Scan for a redirection token; record its position. */
    int redir = 0;       /* 1 = truncate, 2 = append */
    int redir_idx = -1;
    int last_word = argc;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], ">") == 0) {
            redir = 1; redir_idx = i; last_word = i; break;
        }
        if (strcmp(argv[i], ">>") == 0) {
            redir = 2; redir_idx = i; last_word = i; break;
        }
    }

    if (!redir) {
        /* Plain echo: print all args separated by spaces. */
        for (int i = 1; i < argc; i++) {
            if (i > 1) putchar(' ');
            fputs(argv[i], stdout);
        }
        putchar('\n');
        return 0;
    }

    if (redir_idx + 1 >= argc) {
        fputs("echo: missing filename after redirection\n", stderr);
        return 1;
    }

    const char *filename = argv[redir_idx + 1];
    FILE *fp = fopen(filename, (redir == 1) ? "w" : "a");
    if (!fp) {
        fprintf(stderr, "echo: %s: cannot open\n", filename);
        return 1;
    }

    for (int i = 1; i < last_word; i++) {
        if (i > 1) fputc(' ', fp);
        fputs(argv[i], fp);
    }
    fputc('\n', fp);
    fclose(fp);

    return 0;
}
