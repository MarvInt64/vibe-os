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

/* sys.c — thin libc wrappers over the raw VibeOS syscalls, plus errno. */
#include <unistd.h>
#include <errno.h>
#include <vibeos.h>
#include <time.h>
#include <sys/time.h>
#include <sys/syscall.h>
#include <sys/stat.h>


int errno = 0;

/* Translate a syscall return: negative => -errno, set errno and return -1. */
static long ck(long r) {
    if (r < 0) { errno = (int)-r; return -1; }
    return r;
}

ssize_t read(int fd, void *buf, size_t n)  { return ck(__sc3(SYS_READ,  (uint64_t)fd, (uint64_t)(size_t)buf, (uint64_t)n)); }
ssize_t write(int fd, const void *buf, size_t n) { return ck(__sc3(SYS_WRITE, (uint64_t)fd, (uint64_t)(size_t)buf, (uint64_t)n)); }
int     close(int fd) { return (int)ck(__sc1(SYS_CLOSE, (uint64_t)fd)); }
off_t   lseek(int fd, off_t offset, int whence) { return (off_t)ck(__sc3(SYS_SEEK, (uint64_t)fd, (uint64_t)(long)offset, (uint64_t)whence)); }
int vos_open_path(const char *path)  { return (int)ck(__sc1(SYS_OPEN,  (uint64_t)(size_t)path)); }
int vos_creat_path(const char *path) { return (int)ck(__sc1(SYS_CREAT, (uint64_t)(size_t)path)); }
int mkdir(const char *path, int mode) { (void)mode; return (int)ck(__sc1(SYS_MKDIR, (uint64_t)(size_t)path)); }
void    _exit(int code) { __sc1(SYS_EXIT, (uint64_t)code); for (;;) __sc0(SYS_YIELD); }
void    sched_yield_(void) { __sc0(SYS_YIELD); }

/* ---- vibeos.h ---- */
void vos_yield(void) { __sc0(SYS_YIELD); }
void vos_sleep_ticks(unsigned long ticks) { __sc1(SYS_TIMER_SLEEP, (uint64_t)ticks); }
void vos_sleep_ms(unsigned int ms)          { __sc1(SYS_SLEEP_MS, (uint64_t)ms); }
int  vos_keymap_set(const char *layout)     { return (int)ck(__sc1(SYS_KEYMAP_SET, (uint64_t)(size_t)layout)); }
int vos_system_info(struct vos_system_info *out) { return (int)ck(__sc1(SYS_SYSTEM_INFO, (uint64_t)(size_t)out)); }
int  vos_spawn(const char *path) { return (int)__sc2(SYS_PROCESS_SPAWN, (uint64_t)(size_t)path, 0); }
int  vos_spawn_arg(const char *path, const char *arg) { return (int)__sc2(SYS_PROCESS_SPAWN, (uint64_t)(size_t)path, (uint64_t)(size_t)arg); }
/* Wait for a spawned child to exit and return ITS exit code. The kernel
 * delivers the child's pid in rax and the exit code in rdx (both for the
 * already-exited fast path and the blocking wake path), so capture rdx —
 * the generic __scN helpers only return rax. */
int  vos_waitpid(int pid) {
#ifdef ARCH_ARM64
    /* aarch64: x8=num, x0=pid; kernel returns child pid in x0, exit code in x1. */
    register long x8 __asm__("x8") = SYS_WAITPID;
    register long x0 __asm__("x0") = pid;
    register long x1 __asm__("x1");
    __asm__ volatile("svc #0" : "+r"(x0), "=r"(x1) : "r"(x8) : "memory");
    (void)x0;
    return (int)x1;           /* exit code */
#else
    long rax, rdx;
    __asm__ volatile("int $0x80"
        : "=a"(rax), "=d"(rdx)
        : "a"((long)SYS_WAITPID), "D"((uint64_t)pid), "S"(0ull), "d"(0ull)
        : "rcx", "r11", "memory");
    (void)rax;                /* rax = reaped child pid */
    return (int)rdx;          /* rdx = child exit code */
#endif
}
int  vos_pty_open(void) { return (int)__sc1(SYS_PTY_OPEN, 0); }
int  vos_spawn_pty(const char *path, int master_fd) { return (int)__sc2(SYS_SPAWN_PTY, (uint64_t)(size_t)path, (uint64_t)master_fd); }
int  vos_pty_interrupt(int master_fd) { return (int)__sc1(SYS_PTY_INTERRUPT, (uint64_t)master_fd); }

