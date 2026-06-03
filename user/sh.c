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

#define SYS_READ 0
#define SYS_WRITE 1
#define SYS_IOCTL 2
#define SYS_YIELD 3
#define SYS_EXIT 4
#define SYS_PROCESS_SPAWN 5
#define SYS_WAITPID 6
#define SYS_OPEN     7
#define SYS_CLOSE    8
#define SYS_STAT 9
#define SYS_READDIR 10
#define SYS_CHDIR 11
#define SYS_GETCWD 12
#define SYS_UNLINK 13
#define SYS_CREAT 14
#define SYS_WRITE_FILE 16
#define SYS_TIMER_SLEEP 20
#define SYS_WINDOWMGR_START 21
#define SYS_NET_INFO 22
#define SYS_NET_PING 23
#define SYS_NET_RESOLVE 24
#define SYS_NET_HTTP_GET 25
#define SYS_JOURNAL_READ 32
#define SYS_MKDIR 26
#define SYS_DISPLAY_MODE 27
#define SYS_SYSTEM_INFO 39
#define SYS_GETUID  55

typedef unsigned long size_t;
typedef long ssize_t;
typedef unsigned long uint64_t;
typedef unsigned int uint32_t;
typedef unsigned short uint16_t;
typedef unsigned char uint8_t;

/* Must match struct net_info in kernel/include/net.h */
struct net_info {
    uint8_t up;
    uint8_t mac[6];
    uint32_t ip;
    uint32_t netmask;
    uint32_t gateway;
};

struct system_info_snapshot {
    uint64_t uptime_ticks;
    uint32_t timer_hz;
    uint32_t process_count;
    uint32_t process_max;
    uint32_t app_window_max;
    uint64_t heap_used_bytes;
    uint64_t heap_total_bytes;
    char version[16];
    char build[32];
};

static inline ssize_t syscall4(uint64_t n, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    ssize_t ret;
    __asm__ volatile(
        "mov %5, %%r10;"
        "int $0x80"
        : "=a"(ret)
        : "a"(n), "D"(a0), "S"(a1), "d"(a2), "r"(a3)
        : "rcx", "r8", "r9", "r10", "r11", "memory"
    );
    return ret;
}

static inline ssize_t syscall3(uint64_t n, uint64_t a0, uint64_t a1, uint64_t a2) {
    ssize_t ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(n), "D"(a0), "S"(a1), "d"(a2) : "rcx", "r11", "memory");
    return ret;
}

static inline ssize_t syscall2(uint64_t n, uint64_t a0, uint64_t a1) {
    ssize_t ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(n), "D"(a0), "S"(a1) : "rcx", "r11", "memory");
    return ret;
}

static inline ssize_t syscall1(uint64_t n, uint64_t a0) {
    ssize_t ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(n), "D"(a0) : "rcx", "r11", "memory");
    return ret;
}

static inline void yield(void) {
    __asm__ volatile("int $0x80" : : "a"((uint64_t)SYS_YIELD) : "rcx", "r11", "memory");
}

static size_t strlen(const char *s) {
    size_t len = 0;
    while (s[len]) len++;
    return len;
}

static int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static int strncmp(const char *a, const char *b, size_t n) {
    while (n-- && *a && *a == *b) { a++; b++; }
    if (!n) return 0;
    return (unsigned char)*a - (unsigned char)*b;
}

static char *strcat(char *dest, const char *src) {
  char *ret = dest;
  while (*dest) dest++;
  while ((*dest++ = *src++));
  return ret;
}

static char *strcpy(char *dst, const char *src) {
    char *ret = dst;
    while ((*dst++ = *src++));
    return ret;
}

static void write_stdout(const char *s, size_t len) {
    syscall3(SYS_WRITE, 1, (uint64_t)(size_t)s, len);
}

static void write_str(const char *s) {
    write_stdout(s, strlen(s));
}

static void write_line(const char *s) {
    write_str(s);
    write_str("\n");
}

static void write_char(char c) {
    write_stdout(&c, 1);
}

static int chdir(const char *path) {
    return (int)syscall1(SYS_CHDIR, (uint64_t)(size_t)path);
}

static int getcwd(char *buf, size_t size) {
  return (int)syscall2(SYS_GETCWD, (uint64_t)(size_t)buf, size);
}

static int readdir_entry(const char *path, uint32_t index, char *name, size_t name_size) {
    return (int)syscall4(SYS_READDIR, (uint64_t)(size_t)path, (uint64_t)index, (uint64_t)(size_t)name, name_size);
}

static int spawn(const char *name) {
    return (int)syscall1(SYS_PROCESS_SPAWN, (uint64_t)(size_t)name);
}

static int spawn_with_arg(const char *path, const char *arg) {
    return (int)syscall2(SYS_PROCESS_SPAWN, (uint64_t)(size_t)path, (uint64_t)(size_t)arg);
}

static int waitpid_ret(int pid) {
    return (int)syscall2(SYS_WAITPID, (uint64_t)pid, 0);
}

#define MAX_ARGS 8
#define MAX_ARG_LEN 64
#define MAX_PATH 256
/* Expand input cap to fit history lines and variable-expanded strings. */
#define MAX_INPUT 512
#define LINE_CAP  512

/* ---- History ring buffer ------------------------------------------------- */

#define HIST_CAP 100

static char g_hist[HIST_CAP][LINE_CAP];
static int  g_hist_head  = 0;  /* index of next slot to write */
static int  g_hist_total = 0;  /* total entries ever added */

/*
 * Add a line to history, skipping empty strings and exact duplicates of the
 * most recent entry so the history doesn't fill with repeated commands.
 */
