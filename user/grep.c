#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

static int case_insensitive = 0;
static int invert = 0;
static int print_line_num = 0;
static int count_only = 0;

static int match(const char *line, const char *pat) {
    /* Simple substring search (no regex) */
    int plen = (int)strlen(pat);
    int llen = (int)strlen(line);
    for (int i = 0; i <= llen - plen; i++) {
        int ok = 1;
        for (int j = 0; j < plen; j++) {
            char lc = line[i+j], pc = pat[j];
            if (case_insensitive) {
                if (lc >= 'A' && lc <= 'Z') lc += 32;
                if (pc >= 'A' && pc <= 'Z') pc += 32;
            }
            if (lc != pc) { ok = 0; break; }
        }
        if (ok) return 1;
    }
    return 0;
}

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
            if (invert) m = !m;
            if (m) {
                hits++;
                if (!count_only) {
                    if (filename) printf("%s:", filename);
                    if (print_line_num) printf("%d:", linenum);
                    fputs(line, stdout);
                    if (line[pos-1] != '\n') putchar('\n');
                }
            }
            pos = 0;
        }
    }
    if (count_only && filename) printf("%s:%d\n", filename, hits);
    else if (count_only) printf("%d\n", hits);
    return hits;
}

int main(int argc, char *argv[]) {
    int first = 1;
    for (; first < argc && argv[first][0] == '-'; first++) {
        for (int j = 1; argv[first][j]; j++) {
            switch (argv[first][j]) {
                case 'i': case_insensitive = 1; break;
                case 'v': invert = 1; break;
                case 'n': print_line_num = 1; break;
                case 'c': count_only = 1; break;
                default:
                    fprintf(stderr, "grep: unknown option -%c\n", argv[first][j]);
                    return 2;
            }
        }
    }
    if (first >= argc) {
        fputs("Usage: grep [-ivnc] <pattern> [file...]\n", stderr);
        return 2;
    }
    const char *pat = argv[first++];
    int status = 1; /* 1=no match, 0=match found */

    if (first >= argc) {
        if (grep_fd(STDIN_FILENO, NULL, pat)) status = 0;
        return status;
    }
    int multi = (argc - first > 1);
    for (int i = first; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) { fprintf(stderr, "grep: %s: cannot open\n", argv[i]); continue; }
        if (grep_fd(fd, multi ? argv[i] : NULL, pat)) status = 0;
        close(fd);
    }
    return status;
}
