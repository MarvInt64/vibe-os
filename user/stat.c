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
 * stat — display file metadata in a human-readable format
 *
 * Usage: stat <path> [path2 ...]
 *
 * Output per entry:
 *   File:  <path>
 *   Size:  <bytes>
 *   Type:  regular file | directory | unknown
 *   Mode:  <octal>  (<rwxrwxrwx string>)
 *   Owner: uid=<N>  gid=<N>
 */

#include <stdio.h>
#include <sys/stat.h>

/* Convert mode bits to a 10-char "drwxr-xr-x" string (null-terminated). */
static void mode_str(unsigned int kind_is_dir, unsigned int mode, char out[11]) {
    out[0] = kind_is_dir ? 'd' : '-';
    out[1] = (mode & 0400u) ? 'r' : '-';
    out[2] = (mode & 0200u) ? 'w' : '-';
    out[3] = (mode & 0100u) ? 'x' : '-';
    out[4] = (mode & 0040u) ? 'r' : '-';
    out[5] = (mode & 0020u) ? 'w' : '-';
    out[6] = (mode & 0010u) ? 'x' : '-';
    out[7] = (mode & 0004u) ? 'r' : '-';
    out[8] = (mode & 0002u) ? 'w' : '-';
    out[9] = (mode & 0001u) ? 'x' : '-';
    out[10] = '\0';
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fputs("Usage: stat <path> [path2 ...]\n", stderr);
        return 1;
    }

    int status = 0;
    for (int i = 1; i < argc; i++) {
        struct stat st;
        if (stat(argv[i], &st) < 0) {
            fprintf(stderr, "stat: %s: no such file or directory\n", argv[i]);
            status = 1;
            continue;
        }

        int is_dir = S_ISDIR(st.st_mode);
        const char *type = is_dir          ? "directory"    :
                           S_ISREG(st.st_mode) ? "regular file" : "unknown";

        char mstr[11];
        mode_str((unsigned int)is_dir, st.st_mode & 0777u, mstr);

        printf("  File:  %s\n",        argv[i]);
        printf("  Size:  %d\n",        (int)st.st_size);
        printf("  Type:  %s\n",        type);
        printf("  Mode:  %04o  (%s)\n",(unsigned)(st.st_mode & 0777u), mstr);
        printf("  Owner: uid=%-4u gid=%u\n",
               (unsigned)st.st_uid, (unsigned)st.st_gid);
    }
    return status;
}
