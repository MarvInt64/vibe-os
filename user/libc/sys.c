/* sys.c — thin libc wrappers over the raw VibeOS syscalls, plus errno. */
#include <unistd.h>
#include <errno.h>
#include <vibeos.h>
#include <sys/syscall.h>

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
int mkdir(const char *path, int mode) { (void)mode; return (int)ck(__sc1(26, (uint64_t)(size_t)path)); }
void    _exit(int code) { __sc1(SYS_EXIT, (uint64_t)code); for (;;) __sc0(SYS_YIELD); }
void    sched_yield_(void) { __sc0(SYS_YIELD); }

/* ---- vibeos.h ---- */
void vos_yield(void) { __sc0(SYS_YIELD); }
void vos_sleep_ticks(unsigned long ticks) { __sc1(SYS_TIMER_SLEEP, (uint64_t)ticks); }
int vos_system_info(struct vos_system_info *out) { return (int)ck(__sc1(SYS_SYSTEM_INFO, (uint64_t)(size_t)out)); }
int  vos_spawn(const char *path) { return (int)__sc1(SYS_PROCESS_SPAWN, (uint64_t)(size_t)path); }
int  vos_spawn_arg(const char *path, const char *arg) { return (int)__sc2(SYS_PROCESS_SPAWN, (uint64_t)(size_t)path, (uint64_t)(size_t)arg); }
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
    if (stack_size <= 0) stack_size = 64 * 1024;

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

unsigned int sleep(unsigned int seconds) {
    vos_sleep_ticks((unsigned long)seconds * 100);
    return 0;
}

int usleep(useconds_t usec) {
    unsigned long ticks = ((unsigned long)usec + 9999) / 10000;
    if (ticks > 0) {
        vos_sleep_ticks(ticks);
    } else {
        __sc0(SYS_YIELD);
    }
    return 0;
}
