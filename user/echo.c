#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

int main(int argc, char *argv[]) {
    int i;
    int redir = 0;        /* 0: none, 1: truncate (>), 2: append (>>) */
    int redir_idx = -1;
    int last_word = argc;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], ">") == 0) { redir = 1; redir_idx = i; last_word = i; break; }
        if (strcmp(argv[i], ">>") == 0) { redir = 2; redir_idx = i; last_word = i; break; }
    }

    if (!redir) {
        for (i = 1; i < argc; i++) {
            if (i > 1) printf(" ");
            printf("%s", argv[i]);
        }
        printf("\n");
        return 0;
    }

    if (redir_idx + 1 >= argc) {
        fprintf(stderr, "echo: missing filename after redirection\n");
        return 1;
    }

    const char *filename = argv[redir_idx + 1];
    
    FILE *fp = NULL;
    if (redir == 1) {
        fp = fopen(filename, "w");
    } else {
        fp = fopen(filename, "a");
    }

    if (!fp) {
        fprintf(stderr, "echo: could not open %s\n", filename);
        return 1;
    }

    for (i = 1; i < last_word; i++) {
        if (i > 1) fprintf(fp, " ");
        fprintf(fp, "%s", argv[i]);
    }
    fprintf(fp, "\n");
    fclose(fp);

    return 0;
}
