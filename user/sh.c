/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Marvin Kicha <https://github.com/MarvInt64/> */

#define SYS_READ 0
#define SYS_WRITE 1
#define SYS_IOCTL 2
#define SYS_YIELD 3
#define SYS_EXIT 4
#define SYS_PROCESS_SPAWN 5
#define SYS_WAITPID 6
#define SYS_OPEN 7
#define SYS_CLOSE 8
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

static char *strcat(char *dest, const char *src) {
  char *ret = dest;
  while (*dest) dest++;
  while ((*dest++ = *src++));
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

static ssize_t read_stdin(char *buf, size_t len) {
    ssize_t n;
    while (1) {
        n = syscall3(SYS_READ, 0, (uint64_t)(size_t)buf, len);
        if (n > 0) return n;
        yield();
    }
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

static void waitpid(int pid) {
    syscall2(SYS_WAITPID, (uint64_t)pid, 0);
}

#define MAX_ARGS 8
#define MAX_ARG_LEN 64
#define MAX_PATH 256
#define MAX_INPUT 128

static char g_cwd[MAX_PATH] = "/";
static char g_input[MAX_INPUT];
static char g_args[MAX_ARGS][MAX_ARG_LEN];
static int g_argc;
static char g_full_path[MAX_PATH];
static char g_io_buf[1024];
static char g_ping_arg_buf[256];
static char g_cd_path[MAX_PATH];
static char g_cd_buf[MAX_PATH];

static int spawn_with_arguments(const char *path) {
    char full_args[256];
    full_args[0] = '\0';
    for (int i = 1; i < g_argc && i < MAX_ARGS; ++i) {
        if (i > 1) {
            strcat(full_args, " ");
        }
        strcat(full_args, g_args[i]);
    }
    if (full_args[0] != '\0') {
        return spawn_with_arg(path, full_args);
    } else {
        return spawn(path);
    }
}

static void prompt(void) {
    write_str(g_cwd);
    write_str("$ ");
}

static void show_help(void) {
	write_line("Commands: help, about, clear, ls, cd, pwd, cat, edit, stat, echo, touch, mkdir, rm, cp, mv, ifconfig, ping, curl, display, gui, uidemo, taskmgr, browser, exit");
}

static void write_dec(uint64_t n);   /* defined later */

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
        /* Remove trailing slash if not root */
        out--;
    }
    *out = 0;
}

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