int vos_resolve(const char *host, uint32_t *ip_out) {
    return (int)__sc2(SYS_NET_RESOLVE, (uint64_t)(size_t)host, (uint64_t)(size_t)ip_out);
}
int vos_http_get(struct vos_http_req *req) {
    return (int)__sc1(SYS_NET_HTTP_GET, (uint64_t)(size_t)req);
}
int vos_https_get(struct vos_http_req *req) {
    return (int)__sc1(SYS_NET_HTTPS_GET, (uint64_t)(size_t)req);
}

int vos_window_create(const char *title, int w, int h) {
    return (int)__sc3(SYS_WINDOW_CREATE, (uint64_t)(size_t)title, (uint64_t)w, (uint64_t)h);
}
int vos_window_create_ex(const struct vos_window_options *options) {
    return (int)__sc1(SYS_WINDOW_CREATE_EX, (uint64_t)(size_t)options);
}
int vos_window_present(int id, const uint32_t *pixels, int w, int h) {
    return (int)__sc4(SYS_WINDOW_PRESENT, (uint64_t)id, (uint64_t)(size_t)pixels, (uint64_t)w, (uint64_t)h);
}
int vos_set_wallpaper(const uint32_t *pixels, int w, int h) {
    return (int)__sc3(SYS_SET_WALLPAPER, (uint64_t)(size_t)pixels, (uint64_t)w, (uint64_t)h);
}
int vos_window_present_rect(int id, const uint32_t *pixels, int w, int h, int dx, int dy, int dw, int dh) {
    return (int)__sc6(SYS_WINDOW_PRESENT_RECT, (uint64_t)id, (uint64_t)(size_t)pixels,
                      (uint64_t)w, (uint64_t)h,
                      (uint64_t)(((dx & 0xffff) << 16) | (dy & 0xffff)),
                      (uint64_t)(((dw & 0xffff) << 16) | (dh & 0xffff)));
}
int vos_getarg(char *buf, int size) {
    return (int)__sc2(SYS_GETARG, (uint64_t)(size_t)buf, (uint64_t)size);
}
int vos_event_poll(int id, struct vos_event *out) {
    return (int)__sc2(SYS_EVENT_POLL, (uint64_t)id, (uint64_t)(size_t)out);
}
int vos_window_set_menu(int id, const struct vos_menu_item *items, int count) {
    return (int)__sc3(SYS_WINDOW_SET_MENU, (uint64_t)id, (uint64_t)(size_t)items, (uint64_t)count);
}
int vos_window_set_menubar(int id, const struct vos_menubar_item *items, int count) {
    return (int)__sc3(SYS_WINDOW_SET_MENUBAR, (uint64_t)id, (uint64_t)(size_t)items, (uint64_t)count);
}

void vos_log(int level, const char *msg) {
    __sc2(SYS_LOG, (uint64_t)level, (uint64_t)(size_t)msg);
}
unsigned long vos_journal_total(void) {
    return (unsigned long)__sc2(SYS_JOURNAL_READ, 0, 0);
}
int vos_journal_read(unsigned long seq, struct vos_journal_entry *out) {
    return (int)__sc2(SYS_JOURNAL_READ, (uint64_t)seq, (uint64_t)(size_t)out);
}

/* ---- Threads -------------------------------------------------------------
 * A thread shares the caller's address space. The kernel needs an entry point,
 * a stack (allocated here from the shared user heap), and an argument.
 *
 * The kernel jumps straight to the entry with rsp at the stack top and no
 * return address, so a thread function must never `ret`. Every thread is
 * therefore routed through thread_trampoline, which calls the user function
 * and then _exit()s the thread. The per-thread control block + stack are
 * heap-allocated and intentionally leaked on plain exit (workers here are
 * long-lived; a joined thread could reclaim them later). */

extern void *malloc(unsigned long);   /* from stdlib */

struct vos_thread_tcb {
    void (*fn)(void *);
    void  *arg;
};

static void thread_trampoline(void *raw) {
    struct vos_thread_tcb *tcb = (struct vos_thread_tcb *)raw;
    void (*fn)(void *) = tcb->fn;
    void *arg          = tcb->arg;
    fn(arg);
    _exit(0);   /* thread done — kernel reaps this slot */
}

