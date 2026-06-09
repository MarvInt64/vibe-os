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
 * chown — change file ownership
 *
 * Usage: chown <owner>[:<group>] <file> [file2 ...]
 *
 * Both owner and group accept symbolic names (looked up in /etc/passwd
 * and /etc/group) or numeric IDs.  Examples:
 *   chown root:root /bin/doom
 *   chown 1000:1000 /home/marvin/file
 *   chown marvin: /home/marvin/file    (group unchanged)
 *
 * Only root (uid 0) may change ownership of a file.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

/* ---- /etc/passwd resolver ---------------------------------------------- */

/*
 * Look up a user or group name in /etc/passwd and return its numeric ID.
 * Format: name:x:uid:gid:gecos:home:shell
 * Returns the uid/gid on success, or (unsigned int)-1 if not found.
 * If `want_gid` is non-zero, returns the gid field (index 3) instead of uid.
 */
static unsigned int passwd_lookup(const char *name, int want_gid) {
    char buf[4096];
    int fd = open("/etc/passwd", O_RDONLY);
    if (fd < 0) return (unsigned int)-1;

    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return (unsigned int)-1;
    buf[n] = '\0';

    size_t name_len = strlen(name);
    char *line = buf;
    while (*line) {
        /* Find end of this line. */
        char *nl = line;
        while (*nl && *nl != '\n') nl++;

        /* Compare field 0 (username). */
        if ((size_t)(nl - line) >= name_len && line[name_len] == ':' &&
            memcmp(line, name, name_len) == 0) {
            /* Walk to the desired field (uid=field 2, gid=field 3). */
            char *p = line;
            int field = 0;
            int target = want_gid ? 3 : 2;
            while (field < target && p < nl) {
                if (*p == ':') field++;
                p++;
            }
            if (field == target && p < nl) {
                unsigned int id = 0;
                while (p < nl && *p >= '0' && *p <= '9') {
                    id = id * 10 + (unsigned int)(*p - '0');
                    p++;
                }
                return id;
            }
            return (unsigned int)-1;
        }

        /* Advance to next line. */
        line = nl;
        if (*line == '\n') line++;
    }
    return (unsigned int)-1;
}

/*
 * Resolve a user/group spec to a numeric ID.
 * Tries numeric parse first; if the string is not purely numeric,
 * falls back to /etc/passwd.
 */
static unsigned int resolve_id(const char *spec, int want_gid) {
    /* Empty spec → leave unchanged (caller must detect). */
    if (!spec || !*spec) return (unsigned int)-1;

    /* Try numeric parse. */
    char *end;
    unsigned long v = strtoul(spec, &end, 10);
    if (*end == '\0' && end != spec) return (unsigned int)v;

    /* Fall back to /etc/passwd. */
    return passwd_lookup(spec, want_gid);
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fputs("Usage: chown <owner>[:<group>] <file> [file2 ...]\n", stderr);
        return 1;
    }

    /* Parse "owner" or "owner:group".  Copy to a mutable buffer. */
    char spec[64];
    int n = (int)strlen(argv[1]);
    if (n >= (int)sizeof(spec)) {
        fputs("chown: owner spec too long\n", stderr);
        return 1;
    }
    memcpy(spec, argv[1], (size_t)n + 1);

    char *colon = strchr(spec, ':');
    unsigned int uid, gid;

    if (colon) {
        *colon = '\0';
        uid = resolve_id(spec, 0);
        if (uid == (unsigned int)-1) {
            fprintf(stderr, "chown: unknown user '%s'\n", spec);
            return 1;
        }
        char *group_name = colon + 1;
        if (*group_name) {
            gid = resolve_id(group_name, 1);
            if (gid == (unsigned int)-1) {
                fprintf(stderr, "chown: unknown group '%s'\n", group_name);
                return 1;
            }
        } else {
            /* "user:" with empty group → leave group unchanged.
             * We use uid as gid fallback. */
            gid = uid;
        }
    } else {
        uid = resolve_id(spec, 0);
        if (uid == (unsigned int)-1) {
            fprintf(stderr, "chown: unknown user '%s'\n", spec);
            return 1;
        }
        gid = uid;   /* mirror uid when no group given */
    }

    int status = 0;
    for (int i = 2; i < argc; i++) {
        if (chown(argv[i], uid, gid) < 0) {
            fprintf(stderr, "chown: %s: failed (not root or not found)\n", argv[i]);
            status = 1;
        }
    }
    return status;
}