static void cmd_cd(const char *path) {
    size_t i, j;
    char buf[MAX_PATH];

    write_str("CD: path='");
    write_str(path);
    write_str("'\n");

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

    write_str("CD: buf='");
    write_str(buf);
    write_str("'\n");

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

static void cmd_ls(const char *path) {
	char name[64];
	uint32_t index;
	int result;

	if (!path || !path[0]) path = g_cwd;

	for (index = 0; ; index++) {
		name[0] = 0;
		result = readdir_entry(path, index, name, sizeof(name));
		if (result < 0) {
            write_str("ls: ");
            write_str(path);
            write_str(": ");
            write_line(strerror(result));
            break;
        }
        if (result == 0) break;
		if (name[0]) write_line(name);
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
    /* Curated list of resolutions the BGA/QEMU std-vga reliably supports
     * (all within the compositor's 1920x1080 buffer limit). */
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
            continue;   /* scrolled out of the ring */
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

    /* Parse optional flags before the target. */
    const char *target_str = args;
    if (args[0] == '-') {
        /* '-c N' or '-t' */
        if (args[1] == 'c' && args[2] == ' ') {
            count = 0;
            const char *p = args + 3;
            while (*p >= '0' && *p <= '9') {
                count = count * 10 + (*p - '0');
                ++p;
            }
            if (count <= 0) {
                write_line("ping: invalid count");
                return;
            }
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

    /* Copy target to avoid modifying original string */
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
            write_str("seq=");
            write_dec((uint64_t)seq);
            write_str(" : reply from "); write_ip(ip);
            write_str(" rtt="); write_dec((uint64_t)rtt); write_line(" ms");
        } else {
            write_str("seq=");
            write_dec((uint64_t)seq);
            write_line(" : timeout");
        }

        if (continuous) {
            /* Sleep 1 second between pings. Check for Ctrl+C between sends. */
            syscall1(SYS_TIMER_SLEEP, 1000);

            /* Check if a key was pressed (non-blocking read of 1 byte). */
            char c = 0;
            /* Use a non-blocking read with minimal timeout: yield then check. */
            syscall1(SYS_YIELD, 0);
            ssize_t nr = syscall3(SYS_READ, (uint64_t)0, (uint64_t)(size_t)&c, (uint64_t)1);
            if (nr > 0 && c == 0x03) {
                write_line("^C");
                break;
            }
        } else if (!continuous) {
            /* Fixed count: small delay between packets */
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
    /* Strip an optional http:// prefix. */
    if (url[0]=='h'&&url[1]=='t'&&url[2]=='t'&&url[3]=='p'&&url[4]==':'&&url[5]=='/'&&url[6]=='/') {
        url += 7;
    }
    /* Split into host and path at the first '/'. */
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
    req.timeout_ms = 0;   /* kernel default */

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



static void parse_input(void) {
    char *p = g_input;
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
		cmd_ls(g_argc > 1 ? g_args[1] : "");
	} else if (strcmp(cmd, "ifconfig") == 0 || strcmp(cmd, "ip") == 0) {
		cmd_ifconfig();
	} else if (strcmp(cmd, "ping") == 0) {
		/* Build full arg string from g_args[1..g_argc-1] so flags like -t and -c work. */
		g_ping_arg_buf[0] = '\0';
		for (int i = 1; i < g_argc && i < 16; ++i) {
			if (i > 1) {
				/* Append space separator */
				int len = 0; while (g_ping_arg_buf[len]) ++len;
				if (len + 1 < (int)sizeof(g_ping_arg_buf)) g_ping_arg_buf[len] = ' ';
			}
			/* Append g_args[i] */
			int bi = 0; while (g_ping_arg_buf[bi]) ++bi;
			int ai = 0;
			while (g_args[i][ai] && bi + 1 < (int)sizeof(g_ping_arg_buf)) {
				g_ping_arg_buf[bi++] = g_args[i][ai++];
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
            
            int bg = 0;
            if (g_argc > 1 && strcmp(g_args[g_argc - 1], "&") == 0) {
                bg = 1;
                g_argc--;
            } else if (strcmp(cmd, "uidemo") == 0 || strcmp(cmd, "taskmgr") == 0 ||
                       strcmp(cmd, "browser") == 0 || strcmp(cmd, "dock") == 0 ||
                       strcmp(cmd, "sysinfo") == 0 || strcmp(cmd, "wallpaper") == 0 ||
                       strcmp(cmd, "audiocfg") == 0) {
                bg = 1;
            }
            
            int pid = spawn_with_arguments(spawn_path);
            if (pid <= 0) {
                write_str("spawn: ");
                write_str(spawn_path);
                write_str(": ");
                write_line(strerror(pid));
            } else {
                if (!bg) {
                    waitpid(pid);
                }
            }
        } else {
            spawn_path[0] = '\0';
            strcat(spawn_path, "/bin/");
            strcat(spawn_path, cmd);
            
            int bg = 0;
            if (g_argc > 1 && strcmp(g_args[g_argc - 1], "&") == 0) {
                bg = 1;
                g_argc--;
            } else if (strcmp(cmd, "uidemo") == 0 || strcmp(cmd, "taskmgr") == 0 ||
                       strcmp(cmd, "browser") == 0 || strcmp(cmd, "dock") == 0 ||
                       strcmp(cmd, "sysinfo") == 0 || strcmp(cmd, "wallpaper") == 0 ||
                       strcmp(cmd, "audiocfg") == 0) {
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
            } else {
                if (!bg) {
                    waitpid(pid);
                }
            }
        }
    }
}

void _start(void) {
    ssize_t len;
    struct system_info_snapshot info;
    
    getcwd(g_cwd, sizeof(g_cwd));
    
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

        len = read_stdin(g_input, sizeof(g_input) - 1);
        if (len <= 0) continue;

        while (len > 0 && (g_input[len - 1] == '\n' || g_input[len - 1] == '\r')) {
            len--;
        }
        g_input[len] = 0;

        parse_input();
        execute_command();
    }
}
