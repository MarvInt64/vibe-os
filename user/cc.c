/*
 * MIT License
 *
 * Copyright (c) 2026 Marvin Kicha
 *
 * cc — a friendly front-end for the on-device tcc compiler.
 *
 * VibeOS self-hosts tcc, but invoking it directly means repeating boilerplate
 * (output naming). `cc` wraps /bin/tcc with sensible defaults so the on-device
 * edit -> compile -> run loop is ergonomic:
 *
 *     cc hello.c            # produces ./hello (source name minus .c)
 *     cc hello.c -o myprog  # explicit output
 *     cc a.c b.c -o prog    # multiple inputs
 *     cc -run hello.c       # compile to a temp file and run it immediately
 *
 * Any flags other than -run are passed straight through to tcc, so the full
 * compiler is still available (-I, -D, -c, ...).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vibeos.h>

/* Append src to dst without overflowing dst[cap]. Returns 1 if it fit. */
static int append(char *dst, size_t cap, const char *src) {
    size_t d = strlen(dst);
    size_t s = strlen(src);
    if (d + s + 1 > cap) return 0;
    memcpy(dst + d, src, s + 1);
    return 1;
}

/* Strip a trailing ".c" and any leading directory is kept (so /a/b.c -> /a/b).
 * Writes into out (cap bytes). Returns 1 on success. */
static int default_output_name(const char *src, char *out, size_t cap) {
    size_t n = strlen(src);
    if (n >= 2 && src[n - 2] == '.' && src[n - 1] == 'c') {
        n -= 2;
    }
    if (n + 1 > cap) return 0;
    memcpy(out, src, n);
    out[n] = '\0';
    return 1;
}

int main(int argc, char *argv[]) {
    char cmd[480];
    char outbuf[256];
    const char *first_src = 0;
    int have_o = 0;
    int do_run = 0;
    int i;

    if (argc < 2) {
        printf("usage: cc [-run] file.c [more.c ...] [-o out] [tcc flags]\n");
        return 1;
    }

    /* First pass: scan for -o, -run, and the first .c input. */
    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-o") == 0) {
            have_o = 1;
        } else if (strcmp(argv[i], "-run") == 0) {
            do_run = 1;
        } else if (argv[i][0] != '-') {
            size_t l = strlen(argv[i]);
            if (first_src == 0 && l >= 2 && argv[i][l - 2] == '.' && argv[i][l - 1] == 'c') {
                first_src = argv[i];
            }
        }
    }

    if (first_src == 0) {
        printf("cc: no .c input file\n");
        return 1;
    }

    /* Decide the output path. -run always compiles to a fixed temp path. */
    if (do_run) {
        strcpy(outbuf, "/tmp/cc-run.out");
    } else if (!have_o) {
        if (!default_output_name(first_src, outbuf, sizeof(outbuf))) {
            printf("cc: output name too long\n");
            return 1;
        }
    }

    /* Build the tcc argument string: pass through every arg except our own
     * -run, then append "-o <outbuf>" when we chose the output ourselves. */
    cmd[0] = '\0';
    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-run") == 0) continue;
        if (cmd[0] != '\0' && !append(cmd, sizeof(cmd), " ")) goto toolong;
        if (!append(cmd, sizeof(cmd), argv[i])) goto toolong;
    }
    if (do_run || !have_o) {
        if (!append(cmd, sizeof(cmd), " -o ")) goto toolong;
        if (!append(cmd, sizeof(cmd), outbuf)) goto toolong;
    }
    if (0) {
toolong:
        printf("cc: command line too long\n");
        return 1;
    }

    {
        int pid = vos_spawn_arg("/bin/tcc", cmd);
        if (pid < 0) {
            printf("cc: could not start /bin/tcc\n");
            return 1;
        }
        int code = vos_waitpid(pid);
        if (code != 0) {
            return code;   /* tcc already printed the error */
        }
    }

    if (do_run) {
        int pid = vos_spawn(outbuf);
        if (pid < 0) {
            printf("cc: could not run %s\n", outbuf);
            return 1;
        }
        return vos_waitpid(pid);
    }

    return 0;
}
