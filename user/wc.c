#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

static void count(int fd, const char *name, long *tl, long *tw, long *tb) {
    char buf[1024];
    ssize_t n;
    long lines = 0, words = 0, bytes = 0;
    int in_word = 0;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        bytes += n;
        for (ssize_t i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n') lines++;
            int sp = (c == ' ' || c == '\t' || c == '\n' || c == '\r');
            if (!sp && !in_word) { words++; in_word = 1; }
            else if (sp) in_word = 0;
        }
    }
    if (name)
        printf("%7ld %7ld %7ld %s\n", lines, words, bytes, name);
    else
        printf("%7ld %7ld %7ld\n", lines, words, bytes);
    *tl += lines; *tw += words; *tb += bytes;
}

int main(int argc, char *argv[]) {
    long tl = 0, tw = 0, tb = 0;
    if (argc < 2) {
        count(STDIN_FILENO, NULL, &tl, &tw, &tb);
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) { fprintf(stderr, "wc: %s: cannot open\n", argv[i]); continue; }
        count(fd, argv[i], &tl, &tw, &tb);
        close(fd);
    }
    if (argc > 2)
        printf("%7ld %7ld %7ld total\n", tl, tw, tb);
    return 0;
}
