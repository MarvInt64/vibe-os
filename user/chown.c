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
 * Usage: chown <uid>[:<gid>] <file> [file2 ...]
 *
 * Both uid and gid are numeric.  The ':gid' part is optional; if omitted
 * the group ID is left unchanged (set to the same value as uid here, since
 * we don't have /etc/passwd lookups yet).
 *
 * Only root (uid 0) may change ownership of a file.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fputs("Usage: chown <uid>[:<gid>] <file> [file2 ...]\n", stderr);
        return 1;
    }

    /* Parse "uid" or "uid:gid". */
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
        uid = (unsigned int)strtol(spec,   NULL, 10);
        gid = (unsigned int)strtol(colon + 1, NULL, 10);
    } else {
        uid = (unsigned int)strtol(spec, NULL, 10);
        gid = uid;  /* no group specified: mirror uid */
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
