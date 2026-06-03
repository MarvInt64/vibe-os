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
 * id — print user and group identity
 *
 * Usage: id
 *
 * Prints: uid=N(name) gid=N(name)
 * where N is the numeric ID and name is resolved from /etc/passwd.
 * Falls back to the numeric ID alone when /etc/passwd is absent.
 *
 * Example output:
 *   uid=0(root) gid=0(root)
 *   uid=1000(marvin) gid=1000(marvin)
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

/* Read /etc/passwd and find the name for a given numeric uid.
 * Writes the name into 'out' and returns 1 on success, 0 if not found. */
static int name_for_uid(unsigned int uid, char *out, int cap) {
    int fd = open("/etc/passwd", O_RDONLY);
    if (fd < 0) return 0;

    char buf[512];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = '\0';

    char *line = buf;
    while (*line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        /* passwd line: name : pwd : uid : gid : ... */
        char *f0 = line;
        char *p  = strchr(f0, ':'); if (!p) goto next;
        *p = '\0'; p++;
        p = strchr(p, ':'); if (!p) goto next;  /* skip pwd field */
        p++;
        /* p now points at the uid field. */
        char *end = strchr(p, ':'); if (end) *end = '\0';

        unsigned int entry_uid = 0;
        for (char *d = p; *d >= '0' && *d <= '9'; d++)
            entry_uid = entry_uid * 10 + (unsigned)(*d - '0');

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
    unsigned int gid = (unsigned int)getgid();
    char uname[64] = {0};
    char gname[64] = {0};

    int has_uname = name_for_uid(uid, uname, sizeof(uname));
    /* For now group names also come from passwd (no /etc/group yet). */
    int has_gname = name_for_uid(gid, gname, sizeof(gname));

    if (has_uname)
        printf("uid=%u(%s) ", uid, uname);
    else
        printf("uid=%u ", uid);

    if (has_gname)
        printf("gid=%u(%s)\n", gid, gname);
    else
        printf("gid=%u\n", gid);

    return 0;
}