static void hist_add(const char *line) {
    if (!line || !line[0]) return;
    /* Deduplicate against the last entry. */
    if (g_hist_total > 0) {
        int last = (g_hist_head - 1 + HIST_CAP) % HIST_CAP;
        if (strcmp(g_hist[last], line) == 0) return;
    }
    int slot = g_hist_head % HIST_CAP;
    int i;
    for (i = 0; line[i] && i < LINE_CAP - 1; ++i)
        g_hist[slot][i] = line[i];
    g_hist[slot][i] = '\0';
    g_hist_head = (g_hist_head + 1) % HIST_CAP;
    g_hist_total++;
}

/*
 * Retrieve a history entry by recency. back=1 returns the most recent entry.
 * Returns NULL if out of range.
 */
static const char *hist_get(int back) {
    if (back < 1 || back > g_hist_total || back > HIST_CAP) return 0;
    int idx = (g_hist_head - back + HIST_CAP * 2) % HIST_CAP;
    return g_hist[idx];
}

/* ---- Shell state --------------------------------------------------------- */

static char g_cwd[MAX_PATH] = "/";
static char g_input[MAX_INPUT];
static char g_expanded[MAX_INPUT];  /* variable-expanded copy of g_input */
static char g_args[MAX_ARGS][MAX_ARG_LEN];
static int  g_argc;
static char g_full_path[MAX_PATH];
static char g_io_buf[1024];
static char g_ping_arg_buf[256];
static int  g_last_exit = 0;  /* exit code of the last foreground command */

/* ---- Username from /etc/passwd ------------------------------------------ */

/*
 * Parse /etc/passwd (colon-separated) to find the username for a given uid.
 * Format: name:x:uid:gid:gecos:home:shell
 * Falls back to "root" for uid 0 or "user" for others when the file is absent.
 */
static void get_username(uint32_t uid, char *buf, size_t cap) {
    /* Open /etc/passwd using a raw read — no libc here. */
    static char passwd_buf[512];
    ssize_t n = syscall3(SYS_READ,
                         (uint64_t)(size_t)"/etc/passwd",
                         (uint64_t)(size_t)passwd_buf,
                         sizeof(passwd_buf) - 1);
    if (n <= 0) {
        /* File unreadable; use sensible defaults. */
        const char *def = (uid == 0) ? "root" : "user";
        size_t i;
        for (i = 0; def[i] && i < cap - 1; ++i) buf[i] = def[i];
        buf[i] = '\0';
        return;
    }
    passwd_buf[n] = '\0';

    /* Walk each newline-terminated line. */
    char *line = passwd_buf;
    while (*line) {
        /* field 0: name */
        char *p = line;
        while (*p && *p != ':' && *p != '\n') p++;
        size_t name_len = (size_t)(p - line);

        /* Skip to field 2 (uid), which is after two colons. */
        int colons = 0;
        while (*p && *p != '\n' && colons < 2) {
            if (*p == ':') colons++;
            p++;
        }
        /* Parse the uid number in field 2. */
        uint32_t entry_uid = 0;
        while (*p >= '0' && *p <= '9') {
            entry_uid = entry_uid * 10 + (uint32_t)(*p - '0');
            p++;
        }
        if (entry_uid == uid) {
            if (name_len >= cap) name_len = cap - 1;
            size_t i;
            for (i = 0; i < name_len; ++i) buf[i] = line[i];
            buf[name_len] = '\0';
            return;
        }
        /* Advance to the next line. */
        while (*line && *line != '\n') line++;
        if (*line == '\n') line++;
    }
    /* No match — fall back. */
    const char *def = (uid == 0) ? "root" : "user";
    size_t i;
    for (i = 0; def[i] && i < cap - 1; ++i) buf[i] = def[i];
    buf[i] = '\0';
}

/* ---- Variable expansion -------------------------------------------------- */

/*
 * Copy src to dst, replacing shell variables inline.
 * We handle $?, $HOME, $PATH, $USER, $USERNAME so scripts and interactive
 * users can reference them without a real environment table.
 */
static void expand_vars(const char *src, char *dst, size_t cap,
                        int last_exit, uint32_t uid, const char *username) {
    size_t di = 0;
    size_t si = 0;

    while (src[si] && di < cap - 1) {
        if (src[si] != '$') {
            dst[di++] = src[si++];
            continue;
        }
        /* Peek at what follows '$'. */
        si++;  /* skip '$' */
        const char *insert = 0;
        char num_buf[12];

        if (src[si] == '?') {
            /* Last exit code — convert to decimal string. */
            int v = last_exit;
            int neg = (v < 0);
            if (neg) v = -v;
            int i = 0;
            if (v == 0) { num_buf[i++] = '0'; }
            else {
                char tmp[12]; int ti = 0;
                while (v) { tmp[ti++] = (char)('0' + v % 10); v /= 10; }
                if (neg) tmp[ti++] = '-';
                while (ti > 0) num_buf[i++] = tmp[--ti];
            }
            num_buf[i] = '\0';
            insert = num_buf;
            si++;
        } else if (strncmp(src + si, "HOME", 4) == 0 && !(src[si+4] >= 'A' && src[si+4] <= 'Z') && !(src[si+4] >= 'a' && src[si+4] <= 'z') && src[si+4] != '_') {
            insert = (uid == 0) ? "/root" : "/home/user";
            si += 4;
        } else if (strncmp(src + si, "PATH", 4) == 0 && !(src[si+4] >= 'A' && src[si+4] <= 'Z') && !(src[si+4] >= 'a' && src[si+4] <= 'z') && src[si+4] != '_') {
            insert = "/bin";
            si += 4;
        } else if (strncmp(src + si, "USERNAME", 8) == 0) {
            insert = username;
            si += 8;
        } else if (strncmp(src + si, "USER", 4) == 0 && !(src[si+4] >= 'A' && src[si+4] <= 'Z') && !(src[si+4] >= 'a' && src[si+4] <= 'z') && src[si+4] != '_') {
            insert = username;
            si += 4;
        } else {
            /* Unknown variable — emit literal '$'. */
            dst[di++] = '$';
            continue;
        }

        if (insert) {
            while (*insert && di < cap - 1)
                dst[di++] = *insert++;
        }
    }
    dst[di] = '\0';
}