int vos_thread_create(void (*fn)(void *), void *arg, int stack_size) {
    if (fn == 0) { errno = EINVAL; return -1; }
    if (stack_size <= 0) stack_size = 512 * 1024;

    unsigned char *stack = (unsigned char *)malloc((unsigned long)stack_size);
    struct vos_thread_tcb *tcb =
        (struct vos_thread_tcb *)malloc(sizeof(struct vos_thread_tcb));
    if (!stack || !tcb) { errno = ENOMEM; return -1; }
    tcb->fn = fn;
    tcb->arg = arg;

    /* 16-byte aligned stack top (kernel applies the -8 SysV ABI adjustment). */
    unsigned long top = (unsigned long)(stack + stack_size) & ~(unsigned long)0xF;

    long tid = __sc3(SYS_THREAD_CREATE,
                     (uint64_t)(size_t)thread_trampoline,
                     (uint64_t)top,
                     (uint64_t)(size_t)tcb);
    return (int)ck(tid);
}

int vos_thread_join(int tid) {
    int status = 0;
    long r = __sc2(SYS_WAITPID, (uint64_t)tid, (uint64_t)(size_t)&status);
    if (r < 0) { errno = (int)-r; return -1; }
    return status;
}

pid_t getpid(void) {
    return (pid_t)ck(__sc0(SYS_GETPID));
}

pid_t getppid(void) {
    return (pid_t)ck(__sc0(SYS_GETPPID));
}

unsigned int sleep(unsigned int seconds) {
    vos_sleep_ms(seconds * 1000);
    return 0;
}

int usleep(useconds_t usec) {
    unsigned int ms = usec / 1000;
    if (usec > 0 && ms == 0) ms = 1;  /* minimum 1 ms */
    if (ms > 0) {
        vos_sleep_ms(ms);
    } else {
        __sc0(SYS_YIELD);
    }
    return 0;
}

int unlink(const char *path) {
    return (int)ck(__sc1(SYS_UNLINK, (uint64_t)(size_t)path));
}

int rmdir(const char *path) {
    return unlink(path);
}

char *getcwd(char *buf, size_t size) {
    long r = ck(__sc2(SYS_GETCWD, (uint64_t)(size_t)buf, (uint64_t)size));
    if (r < 0) return (char *)0;
    return buf;
}

int chdir(const char *path) {
    return (int)ck(__sc1(SYS_CHDIR, (uint64_t)(size_t)path));
}

time_t time(time_t *tloc) {
    struct vos_system_info info;
    time_t sec = 0;
    if (vos_system_info(&info) == 0) {
        if (info.timer_hz > 0) {
            sec = (time_t)(info.uptime_ticks / info.timer_hz);
        }
    }
    if (tloc) *tloc = sec;
    return sec;
}

int readdir_at(const char *path, int idx, char *name_out, int cap, int *type_out) {
    long ret, kind;
#ifdef ARCH_ARM64
    /* x8=num, x0=path,x1=idx,x2=name_buf,x3=cap → x0=result, x1=kind */
    register long x8 __asm__("x8") = SYS_READDIR;
    register long x0 __asm__("x0") = (long)path;
    register long x1 __asm__("x1") = (long)idx;
    register long x2 __asm__("x2") = (long)name_out;
    register long x3 __asm__("x3") = (long)cap;
    __asm__ volatile("svc #0" : "+r"(x0), "+r"(x1)
                     : "r"(x8), "r"(x2), "r"(x3) : "memory");
    ret = x0; kind = x1;
#else
    /* rdi=path, rsi=index, rdx=name_buf, r10=capacity → rax=result, rdx=kind */
    register long r10 __asm__("r10") = (long)cap;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret), "=d"(kind)
        : "a"((long)SYS_READDIR), "D"((long)path),
          "S"((long)idx), "d"((long)name_out), "r"(r10)
        : "rcx", "r11", "r8", "memory"
    );
#endif
    if (ret <= 0) return (int)ret;
    if (type_out) *type_out = (kind == 2) ? 2 : 1;
    return 1;
}

int stat(const char *path, struct stat *s) {
    long ret, kind;
    /* The kernel returns:
     *   rax = 0 (success) or negative error
     *   rdx = vfs kind (1 = file, 2 = dir)
     *   r8  = byte size
     *   r9  = packed permission data: bits 0-15 = mode, 16-31 = uid, 32-47 = gid */
#ifdef ARCH_ARM64
    /* x8=num, x0=path → x0=result, x1=kind, x2=size, x3=perm-packed */
    register long x8 __asm__("x8") = SYS_STAT;
    register long x0 __asm__("x0") = (long)path;
    register long x1 __asm__("x1");
    register long x2 __asm__("x2");
    register long x3 __asm__("x3");
    __asm__ volatile("svc #0" : "+r"(x0), "=r"(x1), "=r"(x2), "=r"(x3)
                     : "r"(x8) : "memory");
    ret = x0; kind = x1;
    long sz = x2, r9 = x3;
#else
    register long sz  __asm__("r8") = 0;
    register long r9  __asm__("r9") = 0;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret), "=d"(kind), "+r"(sz), "+r"(r9)
        : "a"((long)SYS_STAT), "D"((long)path)
        : "rcx", "r11", "memory"
    );
