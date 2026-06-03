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
 * Reads /etc/shadow for plaintext passwords, /etc/passwd for uid lookup.
 * On success calls SYS_SETUID to change the effective uid of this process.
 * Because the shell forks before executing su, the setuid only affects this
 * child — that is the correct Unix behaviour.
 */

#define SYS_READ    0
#define SYS_WRITE   1
#define SYS_EXIT    4
#define SYS_OPEN    7
#define SYS_CLOSE   8
#define SYS_SETUID  57

typedef unsigned long  size_t;
typedef long           ssize_t;
typedef unsigned long  uint64_t;
typedef unsigned int   uint32_t;
typedef unsigned short uint16_t;
typedef unsigned char  uint8_t;

/* ---- Bare-metal syscall stubs -------------------------------------------- */

static inline ssize_t syscall3(uint64_t n, uint64_t a0, uint64_t a1, uint64_t a2) {
    ssize_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(n), "D"(a0), "S"(a1), "d"(a2)
                     : "rcx", "r11", "memory");
    return ret;
}

static inline ssize_t syscall1(uint64_t n, uint64_t a0) {
    ssize_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(n), "D"(a0)
                     : "rcx", "r11", "memory");
    return ret;
}

/* ---- String helpers (no libc) -------------------------------------------- */

static size_t su_strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

static int su_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

/* ---- I/O helpers ---------------------------------------------------------- */

static void su_write(const char *s) {
    syscall3(SYS_WRITE, 1, (uint64_t)(size_t)s, su_strlen(s));
}

/* Read one char from stdin, blocking until available. */
static char su_readchar(void) {
    char c = 0;
    while (syscall3(SYS_READ, 0, (uint64_t)(size_t)&c, 1) <= 0)
        ;
    return c;
}

/* ---- File reading helper -------------------------------------------------- */

/*
 * Open a file by path, read its entire contents into buf, close it.
 * Returns the number of bytes read, or -1 on error.
 * SYS_READ needs a file descriptor (fd), not a path — we must SYS_OPEN first.
 */
static ssize_t read_file(const char *path, char *buf, size_t cap) {
    ssize_t fd = syscall1(SYS_OPEN, (uint64_t)(size_t)path);
    if (fd < 0) return -1;
    ssize_t n = syscall3(SYS_READ, (uint64_t)fd, (uint64_t)(size_t)buf, cap - 1);
    syscall1(SYS_CLOSE, (uint64_t)fd);
    if (n > 0) buf[n] = '\0';
    return n;
}

/* ---- /etc/passwd lookup: name -> uid ------------------------------------- */

/*
 * Scan /etc/passwd for username and return the numeric uid in field 2.
 * Returns -1 if the user is not found.
 * Format: name:x:uid:gid:gecos:home:shell
 */
static int lookup_uid(const char *username) {
    static char buf[512];
    ssize_t n = read_file("/etc/passwd", buf, sizeof(buf));
    if (n <= 0) return -1;

    char *line = buf;
    while (*line) {
        /* Field 0: username. */
        char *p = line;
        while (*p && *p != ':' && *p != '\n') p++;
        size_t name_len = (size_t)(p - line);
        size_t user_len = su_strlen(username);

        if (name_len == user_len) {
            /* Compare name manually since su_strcmp is NUL-terminated. */
            int match = 1;
            for (size_t i = 0; i < name_len; ++i) {
                if (line[i] != username[i]) { match = 0; break; }
            }
            if (match) {
                /* Skip two colon-separated fields to reach uid. */
                int colons = 0;
                while (*p && *p != '\n' && colons < 2) {
                    if (*p == ':') colons++;
                    p++;
                }
                int uid = 0;
                while (*p >= '0' && *p <= '9') {
                    uid = uid * 10 + (*p - '0');
                    p++;
                }
                return uid;
            }
        }

        while (*line && *line != '\n') line++;
        if (*line == '\n') line++;
    }
    return -1;
}

/* ---- /etc/shadow lookup: name -> plaintext password ---------------------- */

/*
 * Scan /etc/shadow for username and copy its password into pw_buf.
 * Format: name:password\n  (one entry per line, plaintext for now)
 * Returns 1 on success, 0 if user not found.
 */
static int lookup_shadow(const char *username, char *pw_buf, size_t pw_cap) {
    static char buf[512];
    ssize_t n = read_file("/etc/shadow", buf, sizeof(buf));
    if (n <= 0) return 0;

    char *line = buf;
    while (*line) {
        /* Field 0: username. */
        char *p = line;
        while (*p && *p != ':' && *p != '\n') p++;
        size_t name_len = (size_t)(p - line);
        size_t user_len = su_strlen(username);

        if (name_len == user_len) {
            int match = 1;
            for (size_t i = 0; i < name_len; ++i) {
                if (line[i] != username[i]) { match = 0; break; }
            }
            if (match && *p == ':') {
                p++;  /* skip ':' */
                size_t i = 0;
                while (*p && *p != '\n' && i < pw_cap - 1) {
                    pw_buf[i++] = *p++;
                }
                pw_buf[i] = '\0';
                return 1;
            }
        }

        while (*line && *line != '\n') line++;
        if (*line == '\n') line++;
    }
    return 0;
}

/* ---- Entry point ---------------------------------------------------------- */

int main(int argc, char **argv) {
    const char *target = "root";
    if (argc >= 2) target = argv[1];

    /* Resolve target uid before asking for a password to avoid leaking
     * that a username exists through a timing difference. */
    int target_uid = lookup_uid(target);
    if (target_uid < 0) {
        /* Don't reveal whether the user exists; just deny. */
        su_write("su: incorrect password\n");
        return 1;
    }

    /* Fetch password from shadow. */
    char expected_pw[128];
    if (!lookup_shadow(target, expected_pw, sizeof(expected_pw))) {
        /* No shadow entry means access is denied — this is intentional:
         * every account that allows su must have an explicit shadow entry. */
        su_write("su: incorrect password\n");
        return 1;
    }

    /* Prompt for password.  The TTY echoes what is typed (no way to suppress
     * it without a TCSETA ioctl, which is not yet implemented).
     * Read the whole line at once — same approach as the shell readline fix:
     * the PTY delivers a complete line when Enter is pressed, so a large
     * SYS_READ call is the only reliable way to get the full password. */
    su_write("Password: ");
    char entered[128];
    ssize_t ei = syscall3(SYS_READ, 0,
                          (uint64_t)(size_t)entered,
                          sizeof(entered) - 1);
    if (ei < 0) ei = 0;
    /* Strip trailing CR and LF so "vibeos\r\n" → "vibeos". */
    while (ei > 0 && (entered[ei-1] == '\n' || entered[ei-1] == '\r')) ei--;
    entered[ei] = '\0';
    su_write("\n");

    if (su_strcmp(entered, expected_pw) != 0) {
        su_write("su: incorrect password\n");
        return 1;
    }

    /* Password matched — switch uid. */
    syscall1(SYS_SETUID, (uint64_t)(uint32_t)target_uid);

    su_write("su: switched to ");
    su_write(target);
    su_write("\n");
    return 0;
}
