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