/* ---- Tab completion ------------------------------------------------------ */

static void prompt_str(const char *username, uint32_t uid);  /* forward decl */

/*
 * Complete the current token in buf[0..len-1].
 * Searches /bin/ for command completion or the parent directory for path
 * completion. On a unique match, fills in the missing characters.
 * On multiple matches, lists them and reprints the prompt + buffer.
 */
static void tab_complete(char *buf, int *len_ptr,
                         const char *username, uint32_t uid) {
    int len = *len_ptr;

    /* Find start of the current (incomplete) token. */
    int tok_start = len;
    while (tok_start > 0 && buf[tok_start - 1] != ' ') tok_start--;
    const char *tok = buf + tok_start;
    int tok_len = len - tok_start;

    /* Decide whether this is a path completion or a command completion. */
    int is_path = 0;
    int i;
    for (i = 0; i < tok_len; ++i) {
        if (tok[i] == '/') { is_path = 1; break; }
    }
    if (tok_len > 0 && tok[0] == '.') is_path = 1;

    char dir_path[MAX_PATH];
    const char *prefix;
    int prefix_len;

    if (is_path) {
        /* Split the token into directory + prefix basename. */
        int last_slash = -1;
        for (i = 0; i < tok_len; ++i) {
            if (tok[i] == '/') last_slash = i;
        }
        if (last_slash < 0) {
            strcpy(dir_path, g_cwd);
            prefix = tok;
            prefix_len = tok_len;
        } else {
            /* Copy directory portion. */
            if (tok[0] == '/') {
                int di = 0;
                for (i = 0; i <= last_slash && i < MAX_PATH - 1; ++i)
                    dir_path[di++] = tok[i];
                dir_path[di] = '\0';
            } else {
                strcpy(dir_path, g_cwd);
                if (dir_path[strlen(dir_path) - 1] != '/')
                    strcat(dir_path, "/");
                int di = (int)strlen(dir_path);
                for (i = 0; i <= last_slash && di < MAX_PATH - 1; ++i)
                    dir_path[di++] = tok[i];
                dir_path[di] = '\0';
            }
            prefix = tok + last_slash + 1;
            prefix_len = tok_len - last_slash - 1;
        }
    } else {
        strcpy(dir_path, "/bin");
        prefix = tok;
        prefix_len = tok_len;
    }

    /* Collect matching entries. */
#define COMP_MAX 32
    static char matches[COMP_MAX][64];
    int match_count = 0;
    char entry[64];
    uint32_t idx;

    for (idx = 0; match_count < COMP_MAX; ++idx) {
        entry[0] = '\0';
        int r = readdir_entry(dir_path, idx, entry, sizeof(entry));
        if (r <= 0) break;
        if (!entry[0]) continue;
        if (prefix_len == 0 || strncmp(entry, prefix, (size_t)prefix_len) == 0) {
            strcpy(matches[match_count++], entry);
        }
    }

    if (match_count == 0) return;

    if (match_count == 1) {
        /* Single match: append the missing characters to the buffer. */
        const char *rest = matches[0] + prefix_len;
        while (*rest && len < LINE_CAP - 1) {
            buf[len++] = *rest;
            write_char(*rest);
            rest++;
        }
        *len_ptr = len;
        return;
    }

    /* Multiple matches: show them on a new line then reprint the prompt. */
    write_str("\n");
    for (i = 0; i < match_count; ++i) {
        write_str(matches[i]);
        write_char(' ');
    }
    write_str("\n");
    prompt_str(username, uid);
    write_stdout(buf, (size_t)len);
}

/* ---- Readline with history and Ctrl sequences ---------------------------- */

/*
 * Read a line character-by-character so we can handle arrow keys, backspace,
 * Ctrl+C, Ctrl+U and tab completion without relying on any line discipline.
 * The terminal in VibeOS passes raw bytes so we must do all of this ourselves.
 */
