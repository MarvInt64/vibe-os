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
 * adduser — create a new user account (modelled on Linux adduser/useradd).
 *
 * Usage: adduser [OPTIONS] <username>
 *
 *   -u, --uid UID       numeric user ID (default: next free id >= 1000)
 *   -d, --home DIR       home directory (default: /home/<username>)
 *   -s, --shell SHELL    login shell (default: /bin/sh)
 *   -c, --gecos NAME     full name / comment field
 *   -h, --help           show this help and exit
 *
 * Creating an account does four things, mirroring the Unix convention:
 *   1. append an entry to /etc/passwd  (name:x:uid:gid:gecos:home:shell)
 *   2. append an entry to /etc/shadow  (name:password — plaintext, the scheme
 *      VibeOS currently uses; see su(1))
 *   3. create the home directory
 *   4. give the home directory to the new user, mode 0700 (owner + root only)
 *
 * Only root (uid 0) may run this: it edits system databases, creates a
 * directory and changes its ownership (all privileged operations).
 */

#include <stdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

namespace {

constexpr const char *kPasswdPath = "/etc/passwd";
constexpr const char *kShadowPath = "/etc/shadow";
constexpr const char *kDefaultShell = "/bin/sh";

/* Regular (non-system) user IDs start here, as on most Linux distributions. */
constexpr int kFirstRegularUid = 1000;
constexpr int kLastRegularUid  = 60000;

constexpr int kMaxNameLen = 32;     /* keeps /home/<name> well under PATH_MAX */
constexpr int kPathCap    = 64;     /* kernel copies paths into a 64-byte buffer */

/* ---- low-level file helpers --------------------------------------------- */

/*
 * Read the entire file at 'path' into 'buf' (NUL-terminated). Returns the
 * number of bytes read, 0 if the file is empty, or -1 if it cannot be opened.
 */
ssize_t read_whole_file(const char *path, char *buf, size_t cap) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = '\0';
    return (ssize_t)n;
}

/*
 * Append 'line' to the file at 'path' (creating it if absent). The database is
 * read, a trailing newline is ensured so entries never run together, the line
 * is appended, and the whole file rewritten with fopen("w") (which truncates).
 * Returns 0 on success, -1 on failure.
 */
int append_line(const char *path, const char *line) {
    char buf[4096];
    ssize_t n = read_whole_file(path, buf, sizeof(buf));
    if (n < 0) n = 0;  /* file did not exist yet — start empty */

    /* Make sure the existing content is newline-terminated. */
    if (n > 0 && buf[n - 1] != '\n') {
        if ((size_t)n + 1 >= sizeof(buf)) return -1;
        buf[n++] = '\n';
    }

    size_t line_len = strlen(line);
    if ((size_t)n + line_len >= sizeof(buf)) return -1;  /* would overflow */
    memcpy(buf + n, line, line_len);
    n += (ssize_t)line_len;

    FILE *f = fopen(path, "w");
    if (!f) return -1;
    size_t w = fwrite(buf, 1, (size_t)n, f);
    return (fclose(f) == 0 && w == (size_t)n) ? 0 : -1;
}

/* ---- /etc/passwd queries ------------------------------------------------- */

/* Copy field 'index' (0-based, ':'-separated) of 'line' into 'out'. The line
 * must already be NUL-terminated at its newline. Returns 1 on success. */
int field(const char *line, int index, char *out, size_t cap) {
    int cur = 0;
    const char *p = line;
    const char *start = p;
    while (true) {
        if (*p == ':' || *p == '\0') {
            if (cur == index) {
                size_t len = (size_t)(p - start);
                if (len >= cap) len = cap - 1;
                memcpy(out, start, len);
                out[len] = '\0';
                return 1;
            }
            if (*p == '\0') return 0;
            cur++;
            start = p + 1;
        }
        p++;
    }
}

/* Run 'fn(line)' for each non-empty line of /etc/passwd. Stops early if fn
 * returns false. */
template <typename Fn>
void for_each_passwd_line(Fn fn) {
    char buf[4096];
    if (read_whole_file(kPasswdPath, buf, sizeof(buf)) <= 0) return;

    char *line = buf;
    while (*line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        if (*line && !fn(line)) return;
        if (!nl) break;
        line = nl + 1;
    }
}

/* True if 'name' already has an entry in /etc/passwd. */
bool user_exists(const char *name) {
    bool found = false;
    for_each_passwd_line([&](char *line) {
        char f0[kMaxNameLen + 1];
        if (field(line, 0, f0, sizeof(f0)) && strcmp(f0, name) == 0) {
            found = true;
            return false;  /* stop scanning */
        }
        return true;
    });
    return found;
}

