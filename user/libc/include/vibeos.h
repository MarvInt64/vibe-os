/* vibeos.h — VibeOS-specific syscall surface for apps (networking, the window
 * server, process control). Generic C runtime lives in the standard headers;
 * this is the OS-specific part that has no POSIX equivalent. */
#ifndef VIBEOS_VIBEOS_H
#define VIBEOS_VIBEOS_H
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- process ---- */
int  vos_spawn(const char *path);
int  vos_spawn_arg(const char *path, const char *arg);
void vos_yield(void);
void vos_sleep_ticks(unsigned long ticks);

/* ---- threads ----
 * Threads share the caller's address space. fn runs as fn(arg) on a freshly
 * allocated stack (stack_size bytes, <=0 => 64 KB default). Returns a thread
 * id (>0) or -1. Join blocks until the thread finishes and returns its exit
 * code. A thread function may simply return — the runtime exits it cleanly. */
int  vos_thread_create(void (*fn)(void *), void *arg, int stack_size);
int  vos_thread_join(int tid);

/* ---- kernel event journal ---- */
enum { VOS_LOG_INFO = 0, VOS_LOG_WARN = 1, VOS_LOG_ERROR = 2, VOS_LOG_FAULT = 3, VOS_LOG_APP = 4 };
struct vos_journal_entry {
    uint64_t seq;
    uint64_t tick;
    uint32_t level;
    uint32_t pid;
    char msg[96];
};
/* Append a line to the kernel journal (and the controlling terminal sees it via
 * normal stdout separately). */
void vos_log(int level, const char *msg);
/* Total records ever logged (the seq just past the newest). */
unsigned long vos_journal_total(void);
/* Read record `seq` into *out. Returns 1 if present, 0 if scrolled out/absent. */
int vos_journal_read(unsigned long seq, struct vos_journal_entry *out);

/* ---- networking ---- */
struct vos_http_req {
    uint32_t ip;
    uint16_t port;
    const char *host;
    const char *path;
    char *out;
    int cap;
    const char *user_agent;   /* User-Agent header value (0 to omit) */
    int timeout_ms;           /* request timeout (0 = kernel default) */
};
/* Resolve a hostname to an IPv4 address (host byte order). 0 on success. */
int vos_resolve(const char *host, uint32_t *ip_out);
/* HTTP/1.0 GET. Returns bytes written to req->out, or <0 on failure. */
int vos_http_get(struct vos_http_req *req);
/* HTTPS GET over TLS (BearSSL in the kernel). v1 does NOT validate
 * certificates (accept-all): encrypted but MITM-able. Same return contract. */
int vos_https_get(struct vos_http_req *req);

/* ---- window server (see winsys ABI) ---- */
enum {
    VOS_EV_NONE = 0, VOS_EV_MOUSE_MOVE = 1, VOS_EV_MOUSE_DOWN = 2,
    VOS_EV_MOUSE_UP = 3, VOS_EV_KEY = 4, VOS_EV_CLOSE = 5,
    VOS_EV_CONTEXT_MENU = 6, VOS_EV_MENU_ACTION = 7, VOS_EV_SCROLL = 8,
    VOS_EV_RESIZE = 9
};
struct vos_event { uint32_t type; int32_t x; int32_t y; uint32_t buttons; uint32_t key; };
struct vos_menu_item { char label[24]; uint32_t action_id; };

int vos_window_create(const char *title, int w, int h);   /* >=0 id, <0 = no server */
int vos_window_present(int id, const uint32_t *pixels, int w, int h);
int vos_event_poll(int id, struct vos_event *out);         /* 1 = got event, 0 = empty */
int vos_window_set_menu(int id, const struct vos_menu_item *items, int count);

#ifdef __cplusplus
}
#endif

#endif