#endif
    if (ret < 0) return (int)ck(ret);
    if (s) {
        s->st_size = (off_t)sz;
        /* Reconstruct st_mode: file-type bits from vfs kind + permission bits
         * from the lower 16 bits of r9. */
        uint16_t perm = (uint16_t)(r9 & 0xFFFFu);
        uint16_t type = (kind == 1) ? 0x8000u : (kind == 2) ? 0x4000u : 0u;
        s->st_mode = (uint32_t)(type | perm);
        s->st_uid  = (uint16_t)((r9 >> 16) & 0xFFFFu);
        s->st_gid  = (uint16_t)((r9 >> 32) & 0xFFFFu);
    }
    return 0;
}

/* ---- User/group identity ------------------------------------------------ */

uid_t getuid(void) {
    return (uid_t)__sc0(SYS_GETUID);
}

gid_t getgid(void) {
    return (gid_t)__sc0(SYS_GETGID);
}

/*
 * vos_home_dir — resolve the calling user's home directory into buf.
 *
 * Reads /etc/passwd (line format name:passwd:uid:gid:gecos:home:shell) and
 * copies the home field of the entry whose uid matches getuid(). This is how
 * the GUI apps find their working directory instead of hardcoding a path, so
 * the dialog/editor follow whoever the session runs as (root -> /root today,
 * a regular user -> /home/<name> once one exists and the desktop runs as it).
 *
 * Falls back to "/root" for uid 0, "/" otherwise, when the file or a matching
 * entry is missing. Always NUL-terminates (when cap > 0); returns the length.
 */
int vos_home_dir(char *buf, int cap) {
    if (buf == 0 || cap <= 0) return 0;
    buf[0] = '\0';
    unsigned int uid = (unsigned int)getuid();

    int fd = vos_open_path("/etc/passwd");
    if (fd >= 0) {
        char data[1024];
        ssize_t n = read(fd, data, sizeof(data) - 1);
        close(fd);
        if (n > 0) {
            data[n] = '\0';
            char *line = data;
            while (*line) {
                char *nl = line;
                while (*nl && *nl != '\n') nl++;
                char saved = *nl;
                *nl = '\0';

                /* Split the line into up to 7 ':'-separated fields in place. */
                char *f[7];
                int nf = 0;
                f[nf++] = line;
                for (char *p = line; *p && nf < 7; p++) {
                    if (*p == ':') { *p = '\0'; f[nf++] = p + 1; }
                }

                if (nf >= 6) {
                    unsigned int euid = 0;
                    for (char *p = f[2]; *p >= '0' && *p <= '9'; p++)
                        euid = euid * 10u + (unsigned int)(*p - '0');
                    if (euid == uid && f[5][0]) {
                        int k = 0;
                        while (f[5][k] && k < cap - 1) { buf[k] = f[5][k]; k++; }
                        buf[k] = '\0';
                        return k;
                    }
                }

                if (saved == '\0') break;
                line = nl + 1;
            }
        }
    }

    const char *fb = (uid == 0) ? "/root" : "/";
    int k = 0;
    while (fb[k] && k < cap - 1) { buf[k] = fb[k]; k++; }
    buf[k] = '\0';
    return k;
}

/*
 * Change the effective user ID of the current process.
 * Root (uid 0) may change to any uid; other users may only re-set their own.
 */
int setuid(uid_t uid) {
    return (int)ck(__sc1(SYS_SETUID, (uint64_t)uid));
}

/*
 * chmod — change permission bits on a file or directory.
 * 'mode' must be in the range 0–0777 (standard Unix octal permission bits).
 * The caller must be the file owner or root.
 */
int chmod(const char *path, int mode) {
    return (int)ck(__sc2(SYS_CHMOD, (uint64_t)(size_t)path, (uint64_t)(unsigned)mode));
}

/*
 * chown — change the owner user-ID and group-ID of a file.
 * Only root (uid 0) may call this.
 */
int chown(const char *path, unsigned int uid, unsigned int gid) {
    return (int)ck(__sc3(SYS_CHOWN, (uint64_t)(size_t)path,
                         (uint64_t)uid, (uint64_t)gid));
}

/* ---- Desktop status & menu dispatch (top-bar use) ----------------------- */

int vos_desktop_status(struct vos_desktop_status *out) {
    return (int)ck(__sc1(SYS_DESKTOP_STATUS, (uint64_t)(size_t)out));
}