static void readline(char *buf, int cap, const char *username, uint32_t uid) {
    int len = 0;
    int hist_nav = 0;  /* how many entries back we have navigated; 0 = current */

    /* Read the whole line at once — the TTY/PTY delivers a complete line
     * when the user presses Enter.  Reading char-by-char conflicts with
     * TTY canonical echo and produces empty or garbled input. */
    {
        ssize_t n;
        while (1) {
            n = syscall3(SYS_READ, 0, (uint64_t)(size_t)buf, (uint64_t)(cap - 1));
            if (n > 0) break;
            yield();
        }
        /* Strip trailing CR/LF. */
        while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r')) n--;
        buf[n] = '\0';
        len = (int)n;

        /* Add to history if non-empty. */
        if (len > 0) hist_add(buf);
        (void)hist_nav; (void)username; (void)uid;
        return;
    }

    /* --- Dead code below: char-by-char editor (kept for future raw-mode use) --- */
    while (1) {
        char c = 0;
        ssize_t n = syscall3(SYS_READ, 0, (uint64_t)(size_t)&c, 1);
        if (n <= 0) { yield(); continue; }

        if (c == '\n' || c == '\r') {
            buf[len] = '\0';
            write_char('\n');
            return;
        }

        if (c == 0x03) {
            /* Ctrl+C: cancel the current line. */
            write_str("^C\n");
            buf[0] = '\0';
            return;
        }

        if (c == 0x15) {
            /* Ctrl+U: erase entire line by emitting backspace-space-backspace
             * for each character already typed. */
            while (len > 0) {
                write_str("\b \b");
                len--;
            }
            continue;
        }

        if (c == '\t') {
            tab_complete(buf, &len, username, uid);
            continue;
        }

        if (c == '\x7f' || c == '\b') {
            /* Backspace: erase last character from display and buffer. */
            if (len > 0) {
                write_str("\b \b");
                len--;
            }
            continue;
        }

        if (c == '\x1b') {
            /* Escape sequence: read '[' then the final byte. */
            char seq[2] = {0, 0};
            syscall3(SYS_READ, 0, (uint64_t)(size_t)&seq[0], 1);
            syscall3(SYS_READ, 0, (uint64_t)(size_t)&seq[1], 1);
            if (seq[0] != '[') continue;

            if (seq[1] == 'A') {
                /* Arrow up: navigate history backwards. */
                const char *entry = hist_get(hist_nav + 1);
                if (!entry) continue;
                hist_nav++;
                /* Erase current line and substitute the history entry. */
                while (len > 0) { write_str("\b \b"); len--; }
                while (*entry && len < cap - 1) {
                    buf[len++] = *entry;
                    write_char(*entry);
                    entry++;
                }
            } else if (seq[1] == 'B') {
                /* Arrow down: navigate history forwards (toward present). */
                if (hist_nav == 0) continue;
                hist_nav--;
                while (len > 0) { write_str("\b \b"); len--; }
                if (hist_nav > 0) {
                    const char *entry = hist_get(hist_nav);
                    if (entry) {
                        while (*entry && len < cap - 1) {
                            buf[len++] = *entry;
                            write_char(*entry);
                            entry++;
                        }
                    }
                }
                /* hist_nav == 0 means "back to empty current line". */
            }
            /* Left/right arrows: ignore — cursor movement not yet implemented. */
            continue;
        }

        /* Ordinary printable character. */
        if (c >= 0x20 && c < 0x7f && len < cap - 1) {
            buf[len++] = c;
            write_char(c);
            hist_nav = 0;  /* Any new input resets history navigation. */
        }
    }
}

/* ---- Utility helpers ----------------------------------------------------- */

static void write_dec(uint64_t n);   /* defined later */

static const char *strerror(int err) {
    if (err < 0) err = -err;
    switch (err) {
        case 0: return "Success";
        case 1: return "Operation not permitted";
        case 2: return "No such file or directory";
        case 5: return "I/O error";
        case 9: return "Bad file descriptor";
        case 12: return "Out of memory";
        case 13: return "Permission denied";
        case 17: return "File exists";
        case 20: return "Not a directory";
        case 21: return "Is a directory";
        case 22: return "Invalid argument";
        case 24: return "Too many open files";
        case 27: return "File too large";
        case 28: return "No space left on device";
        case 30: return "Read-only file system";
        case 36: return "File name too long";
        case 38: return "Function not implemented";
        default: return "Unknown error";
    }
}

/* ---- Prompt -------------------------------------------------------------- */

static char g_username[32];
static uint32_t g_uid;

/*
 * Emit the prompt: "name@vibeos:/path# " for root, "name@vibeos:/path$ " for
 * regular users.  The '#' vs '$' distinction is the Unix convention.
 */
static void prompt_str(const char *username, uint32_t uid) {
    write_str(username);
    write_str("@vibeos:");
    write_str(g_cwd);
    write_str(uid == 0 ? "# " : "$ ");
}

static void prompt(void) {
    prompt_str(g_username, g_uid);
}

/* ---- Command helpers ----------------------------------------------------- */

static void show_help(void) {
    write_line("Commands: help, about, clear, ls, cd, pwd, cat, edit, stat, echo, touch, mkdir, rm, cp, mv, ifconfig, ping, curl, display, gui, uidemo, taskmgr, browser, exit");
}

static void show_about(void) {
    struct system_info_snapshot info;
    int res = (int)syscall1(SYS_SYSTEM_INFO, (uint64_t)(size_t)&info);
    if (res == 0) {
        write_str("VibeOS Kernel v");
        write_line(info.version);
    } else {
        write_str("Kernel version fetch failed. Res: ");
        write_dec((uint64_t)-res);
        write_line("");
    }
}

static void cmd_clear(void) {
    write_str("\x1b[2J\x1b[H");
}

static void cmd_pwd(void) {
    write_line(g_cwd);
}

static void build_full_path(const char *path) {
    size_t i = 0;
    if (path[0] == '/') {
        while (path[i] && i < MAX_PATH - 1) { g_full_path[i] = path[i]; i++; }
        g_full_path[i] = 0;
    } else {
        while (g_cwd[i]) { g_full_path[i] = g_cwd[i]; i++; }
        if (i > 0 && g_full_path[i-1] != '/') g_full_path[i++] = '/';
        const char *p = path;
        while (*p && i < MAX_PATH - 1) g_full_path[i++] = *p++;
        g_full_path[i] = 0;
    }
}

static void normalize_path(char *path) {
    char *p = path;
    char *out = path;

    while (*p) {
        if (p[0] == '/' && p[1] == '.') {
            if (p[2] == '.' && (p[3] == '/' || p[3] == 0)) {
                p += 3;
                while (out > path && *--out != '/');
            } else if (p[2] == '/' || p[2] == 0) {
                p += 2;
            } else {
                *out++ = *p++;
            }
        } else {
            *out++ = *p++;
        }
    }

    if (out == path) {
        *out++ = '/';
    } else if (out > path + 1 && out[-1] == '/') {
        /* Remove trailing slash if not root. */
        out--;
    }
    *out = 0;
}

/*
 * Change directory — silently update g_cwd on success, print an error on
 * failure. Debug prints removed; they were useful only during initial dev.
 */
