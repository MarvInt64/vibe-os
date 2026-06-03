/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Marvin Kicha <https://github.com/MarvInt64/> */

/*
 * grep — search for a pattern in files
 *
 * Usage: grep [-ivnc] <pattern> [file ...]
 *
 * Flags:
 *   -i   case-insensitive matching
 *   -v   invert: print lines that do NOT match
 *   -n   prefix each output line with its line number
 *   -c   count matching lines only (no line output)
 *
 * Pattern matching is a simple substring search (no regex).
 * Exit status: 0 if any match found, 1 if none, 2 on usage error.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

static int flag_i = 0;  /* case-insensitive */
static int flag_v = 0;  /* invert match */
static int flag_n = 0;  /* print line numbers */
static int flag_c = 0;  /* count only */

/* Case-fold a single ASCII character when -i is active. */
static char fold(char c) {
    if (flag_i && c >= 'A' && c <= 'Z') return c + 32;
    return c;
}

/* Returns 1 if 'pat' occurs as a substring of 'line'. */
static int match(const char *line, const char *pat) {
    int plen = (int)strlen(pat);
    int llen = (int)strlen(line);
    for (int i = 0; i <= llen - plen; i++) {
        int ok = 1;
        for (int j = 0; j < plen; j++) {
            if (fold(line[i + j]) != fold(pat[j])) { ok = 0; break; }
        }
        if (ok) return 1;
    }
    return 0;
}

/*
 * Scan 'fd' line by line against 'pat'.
 * 'filename' is printed as a prefix when non-NULL (multi-file mode).
 * Returns the number of matching lines.
 */
static int grep_fd(int fd, const char *filename, const char *pat) {
    char line[1024];
    int pos = 0, linenum = 0, hits = 0;
    char c;

    while (read(fd, &c, 1) == 1) {
        if (pos < (int)sizeof(line) - 1) line[pos++] = c;
        if (c == '\n' || pos == (int)sizeof(line) - 1) {
            line[pos] = '\0';
            linenum++;

            int m = match(line, pat);
            if (flag_v) m = !m;

            if (m) {
                hits++;
                if (!flag_c) {
                    if (filename)  printf("%s:", filename);
                    if (flag_n)    printf("%d:", linenum);
                    fputs(line, stdout);
                    /* Ensure output ends with a newline even for truncated lines. */
                    if (pos == 0 || line[pos - 1] != '\n') putchar('\n');
                }
            }
            pos = 0;
        }
    }

    if (flag_c) {
        if (filename) printf("%s:%d\n", filename, hits);
        else          printf("%d\n", hits);
    }

    return hits;
}

int main(int argc, char *argv[]) {
    int first = 1;

    for (; first < argc && argv[first][0] == '-'; first++) {
        for (int j = 1; argv[first][j]; j++) {
            switch (argv[first][j]) {
                case 'i': flag_i = 1; break;
                case 'v': flag_v = 1; break;
                case 'n': flag_n = 1; break;
                case 'c': flag_c = 1; break;
                default:
                    fprintf(stderr, "grep: unknown option: -%c\n",
                            argv[first][j]);
                    return 2;
            }
        }
    }

    if (first >= argc) {
        fputs("Usage: grep [-ivnc] <pattern> [file ...]\n", stderr);
        return 2;
    }

    const char *pat = argv[first++];
    int status = 1;  /* 1 = no match found */
    int multi  = (argc - first > 1);

    if (first >= argc) {
        /* No file arguments: read stdin. */
        if (grep_fd(STDIN_FILENO, NULL, pat)) status = 0;
        return status;
    }

    for (int i = first; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "grep: %s: cannot open\n", argv[i]);
            continue;
        }
        if (grep_fd(fd, multi ? argv[i] : NULL, pat)) status = 0;
        close(fd);
    }
    return status;
}
