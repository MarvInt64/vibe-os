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
/* Open a pseudo-terminal; returns the master fd (>=0) or <0 on failure. Read
 * the master for child output, write it to send child input. */
int  vos_pty_open(void);
/* Spawn `path` with its stdio bound to the slave of the pty whose master fd is
 * `master_fd`. Returns the child pid (>0) or <0 on failure. */
int  vos_spawn_pty(const char *path, int master_fd);
/* Ctrl+C: interrupt (kill) the foreground job running on the given pty master. */
int  vos_pty_interrupt(int master_fd);
void vos_yield(void);
void vos_sleep_ticks(unsigned long ticks);

struct vos_system_info {
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
int vos_system_info(struct vos_system_info *out);

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
    uint64_t boot_id;
    uint32_t hz;
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

/* Top-bar menu bar declared by the focused app. A flat list where each
 * VOS_MB_TITLE item starts a top-level menu (Page/Edit/…) and the items after
 * it (until the next title) are that menu's entries. */
enum {
    VOS_MB_TITLE   = 1u,
    VOS_MB_DIVIDER = 2u,
    VOS_MB_DANGER  = 4u,
    VOS_MB_CHECK   = 8u,
    VOS_MB_CHECKED = 16u,
    VOS_MB_ARROW   = 32u
};
struct vos_menubar_item {
    char label[28];
    char shortcut[16];
    uint32_t flags;
    uint32_t action_id;
};

int vos_window_create(const char *title, int w, int h);   /* >=0 id, <0 = no server */
#define VOS_WINDOW_FRAMELESS     0x00000001u
#define VOS_WINDOW_NO_DOCK       0x00000002u
#define VOS_WINDOW_POSITIONED    0x00000004u
#define VOS_WINDOW_ALWAYS_ON_TOP 0x00000008u
#define VOS_WINDOW_TRANSLUCENT   0x00000010u  /* window background is transparent */
#define VOS_WINDOW_NO_SHADOW     0x00000020u  /* suppress drop shadow */
#define VOS_WINDOW_ASPECT_RATIO  0x00000040u
struct vos_window_options {
    const char *title;
    int32_t width;
    int32_t height;
    uint32_t flags;
    int32_t x;
    int32_t y;
    /* Pixels to offset the drop shadow from the window top (0 = shadow at top). */
    int32_t shadow_inset_top;
};
int vos_window_create_ex(const struct vos_window_options *options);

/* ---- Desktop status (used by the top-bar app) --------------------------- */

/* The pixel value written in transparent areas of a VOS_WINDOW_TRANSLUCENT
 * window — the compositor treats this colour as fully transparent. */
#define VOS_TRANSPARENT_KEY 0x00ff00ffu

/* How many menu-bar items a desktop status snapshot can carry. */
#define VOS_DESKTOP_MENU_MAX 64

/* Snapshot of the desktop state for the top bar:
 * system load, focused app label and its declared menu bar. */
struct vos_desktop_status {
    uint32_t uptime_seconds;
    uint32_t cpu_pct;
    uint32_t ui_pct;
    uint32_t mem_pct;
    uint32_t net_up;                          /* 1 if network has an IP */
    char     app_label[20];                   /* focused window title   */
    uint32_t menu_count;
    struct vos_menubar_item menu[VOS_DESKTOP_MENU_MAX];
};

/* Fill *out with the current desktop snapshot. Returns 0 on success. */
int vos_desktop_status(struct vos_desktop_status *out);

/* Deliver a menu action to the focused window (top-bar use only). */
void vos_menu_dispatch(uint32_t action_id);
int vos_window_present(int id, const uint32_t *pixels, int w, int h);
/* Set the desktop wallpaper from an XRGB buffer (0x00RRGGBB), scaled to screen. */
int vos_set_wallpaper(const uint32_t *pixels, int w, int h);
/* Present only a damaged sub-rect (in content space) of the full canvas. */
int vos_window_present_rect(int id, const uint32_t *pixels, int w, int h, int dx, int dy, int dw, int dh);
/* Copy this process's spawn argument (if any) into buf; returns its length. */
int vos_getarg(char *buf, int size);
int vos_event_poll(int id, struct vos_event *out);         /* 1 = got event, 0 = empty */
int vos_window_set_menu(int id, const struct vos_menu_item *items, int count);
/* Declare the focused-window top-bar menu bar. */
int vos_window_set_menubar(int id, const struct vos_menubar_item *items, int count);

#ifdef __cplusplus
}
#endif

#endif