static void cmd_cd(const char *path) {
    size_t i, j;
    char buf[MAX_PATH];

    if (!path || !path[0]) {
        path = "/";
    }

    i = 0;

    if (path[0] != '/') {
        for (j = 0; j < MAX_PATH - 1 && g_cwd[j]; j++) {
            buf[i++] = g_cwd[j];
        }
        if (i > 0 && buf[i - 1] != '/') {
            buf[i++] = '/';
        }
    }

    for (j = 0; i < MAX_PATH - 1 && path[j]; j++) {
        buf[i++] = path[j];
    }
    buf[i] = 0;

    normalize_path(buf);

    int res = chdir(buf);
    if (res == 0) {
        for (i = 0; i < sizeof(g_cwd) - 1 && buf[i]; i++) {
            g_cwd[i] = buf[i];
        }
        g_cwd[i] = 0;
    } else {
        write_str("cd: ");
        write_str(path);
        write_str(": ");
        write_line(strerror(res));
    }
}

/* Call SYS_STAT and return kind (1=file, 2=dir), filling *size and *mode.
 * Returns -1 if the path does not exist. */
static int ls_stat(const char *path, uint64_t *size, uint16_t *mode) {
    long ret, kind;
    register long r8 __asm__("r8") = 0;
    register long r9 __asm__("r9") = 0;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret), "=d"(kind), "+r"(r8), "+r"(r9)
        : "a"((uint64_t)SYS_STAT), "D"((uint64_t)(size_t)path)
        : "rcx", "r11", "memory"
    );
    if (ret < 0) return -1;
    if (size) *size = (uint64_t)r8;
    if (mode) *mode = (uint16_t)(r9 & 0xFFFFu);
    return (int)kind;
}

