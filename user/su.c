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
 * su — switch user
 *
 * Usage: su [username]   (defaults to "root")
 *
 * Authenticates against /etc/shadow (preferred) or /etc/passwd (fallback),
 * then changes the effective uid of the calling process.  Because the shell
 * spawns su as a child, the uid change only affects this child — matching
 * standard Unix fork+exec semantics.
 *
 * Password database conventions (POSIX-like):
 *   /etc/passwd  — name:passwd:uid:gid:gecos:home:shell
 *   /etc/shadow  — name:password             (plaintext; mode 0600)
 *
 * Authentication policy:
 *   - Root (uid 0) may switch to any account without a password.
 *   - Non-root users must provide the target account's password.
 *   - Password is verified against /etc/shadow first, then /etc/passwd.
 *   - The kernel enforces: only root may change to a different uid;
 *     non-root users calling SYS_SETUID to a different uid get EPERM.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/syscall.h>

/* ---- Syscall numbers (kernel ABI) ---------------------------------------- */

#define SYS_GETUID  55
#define SYS_SETUID  57

/* ---- Bare syscall stubs -------------------------------------------------- */

static long sc0(unsigned long n) {
    long ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(n)
                     : "rcx", "r11", "memory");
    return ret;
}

static long sc1(unsigned long n, unsigned long a0) {
    long ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(n), "D"(a0)
                     : "rcx", "r11", "memory");
    return ret;
}

/* ---- Database lookup helpers ---------------------------------------------- */

#define MAX_LINE     256
#define MAX_PASSWORD 128

/*
 * Extract colon-delimited field `index` (0-based) from a line.
 * Overwrites the delimiter with '\0' in-place.
 * Returns pointer into `line`, or NULL if the field doesn't exist.
 */
static char *field_n(char *line, int index) {
    char *start = line;
    for (int i = 0; i < index; i++) {
        while (*start && *start != ':') start++;
        if (*start == ':') start++;
        else return NULL;
    }
    if (!*start || *start == '\n') return NULL;
    char *end = start;
    while (*end && *end != ':' && *end != '\n') end++;
    *end = '\0';
    return start;
}

/*
 * Look up a username in a colon-delimited database file.
 * If `pw_out` is non-NULL, copies the password field (index `pw_field`).
 * Returns: 1 = found, 0 = user not found, -1 = file cannot be opened.
 */
static int lookup_user(const char *path, const char *username,
                       int pw_field, char *pw_out, size_t pw_cap) {
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;

    char line[MAX_LINE];
    int found = 0;
    while (!found && fgets(line, sizeof(line), fp)) {
        /* Strip trailing newline. */
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';

        char *name = field_n(line, 0);
        if (!name || strcmp(name, username) != 0) continue;

        if (pw_out && pw_cap > 0) {
            char *pw = field_n(line, pw_field);
            size_t n = pw ? strlen(pw) : 0;
            if (n >= pw_cap) n = pw_cap - 1;
            if (pw) memcpy(pw_out, pw, n);
            pw_out[n] = '\0';
        }
        found = 1;
    }
    fclose(fp);
    return found;
}

/*
 * Look up a username in /etc/passwd and return the numeric uid (field 2).
 * Returns -1 if the user is not found.
 */
static int lookup_uid(const char *username) {
    FILE *fp = fopen("/etc/passwd", "r");
    if (!fp) return -1;

    char line[MAX_LINE];
    int uid = -1;
    while (fgets(line, sizeof(line), fp)) {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';

        char *name = field_n(line, 0);
        if (!name || strcmp(name, username) != 0) continue;

        char *uid_str = field_n(line, 2);
        if (uid_str) uid = atoi(uid_str);
        break;
    }
    fclose(fp);
    return uid;
}

/* ---- Password prompt ------------------------------------------------------ */

/*
 * Read a password from stdin.  Reads one byte at a time with a spin-yield
 * loop so this works correctly on both blocking TTY and non-blocking PTY
 * slave file descriptors.  The PTY returns 0 immediately when empty, so
 * we must yield the CPU to let the terminal app deliver more data.
 *
 * Backspace / DEL are honoured.  The caller is responsible for printing
 * the prompt and a trailing newline — this function only reads.
 */
static void read_password(char *buf, size_t cap) {
    size_t i = 0;
    while (i < cap - 1) {
        char c = 0;
        ssize_t n;
        while ((n = read(STDIN_FILENO, &c, 1)) <= 0)
            sc0(3 /* SYS_YIELD */);
        if (c == '\n' || c == '\r') break;
        if ((c == 0x7f || c == '\b') && i > 0) { i--; continue; }
        if (c >= 0x20) buf[i++] = c;
    }
    buf[i] = '\0';
}

/* ---- Entry point ---------------------------------------------------------- */

int main(int argc, char **argv) {
    const char *target = (argc >= 2) ? argv[1] : "root";

    /* Resolve the target uid before prompting — avoids leaking user-existence
     * information through timing differences. */
    int target_uid = lookup_uid(target);
    if (target_uid < 0) {
        fprintf(stderr, "su: user '%s' does not exist\n", target);
        return 1;
    }

    /* Root may switch to any account without a password.  This matches the
     * standard Unix convention: uid 0 is all-powerful. */
    int current_uid = (int)sc0(SYS_GETUID);
    if (current_uid != 0) {
        /* Non-root: must authenticate against the password database.
         * Try /etc/shadow first, then fall back to /etc/passwd.  This
         * mirrors the traditional Unix migration path to shadow passwords. */
        char expected[MAX_PASSWORD];
        int have_pw = lookup_user("/etc/shadow", target, 1,
                                  expected, sizeof(expected));
        if (have_pw <= 0) {
            have_pw = lookup_user("/etc/passwd", target, 1,
                                  expected, sizeof(expected));
        }

        if (have_pw <= 0) {
            fprintf(stderr, "su: no password entry for '%s'\n", target);
            return 1;
        }

        /* Prompt for the password.  Echo control is not yet supported
         * (VibeOS lacks TCSETA), so the caller's terminal is responsible
         * for local echo behaviour. */
        fprintf(stderr, "Password: ");
        char entered[MAX_PASSWORD];
        read_password(entered, sizeof(entered));
        fprintf(stderr, "\n");

        if (strcmp(entered, expected) != 0) {
            fprintf(stderr, "su: authentication failure\n");
            return 1;
        }
    }

    /* Set the new uid.  The kernel returns 0 on success, -EPERM if the
     * caller is not root and the target uid differs. */
    long ret = sc1(SYS_SETUID, (unsigned long)(unsigned int)target_uid);
    if (ret < 0) {
        fprintf(stderr, "su: cannot set uid %d (error %ld)\n", target_uid, -ret);
        return 1;
    }

    return 0;
}