void vos_menu_dispatch(uint32_t action_id) {
    __sc1(SYS_MENU_DISPATCH, (uint64_t)action_id);
}

/* ---- System-wide clipboard ---------------------------------------------- */

/*
 * clipboard_set — write data[0..len) to the shared kernel clipboard.
 *
 * The kernel stores a verbatim copy; no encoding conversion is performed.
 * Returns 0 on success, -1 if len exceeds CLIPBOARD_MAX (64 KB).
 */
int clipboard_set(const char *data, size_t len) {
    if (!data) return -1;
    return (int)ck(__sc2(SYS_CLIPBOARD_SET,
                         (uint64_t)(size_t)data, (uint64_t)len));
}

/*
 * clipboard_get — copy the clipboard into buf[0..cap-1].
 *
 * Always NUL-terminates when cap > 0.  Returns the byte count copied
 * (not counting the NUL), so the caller can detect truncation by
 * comparing the return value against clipboard_len().
 */
size_t clipboard_get(char *buf, size_t cap) {
    if (!buf || cap == 0) return 0;
    return (size_t)__sc2(SYS_CLIPBOARD_GET,
                         (uint64_t)(size_t)buf, (uint64_t)cap);
}

/* clipboard_len — return the current clipboard length in bytes. */
size_t clipboard_len(void) {
    return (size_t)__sc0(SYS_CLIPBOARD_LEN);
}

int gettimeofday(struct timeval *tv, void *tz) {
    (void)tz;
    if (!tv) { errno = EFAULT; return -1; }
    tv->tv_sec = time(0);
    tv->tv_usec = 0;
    return 0;
}

struct tm *localtime_r(const time_t *timep, struct tm *r) {
    if (!timep || !r) return 0;
    time_t t = *timep;
    r->tm_gmtoff = 0;
    r->tm_zone = "UTC";
    r->tm_isdst = 0;

    long long days = t / 86400;
    long long rem = t % 86400;
    if (rem < 0) {
        days--;
        rem += 86400;
    }
    r->tm_hour = rem / 3600;
    r->tm_min = (rem % 3600) / 60;
    r->tm_sec = rem % 60;
    r->tm_wday = (days + 4) % 7;
    if (r->tm_wday < 0) r->tm_wday += 7;

    long long qc_cycles = days / 146097;
    long long qc_rem = days % 146097;
    if (qc_rem < 0) {
        qc_cycles--;
        qc_rem += 146097;
    }
    long long c_cycles = qc_rem / 36524;
    long long c_rem = qc_rem % 36524;
    if (c_cycles == 4) {
        c_cycles = 3;
        c_rem += 36524;
    }
    long long q_cycles = c_rem / 1461;
    long long q_rem = c_rem % 1461;
    long long y_in_q = q_rem / 365;
    long long y_rem = q_rem % 365;
    if (y_in_q == 4) {
        y_in_q = 3;
        y_rem += 365;
    }
    long long year = 1970 + qc_cycles * 400 + c_cycles * 100 + q_cycles * 4 + y_in_q;
    r->tm_year = (int)(year - 1900);
    
    int leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
    r->tm_yday = (int)y_rem;

    static const int mon_lengths[2][12] = {
        { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 },
        { 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 }
    };
    int m = 0;
    while (y_rem >= mon_lengths[leap][m]) {
        y_rem -= mon_lengths[leap][m];
        m++;
    }
    r->tm_mon = m;
    r->tm_mday = (int)(y_rem + 1);
    return r;
}

#include <sys/ioctl.h>
#include <stdarg.h>
int ioctl(int fd, unsigned long request, ...) {
    va_list ap;
    va_start(ap, request);
    uintptr_t arg = va_arg(ap, uintptr_t);
    va_end(ap);
    return (int)__sc3(SYS_IOCTL, (uint64_t)fd, (uint64_t)request, (uint64_t)arg);
}

/* dup2 — duplicate file descriptor (stub) */
int dup2(int oldfd, int newfd) { (void)oldfd; return newfd; }

/* ---- mmap / munmap stubs (for TCC -run support only) ------------------- */
void *mmap(void *addr, unsigned long len, int prot, int flags, int fd, long off) {
    (void)addr; (void)len; (void)prot; (void)flags; (void)fd; (void)off;
    return (void *)0;  /* MAP_FAILED — no mmap on VibeOS */
}
int munmap(void *addr, unsigned long len) {
    (void)addr; (void)len;
    return -1;
}
int mprotect(void *addr, unsigned long len, int prot) {
    (void)addr; (void)len; (void)prot;
    return -1;
}