/* Lowest unused uid in [kFirstRegularUid, kLastRegularUid). */
int next_free_uid() {
    int max_used = kFirstRegularUid - 1;
    for_each_passwd_line([&](char *line) {
        char f2[16];
        if (field(line, 2, f2, sizeof(f2))) {
            int uid = atoi(f2);
            if (uid >= kFirstRegularUid && uid < kLastRegularUid && uid > max_used)
                max_used = uid;
        }
        return true;
    });
    return max_used + 1;
}

/* True if 'uid' is already assigned to some account. */
bool uid_in_use(int uid) {
    bool used = false;
    for_each_passwd_line([&](char *line) {
        char f2[16];
        if (field(line, 2, f2, sizeof(f2)) && atoi(f2) == uid) {
            used = true;
            return false;
        }
        return true;
    });
    return used;
}

/* ---- validation ---------------------------------------------------------- */

/*
 * A valid username matches [a-z_][a-z0-9_-]* and is at most kMaxNameLen long —
 * the portable Linux rule (NAME_REGEX). This keeps it safe to embed in passwd
 * lines and in a /home/<name> path.
 */
bool valid_username(const char *name) {
    if (!name[0]) return false;
    size_t len = strlen(name);
    if (len > (size_t)kMaxNameLen) return false;

    char c = name[0];
    if (!((c >= 'a' && c <= 'z') || c == '_')) return false;
    for (size_t i = 1; i < len; ++i) {
        c = name[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

/* ---- interactive password entry ------------------------------------------ */

/*
 * Read a line from stdin into 'buf' without relying on libc line buffering.
 *
 * The terminal echoes characters (there is no TCSETA ioctl yet to disable it),
 * so the password is visible while typed — acceptable for now and noted to the
 * user. The PTY slave read is non-blocking and returns 0 when the ring buffer
 * is empty, so we spin-yield between reads, exactly like su(1) does. Backspace
 * editing is supported so typos can be corrected.
 */
void read_line(const char *prompt, char *buf, size_t cap) {
    printf("%s", prompt);
    fflush(stdout);

    size_t i = 0;
    while (i < cap - 1) {
        char c = 0;
        while (read(0, &c, 1) <= 0)
            sched_yield_();              /* wait for input without busy-spinning */
        if (c == '\n' || c == '\r') break;
        if ((c == 0x7f || c == '\b')) {  /* backspace */
            if (i > 0) i--;
            continue;
        }
        if (c >= 0x20) buf[i++] = c;
    }
    buf[i] = '\0';
    printf("\n");
}

/* ---- usage --------------------------------------------------------------- */

void usage(int code) {
    FILE *out = code ? stderr : stdout;
    fputs(
        "Usage: adduser [OPTIONS] <username>\n"
        "Create a new user account.\n\n"
        "  -u, --uid UID      numeric user ID (default: next free id >= 1000)\n"
        "  -d, --home DIR     home directory (default: /home/<username>)\n"
        "  -s, --shell SHELL  login shell (default: /bin/sh)\n"
        "  -c, --gecos NAME   full name / comment field\n"
        "  -h, --help         show this help and exit\n",
        out);
    exit(code);
}

/* Match a long or short option, consuming its value from argv when present. */
const char *opt_value(int argc, char **argv, int &i, const char *shortopt,
                      const char *longopt) {
    if (strcmp(argv[i], shortopt) == 0 || strcmp(argv[i], longopt) == 0) {
        if (i + 1 >= argc) {
            fprintf(stderr, "adduser: option %s requires an argument\n", argv[i]);
            exit(1);
        }
        return argv[++i];
    }
    return nullptr;
}

}  // namespace

int main(int argc, char **argv) {
    const char *username = nullptr;
    const char *home = nullptr;
    const char *shell = kDefaultShell;
    const char *gecos = "";
    int explicit_uid = -1;

    /* ---- parse command line --------------------------------------------- */
    for (int i = 1; i < argc; ++i) {
        const char *val;
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(0);
        } else if ((val = opt_value(argc, argv, i, "-u", "--uid"))) {
            explicit_uid = atoi(val);
            if (explicit_uid < 0) {
                fprintf(stderr, "adduser: invalid uid '%s'\n", val);
                return 1;
            }
        } else if ((val = opt_value(argc, argv, i, "-d", "--home"))) {
            home = val;
        } else if ((val = opt_value(argc, argv, i, "-s", "--shell"))) {
            shell = val;
        } else if ((val = opt_value(argc, argv, i, "-c", "--gecos"))) {
            gecos = val;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "adduser: unknown option '%s'\n", argv[i]);
            usage(1);
        } else if (!username) {
            username = argv[i];
        } else {
            fprintf(stderr, "adduser: extra operand '%s'\n", argv[i]);
            usage(1);
        }
    }

    if (!username) usage(1);

    /* ---- preconditions --------------------------------------------------- */
    if (getuid() != 0) {
        fprintf(stderr, "adduser: only root may add users\n");
        return 1;
    }
    if (!valid_username(username)) {
        fprintf(stderr,
                "adduser: invalid username '%s'\n"
                "  (use lowercase letters, digits, '_' and '-'; "
                "start with a letter or '_', max %d chars)\n",
                username, kMaxNameLen);
        return 1;
    }
    if (user_exists(username)) {
        fprintf(stderr, "adduser: user '%s' already exists\n", username);
        return 1;
    }

    /* ---- resolve uid / gid / home --------------------------------------- */
    int uid = (explicit_uid >= 0) ? explicit_uid : next_free_uid();
    if (uid >= kLastRegularUid) {
        fprintf(stderr, "adduser: no free user IDs left\n");
        return 1;
    }
    if (explicit_uid >= 0 && uid_in_use(uid)) {
        fprintf(stderr, "adduser: uid %d is already in use\n", uid);
        return 1;
    }
    int gid = uid;  /* one group per user (no separate group database yet) */

    char home_buf[kPathCap];
    if (!home) {
        if (snprintf(home_buf, sizeof(home_buf), "/home/%s", username) >= kPathCap) {
            fprintf(stderr, "adduser: home path too long\n");
            return 1;
        }
        home = home_buf;
    } else if (strlen(home) >= (size_t)kPathCap) {
        fprintf(stderr, "adduser: home path too long (max %d chars)\n", kPathCap - 1);
        return 1;
    }

    /* ---- password ------------------------------------------------------- */
    char pw1[128], pw2[128];
    read_line("New password: ", pw1, sizeof(pw1));
    read_line("Retype new password: ", pw2, sizeof(pw2));
    if (strcmp(pw1, pw2) != 0) {
        fprintf(stderr, "adduser: passwords do not match\n");
        return 1;
    }
    if (!pw1[0]) {
        fprintf(stderr, "adduser: empty password not allowed\n");
        return 1;
    }
    /* The password must not contain the ':' or newline that delimit shadow
     * records, or it would corrupt the database / allow injection. */
    if (strchr(pw1, ':') || strchr(pw1, '\n')) {
        fprintf(stderr, "adduser: password may not contain ':' or newline\n");
        return 1;
    }

    /* ---- write the account ---------------------------------------------- */
    /* /etc/passwd: name:x:uid:gid:gecos:home:shell */
    char passwd_line[256];
    snprintf(passwd_line, sizeof(passwd_line), "%s:x:%d:%d:%s:%s:%s\n",
             username, uid, gid, gecos, home, shell);
    if (append_line(kPasswdPath, passwd_line) != 0) {
        fprintf(stderr, "adduser: failed to update %s\n", kPasswdPath);
        return 1;
    }

    /* /etc/shadow: name:password */
    char shadow_line[160];
    snprintf(shadow_line, sizeof(shadow_line), "%s:%s\n", username, pw1);
    if (append_line(kShadowPath, shadow_line) != 0) {
        fprintf(stderr, "adduser: failed to update %s "
                        "(account left without a password!)\n", kShadowPath);
        return 1;
    }

    /* ---- create and secure the home directory --------------------------- */
    /* mkdir may fail harmlessly if the directory already exists; ownership and
     * mode are then still applied so the account is usable. */
    mkdir(home, 0700);
    if (chown(home, (unsigned)uid, (unsigned)gid) != 0)
        fprintf(stderr, "adduser: warning: could not chown %s\n", home);
    if (chmod(home, 0700) != 0)
        fprintf(stderr, "adduser: warning: could not chmod %s\n", home);

    /* Seed standard XDG user directories (Desktop, Documents, Downloads) so
     * the file browser's sidebar shortcuts work immediately. */
    char subdir[128];
    snprintf(subdir, sizeof(subdir), "%s/Desktop",   home); mkdir(subdir, 0700);
    snprintf(subdir, sizeof(subdir), "%s/Documents", home); mkdir(subdir, 0700);
    snprintf(subdir, sizeof(subdir), "%s/Downloads", home); mkdir(subdir, 0700);

    printf("Added user '%s' (uid=%d, gid=%d, home=%s, shell=%s).\n",
           username, uid, gid, home, shell);
    return 0;
}