/* Format permission bits as "drwxr-xr-x" (10 chars + NUL). */
static void ls_mode_str(uint32_t kind, uint16_t mode, char out[11]) {
    out[0] = (kind == 2) ? 'd' : '-';
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

/* Right-align a decimal number in a field of 'width' characters. */
static void write_dec_w(uint64_t n, int width) {
    char buf[20];
    int i = 0;
    if (n == 0) { buf[i++] = '0'; }
    else { while (n) { buf[i++] = (char)('0' + n % 10); n /= 10; } }
    for (int pad = i; pad < width; pad++) write_char(' ');
    for (int j = i - 1; j >= 0; j--) write_char(buf[j]);
}

static void cmd_ls(void) {
    /* Parse flags and optional path from g_args. */
    int flag_l = 0, flag_a = 0;
    const char *path = g_cwd;

    for (int i = 1; i < g_argc; i++) {
        if (g_args[i][0] == '-' && g_args[i][1]) {
            for (int j = 1; g_args[i][j]; j++) {
                if (g_args[i][j] == 'l') flag_l = 1;
                else if (g_args[i][j] == 'a') flag_a = 1;
                /* other flags silently ignored */
            }
        } else {
            path = g_args[i];
        }
    }
    if (!path || !path[0]) path = g_cwd;

    char name[64];
    char full[MAX_PATH];

    for (uint32_t index = 0; ; index++) {
        name[0] = 0;
        int result = readdir_entry(path, index, name, sizeof(name));
        if (result < 0) {
            write_str("ls: "); write_str(path); write_str(": ");
            write_line(strerror(result));
            break;
        }
        if (result == 0) break;
        if (!name[0]) continue;

        /* -a: show hidden (dot) files; without -a skip them. */
        if (!flag_a && name[0] == '.') continue;

        if (flag_l) {
            /* Build full path to stat the entry. */
            size_t plen = strlen(path);
            size_t nlen = strlen(name);
            if (plen + nlen + 2 < (size_t)MAX_PATH) {
                size_t k = 0;
                while (path[k]) full[k++] = path[k];
                if (k > 0 && full[k-1] != '/') full[k++] = '/';
                size_t m = 0;
                while (name[m]) full[k++] = name[m++];
                full[k] = '\0';
            } else {
                full[0] = '\0';
            }

            uint64_t size = 0;
            uint16_t mode = 0644;
            int kind = ls_stat(full, &size, &mode);
            if (kind < 0) kind = 1;

            /* If mode is 0 (kernel didn't fill it), use a default. */
            if (!mode) mode = (kind == 2) ? 0755u : 0644u;

            char mstr[11];
            ls_mode_str((uint32_t)kind, mode, mstr);

            write_str(mstr);
            write_char(' ');
            write_dec_w(size, 8);
            write_char(' ');
            write_line(name);
        } else {
            write_line(name);
        }
    }
}

static void write_dec(uint64_t n);   /* defined later */

static int parse_uint(const char *s) {
    int n = 0;
    if (!s) return -1;
    while (*s) {
        if (*s < '0' || *s > '9') return -1;
        n = n * 10 + (*s - '0');
        s++;
    }
    return n;
}

static void cmd_display(int argc) {
    /* Curated list of resolutions the BGA/QEMU std-vga reliably supports. */
    static const char *modes[] = {
        "1920 1080", "1680 1050", "1512 982", "1440 900",
        "1366 768", "1280 800", "1280 720", "1024 768", "800 600", 0
    };
    uint32_t cur = (uint32_t)syscall2(SYS_DISPLAY_MODE, 0, 0);
    int i;

    if (argc >= 3) {
        int w = parse_uint(g_args[1]);
        int h = parse_uint(g_args[2]);
        if (w <= 0 || h <= 0) { write_line("Usage: display <width> <height>"); return; }
        syscall2(SYS_DISPLAY_MODE, (uint64_t)w, (uint64_t)h);
        write_str("Display set to "); write_dec((uint64_t)w); write_str("x"); write_dec((uint64_t)h);
        write_line(" (applies immediately if the desktop is running)");
        return;
    }

    write_str("Current: "); write_dec((cur >> 16) & 0xffff); write_str("x"); write_dec(cur & 0xffff); write_line("");
    write_line("Supported modes (use: display <w> <h>):");
    for (i = 0; modes[i]; ++i) { write_str("  "); write_line(modes[i]); }
}

static void itoa(uint64_t n, char *s) {
    char buf[24];
    int i = 0;
    if (n == 0) {
        s[0] = '0';
        s[1] = '\0';
        return;
    }
    while (n > 0) {
        buf[i++] = (n % 10) + '0';
        n /= 10;
    }
    int j = 0;
    while (i > 0) {
        s[j++] = buf[--i];
    }
    s[j] = '\0';
}

static void write_dec(uint64_t n) {
    char buf[24];
    itoa(n, buf);
    write_str(buf);
}

static void write_hex64(uint64_t n) {
    const char *hex = "0123456789abcdef";
    char buf[16];
    int i = 0;
    write_str("0x");
    if (n == 0) {
        write_char('0');
        return;
    }
    while (n && i < 16) {
        buf[i++] = hex[n & 0xfu];
        n >>= 4;
    }
    while (i > 0) {
        write_char(buf[--i]);
    }
}

static void write_ip(uint32_t ip) {
    write_dec((ip >> 24) & 0xff); write_str(".");
    write_dec((ip >> 16) & 0xff); write_str(".");
    write_dec((ip >> 8) & 0xff);  write_str(".");
    write_dec(ip & 0xff);
}

/* Must match struct journal_entry in kernel/include/journal.h. */
struct sh_journal_entry {
    uint64_t seq;
    uint64_t tick;
    uint64_t boot_id;
    uint32_t hz;
    uint32_t level;
    uint32_t pid;
    char msg[96];
};

static void cmd_dmesg(void) {
    static const char *tags[] = {"INFO ", "WARN ", "ERROR", "FAULT", "APP  "};
    uint64_t total = (uint64_t)syscall2(SYS_JOURNAL_READ, 0, 0);
    uint64_t seq;
    struct sh_journal_entry e;

    for (seq = 0; seq < total; ++seq) {
        if (syscall2(SYS_JOURNAL_READ, (uint64_t)seq, (uint64_t)(size_t)&e) != 1) {
            continue;
        }
        write_str("[#");
        write_dec(e.seq);
        write_str(" t=");
        if (e.hz) {
            write_dec(e.tick / e.hz);
            write_str(".");
            {
                uint64_t ms = ((e.tick % e.hz) * 1000u) / e.hz;
                if (ms < 100) write_str("0");
                if (ms < 10) write_str("0");
                write_dec(ms);
            }
            write_str("s");
        } else {
            write_dec(e.tick);
            write_str(" ticks");
        }
        write_str(" boot=");
        write_hex64(e.boot_id);
        write_str("] ");
        write_str(e.level < 5 ? tags[e.level] : "?????");
        write_str(" pid=");
        write_dec(e.pid);
        write_str("  ");
        write_line(e.msg);
    }
}

static void write_hex2(uint8_t v) {
    const char *h = "0123456789abcdef";
    char b[3];
    b[0] = h[(v >> 4) & 0xf];
    b[1] = h[v & 0xf];
    b[2] = 0;
    write_str(b);
}

static int parse_ipv4(const char *s, uint32_t *out) {
    uint32_t parts[4] = {0,0,0,0};
    int idx = 0, digits = 0;
    while (*s) {
        if (*s >= '0' && *s <= '9') {
            parts[idx] = parts[idx]*10 + (uint32_t)(*s - '0');
            if (parts[idx] > 255) return 0;
            digits++;
        } else if (*s == '.') {
            if (!digits || idx == 3) return 0;
            idx++; digits = 0;
        } else return 0;
        s++;
    }
    if (idx != 3 || !digits) return 0;
    *out = (parts[0]<<24)|(parts[1]<<16)|(parts[2]<<8)|parts[3];
    return 1;
}

static void cmd_ifconfig(void) {
    struct net_info info;
    int i;
    syscall1(SYS_NET_INFO, (uint64_t)(size_t)&info);
    if (!info.up) {
        write_line("net0: no network interface (NIC not found)");
        return;
    }
    write_str("net0  Link encap:Ethernet  HWaddr ");
    for (i = 0; i < 6; i++) {
        write_hex2(info.mac[i]);
        if (i < 5) write_str(":");
    }
    write_str("\n      inet addr:"); write_ip(info.ip);
    write_str("  Mask:"); write_ip(info.netmask);
    write_str("\n      gateway:"); write_ip(info.gateway);
    write_line("  status:UP");
}

static void cmd_ping(const char *args) {
    uint32_t ip;
    int rtt = -1;
    int replies;
    int sent;
    int count = 4;
    int continuous = 0;
    int timeout_ms = 1000;
    char target[128];

    if (!args || !args[0]) {
        write_line("Usage: ping [-c count] <host|a.b.c.d>");
        write_line("       ping -t <host|a.b.c.d>   (continuous)");
        return;
    }

    const char *target_str = args;
    if (args[0] == '-') {
        if (args[1] == 'c' && args[2] == ' ') {
            count = 0;
            const char *p = args + 3;
            while (*p >= '0' && *p <= '9') {
                count = count * 10 + (*p - '0');
                ++p;
            }
            if (count <= 0) { write_line("ping: invalid count"); return; }
            target_str = p;
            while (*target_str == ' ') ++target_str;
        } else if (args[1] == 't' && args[2] == ' ') {
            continuous = 1;
            target_str = args + 3;
            while (*target_str == ' ') ++target_str;
        } else {
            write_str("ping: unknown flag '");
            write_char(args[1]);
            write_line("'");
            return;
        }
    }

    if (!target_str[0]) {
        write_line("Usage: ping [-c count] <host|a.b.c.d>");
        return;
    }

    {
        int i = 0;
        while (target_str[i] && i + 1 < (int)sizeof(target)) {
            target[i] = target_str[i];
            ++i;
        }
        target[i] = '\0';
    }

    if (!parse_ipv4(target, &ip)) {
        int r = (int)syscall2(SYS_NET_RESOLVE, (uint64_t)(size_t)target, (uint64_t)(size_t)&ip);
        if (r < 0) {
            write_str("ping: cannot resolve ");
            write_line(target);
            return;
        }
        write_str("Resolved "); write_str(target); write_str(" -> "); write_ip(ip); write_line("");
    }

    write_str("PING "); write_ip(ip);
    if (continuous) write_line(" (continuous, Ctrl+C to stop)");
    else { write_str(" : "); write_dec((uint64_t)count); write_line(" packets"); }

    replies = 0;
    sent = 0;

    while (continuous || sent < count) {
        rtt = -1;
        int seq = sent + 1;
        int res = (int)syscall4(SYS_NET_PING, (uint64_t)ip, (uint64_t)1,
                                (uint64_t)timeout_ms, (uint64_t)(size_t)&rtt);
        ++sent;

        if (res >= 0) {
            ++replies;
            write_str("seq="); write_dec((uint64_t)seq);
            write_str(" : reply from "); write_ip(ip);
            write_str(" rtt="); write_dec((uint64_t)rtt); write_line(" ms");
        } else {
            write_str("seq="); write_dec((uint64_t)seq);
            write_line(" : timeout");
        }

        if (continuous) {
            syscall1(SYS_TIMER_SLEEP, 1000);
            char c = 0;
            syscall1(SYS_YIELD, 0);
            ssize_t nr = syscall3(SYS_READ, (uint64_t)0, (uint64_t)(size_t)&c, (uint64_t)1);
            if (nr > 0 && c == 0x03) {
                write_line("^C");
                break;
            }
        } else {
            syscall1(SYS_TIMER_SLEEP, 200);
        }
    }

    write_str("\n--- "); write_ip(ip);
    write_str(" ping statistics: ");
    write_dec((uint64_t)sent);
    write_str(" packets transmitted, ");
    write_dec((uint64_t)replies);
    write_str(" received");
    if (sent > 0 && replies > 0) {
        int loss = 100 - (replies * 100 / sent);
        write_str(", "); write_dec((uint64_t)loss); write_str("% loss");
    }
    write_line("");
    (void)rtt;
    (void)replies;
}

struct http_req {
    uint32_t ip;
    uint16_t port;
    const char *host;
    const char *path;
    char *out;
    int cap;
    const char *user_agent;
    int timeout_ms;
};

static char g_http_buf[8192];
static char g_host_buf[128];
static char g_path_buf[256];

static void cmd_curl(const char *url) {
    uint32_t ip;
    int i = 0;
    int h = 0;
    int p = 0;
    int n;
    struct http_req req;

    if (!url || !url[0]) {
        write_line("Usage: curl <host[/path]>");
        return;
    }
    if (url[0]=='h'&&url[1]=='t'&&url[2]=='t'&&url[3]=='p'&&url[4]==':'&&url[5]=='/'&&url[6]=='/') {
        url += 7;
    }
    while (url[i] && url[i] != '/' && h < (int)sizeof(g_host_buf) - 1) {
        g_host_buf[h++] = url[i++];
    }
    g_host_buf[h] = 0;
    g_path_buf[p++] = '/';
    if (url[i] == '/') {
        i++;
        while (url[i] && p < (int)sizeof(g_path_buf) - 1) {
            g_path_buf[p++] = url[i++];
        }
    }
    g_path_buf[p] = 0;

    if (!parse_ipv4(g_host_buf, &ip)) {
        int r = (int)syscall2(SYS_NET_RESOLVE, (uint64_t)(size_t)g_host_buf, (uint64_t)(size_t)&ip);
        if (r < 0) {
            write_str("curl: cannot resolve "); write_line(g_host_buf);
            return;
        }
    }

    write_str("Connecting to "); write_str(g_host_buf); write_str(" ("); write_ip(ip); write_line(") :80");

    req.ip = ip;
    req.port = 80;
    req.host = g_host_buf;
    req.path = g_path_buf;
    req.out = g_http_buf;
    req.cap = (int)sizeof(g_http_buf) - 1;
    req.user_agent = "VibeOS github.com/MarvInt64/vibe-os";
    req.timeout_ms = 0;

    n = (int)syscall1(SYS_NET_HTTP_GET, (uint64_t)(size_t)&req);
    if (n <= 0) {
        write_line("curl: request failed");
        return;
    }
    g_http_buf[n] = 0;
    write_stdout(g_http_buf, (size_t)n);
    write_line("");
    write_str("["); write_dec((uint64_t)n); write_line(" bytes received]");
}

/* ---- Input parsing ------------------------------------------------------- */

static void parse_input(void) {
    char *p = g_expanded;
    g_argc = 0;

    while (*p && g_argc < MAX_ARGS) {
        char *dst;
        size_t len = 0;

        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;

        dst = g_args[g_argc];
        while (*p && *p != ' ' && *p != '\t' && len < MAX_ARG_LEN - 1) {
            dst[len++] = *p++;
        }
        dst[len] = 0;
        g_argc++;
    }
}

/* ---- Background-app detection -------------------------------------------- */

/*
 * GUI apps must run in the background so the shell remains interactive.
 * Centralising the check here avoids the two identical blocks that existed
 * before, one for is_path commands and one for /bin/ commands.
 */
static int is_background_app(const char *cmd) {
    return strcmp(cmd, "uidemo")    == 0 ||
           strcmp(cmd, "taskmgr")  == 0 ||
           strcmp(cmd, "browser")  == 0 ||
           strcmp(cmd, "dock")     == 0 ||
           strcmp(cmd, "sysinfo")  == 0 ||
           strcmp(cmd, "wallpaper") == 0 ||
           strcmp(cmd, "audiocfg") == 0;
}

static int spawn_with_arguments(const char *path) {
    char full_args[256];
    full_args[0] = '\0';
    for (int i = 1; i < g_argc && i < MAX_ARGS; ++i) {
        if (i > 1) strcat(full_args, " ");
        strcat(full_args, g_args[i]);
    }
    if (full_args[0] != '\0') {
        return spawn_with_arg(path, full_args);
    } else {
        return spawn(path);
    }
}

/* ---- Command dispatcher -------------------------------------------------- */

static void execute_command(void) {
    if (g_argc == 0) return;

    char *cmd = g_args[0];
    char spawn_path[MAX_PATH];
    size_t i;

    if (strcmp(cmd, "help") == 0) {
        show_help();
    } else if (strcmp(cmd, "about") == 0) {
        show_about();
    } else if (strcmp(cmd, "clear") == 0) {
        cmd_clear();
    } else if (strcmp(cmd, "pwd") == 0) {
        cmd_pwd();
    } else if (strcmp(cmd, "cd") == 0) {
        cmd_cd(g_argc > 1 ? g_args[1] : "");
    } else if (strcmp(cmd, "ls") == 0) {
        cmd_ls();
    } else if (strcmp(cmd, "ifconfig") == 0 || strcmp(cmd, "ip") == 0) {
        cmd_ifconfig();
    } else if (strcmp(cmd, "ping") == 0) {
        g_ping_arg_buf[0] = '\0';
        for (int k = 1; k < g_argc && k < 16; ++k) {
            if (k > 1) {
                int len = 0; while (g_ping_arg_buf[len]) ++len;
                if (len + 1 < (int)sizeof(g_ping_arg_buf)) g_ping_arg_buf[len] = ' ';
            }
            int bi = 0; while (g_ping_arg_buf[bi]) ++bi;
            int ai = 0;
            while (g_args[k][ai] && bi + 1 < (int)sizeof(g_ping_arg_buf)) {
                g_ping_arg_buf[bi++] = g_args[k][ai++];
            }
            g_ping_arg_buf[bi] = '\0';
        }
        cmd_ping(g_ping_arg_buf);
    } else if (strcmp(cmd, "curl") == 0 || strcmp(cmd, "fetch") == 0 || strcmp(cmd, "wget") == 0) {
        cmd_curl(g_argc > 1 ? g_args[1] : "");
    } else if (strcmp(cmd, "display") == 0 || strcmp(cmd, "resolution") == 0) {
        cmd_display(g_argc);
    } else if (strcmp(cmd, "dmesg") == 0 || strcmp(cmd, "journal") == 0) {
        cmd_dmesg();
    } else if (strcmp(cmd, "gui") == 0 || strcmp(cmd, "wm") == 0 || strcmp(cmd, "desktop") == 0) {
        write_line("Starting VibeOS desktop...");
        syscall1(SYS_WINDOWMGR_START, 0);
    } else if (strcmp(cmd, "exit") == 0) {
        write_line("Goodbye!");
        syscall1(SYS_EXIT, 0);
    } else {
        int is_path = (cmd[0] == '/' || (cmd[0] == '.' && cmd[1] == '/'));

        if (is_path) {
            if (cmd[0] == '.' && cmd[1] == '/') {
                spawn_path[0] = '\0';
                for (i = 0; i < MAX_PATH - 1 && g_cwd[i]; i++) {
                    spawn_path[i] = g_cwd[i];
                }
                if (i > 0 && spawn_path[i - 1] != '/') {
                    spawn_path[i++] = '/';
                }
                char *arg = cmd + 2;
                while (i < MAX_PATH - 1 && *arg) {
                    spawn_path[i++] = *arg++;
                }
                spawn_path[i] = 0;
            } else {
                for (i = 0; i < MAX_PATH - 1 && cmd[i]; i++) {
                    spawn_path[i] = cmd[i];
                }
                spawn_path[i] = 0;
            }
        } else {
            spawn_path[0] = '\0';
            strcat(spawn_path, "/bin/");
            strcat(spawn_path, cmd);
        }

        /* Check explicit '&' suffix first, then the known GUI app list. */
        int bg = 0;
        if (g_argc > 1 && strcmp(g_args[g_argc - 1], "&") == 0) {
            bg = 1;
            g_argc--;
        } else if (is_background_app(cmd)) {
            bg = 1;
        }

        int pid = spawn_with_arguments(spawn_path);
        if (pid <= 0) {
            if (pid == -2) {
                write_str("Unknown: ");
                write_line(cmd);
            } else {
                write_str("spawn: ");
                write_str(spawn_path);
                write_str(": ");
                write_line(strerror(pid));
            }
            g_last_exit = pid;
        } else {
            if (!bg) {
                /* Capture the child's exit code so $? works. */
                g_last_exit = waitpid_ret(pid);
            }
        }
    }
}

/* ---- Entry point --------------------------------------------------------- */

void _start(void) {
    struct system_info_snapshot info;

    getcwd(g_cwd, sizeof(g_cwd));

    /* Fetch uid once at startup; it can change via su in a child process but
     * the shell itself keeps running as the same uid. */
    g_uid = (uint32_t)syscall1(SYS_GETUID, 0);
    get_username(g_uid, g_username, sizeof(g_username));

    write_line("");
    if (syscall1(SYS_SYSTEM_INFO, (uint64_t)(size_t)&info) == 0) {
        write_str("VibeOS Shell | Kernel v");
        write_line(info.version);
    } else {
        write_line("VibeOS Shell v2.0 (fallback)");
    }
    write_line("");

    while (1) {
        prompt();

        /* readline handles all line-editing, history, and tab completion. */
        readline(g_input, sizeof(g_input), g_username, g_uid);

        if (!g_input[0]) continue;

        /* Expand variables before parsing so $? and $HOME work everywhere. */
        expand_vars(g_input, g_expanded, sizeof(g_expanded),
                    g_last_exit, g_uid, g_username);

        hist_add(g_input);  /* Store the raw (pre-expansion) line in history. */

        parse_input();
        execute_command();
    }
}
