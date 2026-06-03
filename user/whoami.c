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
 * whoami — print the username of the current user
 *
 * Usage: whoami
 *
 * Reads the uid from the kernel via getuid(), then looks up the matching
 * entry in /etc/passwd (format: name:x:uid:gid:comment:home:shell).
 * Falls back to printing "root" if uid is 0 and /etc/passwd is missing,
 * or the raw uid number if no entry matches.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

/*
 * Look up 'uid' in /etc/passwd and copy the username into 'out'.
 * Returns 1 if found, 0 if not found or file missing.
 *
 * /etc/passwd line format: username:password:uid:gid:comment:home:shell
 * We only parse the first (username) and third (uid) fields.
 */
static int lookup_user(unsigned int uid, char *out, int cap) {
    int fd = open("/etc/passwd", O_RDONLY);
    if (fd < 0) return 0;

    char buf[512];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = '\0';

    /* Walk each newline-terminated line. */
    char *line = buf;
    while (*line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        /* Field 0: username (up to first ':') */
        char *f0 = line;
        char *f1 = strchr(f0, ':'); if (!f1) goto next;
        *f1 = '\0'; f1++;
        /* Field 1: password placeholder */
        char *f2 = strchr(f1, ':'); if (!f2) goto next;
        *f2 = '\0'; f2++;
        /* Field 2: uid string */
        char *f3 = strchr(f2, ':'); if (f3) *f3 = '\0';

        unsigned int entry_uid = 0;
        for (char *p = f2; *p >= '0' && *p <= '9'; p++)
            entry_uid = entry_uid * 10 + (unsigned)(*p - '0');

        if (entry_uid == uid) {
            int k = 0;
            while (f0[k] && k < cap - 1) { out[k] = f0[k]; k++; }
            out[k] = '\0';
            return 1;
        }

    next:
        if (!nl) break;
        line = nl + 1;
    }
    return 0;
}

int main(void) {
    unsigned int uid = (unsigned int)getuid();
    char name[64];

    if (lookup_user(uid, name, sizeof(name))) {
        puts(name);
    } else if (uid == 0) {
        puts("root");
    } else {
        printf("uid=%u\n", uid);
    }
    return 0;
}
