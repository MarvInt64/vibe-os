#ifndef VIBEOS_SYSCALL_H
#define VIBEOS_SYSCALL_H

#include "fd.h"
#include "types.h"

enum syscall_number {
	SYS_READ = 0,
	SYS_WRITE = 1,
	SYS_IOCTL = 2,
	SYS_YIELD = 3,
	SYS_EXIT = 4,
	SYS_PROCESS_SPAWN = 5,
	SYS_WAITPID = 6,
	SYS_OPEN = 7,
	SYS_CLOSE = 8,
	SYS_STAT = 9,
	SYS_READDIR = 10,
	SYS_CHDIR = 11,
	SYS_GETCWD = 12,
	SYS_UNLINK = 13,
	SYS_CREAT = 14,
	SYS_GETARG = 15,
	SYS_WRITE_FILE = 16,
	SYS_WINDOW_CREATE = 17,
	SYS_WINDOW_PRESENT = 18,
	SYS_EVENT_POLL = 19,
	SYS_TIMER_SLEEP = 20,
	SYS_WINDOWMGR_START = 21,
	SYS_NET_INFO = 22,
	SYS_NET_PING = 23,
	SYS_NET_RESOLVE = 24,
	SYS_NET_HTTP_GET = 25,
	SYS_MKDIR = 26,
	SYS_DISPLAY_MODE = 27,
	SYS_PROCESS_SNAPSHOT = 28,
	SYS_PROCESS_KILL = 29,
	SYS_WINDOW_SET_MENU = 30,
	SYS_NET_HTTPS_GET = 31,
	SYS_JOURNAL_READ = 32,
	SYS_LOG = 33,
	/* Create a thread sharing the caller's address space.
	 * rdi = entry function (user ptr), rsi = stack top (user ptr, owned by
	 * caller, e.g. malloc'd), rdx = argument passed to the entry in rdi.
	 * Returns the new thread's tid (a pid), or <0 on error. */
	SYS_THREAD_CREATE = 34,
	SYS_WINDOW_CREATE_EX = 35,
	/* Set the desktop wallpaper from a userspace XRGB pixel buffer.
	 * rdi = pixels (user ptr, 0x00RRGGBB), rsi = width, rdx = height.
	 * The kernel scales it to the screen and uses it as the backdrop. */
	SYS_SET_WALLPAPER = 36,
	/* Set the focused-window top-bar menu bar.
	 * rdi = win id, rsi = struct winsys_menubar_item* (user), rdx = count. */
	SYS_WINDOW_SET_MENUBAR = 37,
	/* Present only a damaged sub-rectangle of the window's canvas.
	 * rdi = win id, rsi = pixels, rdx = full w, r10 = full h,
	 * r8 = (dx<<16)|dy, r9 = (dw<<16)|dh. */
	SYS_WINDOW_PRESENT_RECT = 38,
	SYS_SYSTEM_INFO = 39,
	/* Rasterize a string into a user-supplied ARGB buffer using the kernel's
	 * anti-aliased TrueType atlas. rdi = buf, rsi = text, rdx = (buf_w<<16)|buf_h,
	 * r10 = ((x&0xffff)<<16)|(y&0xffff) (x,y signed 16-bit), r8 = color, r9 = scale
	 * (1..3). Returns the proportional advance width drawn. */
	SYS_TEXT_DRAW = 40,
	/* Query atlas metrics. rdi = text (or 0), rsi = scale (1..3). With text != 0
	 * returns the proportional pixel width of the string; with text == 0 returns
	 * packed font metrics: lineh | (ascent<<8) | (cellw<<16) | (space<<24). */
	SYS_TEXT_METRICS = 41,
	/* Grow/query the process heap (brk/sbrk style). rdi = signed byte increment
	 * (0 = query). Maps fresh physical pages into the process's heap region on
	 * demand and returns the PREVIOUS break (start of the newly available bytes),
	 * or (void*)-1 on failure. This replaces baking a fixed heap arena into every
	 * binary's BSS — heap memory now grows lazily like a real OS. */
	SYS_SBRK = 42,
	/* Fill a struct winsys_desktop_status (user ptr in rdi) with the data the
	 * top-bar app needs: uptime, CPU/UI/MEM load, focused app label and its
	 * menu bar. Returns 0 on success. */
	SYS_DESKTOP_STATUS = 43,
	/* Deliver a menu action (rdi = action_id) to the focused window as a
	 * WINSYS_EVENT_MENU_ACTION, exactly as the old kernel top bar did. */
	SYS_MENU_DISPATCH = 44,
	/* Allocate a pseudo-terminal and bind its master endpoint to a fresh fd in
	 * the caller's fd table. Returns the master fd (>=0) or <0 on failure. */
	SYS_PTY_OPEN = 45,
	/* Spawn a program (rdi = path) with its stdin/stdout/stderr bound to the
	 * slave end of the pty whose master fd is in rsi. Returns child pid. */
	SYS_SPAWN_PTY = 46,
	/* Ctrl+C: interrupt the foreground job of the pty whose master fd is rdi. */
	SYS_PTY_INTERRUPT = 47,
	/* Seek a VFS fd. rdi=fd, rsi=offset, rdx=whence (0=SET,1=CUR,2=END).
	 * Returns new absolute offset or <0 on error. */
	SYS_SEEK = 48,
	/* Feed raw PCM to the AC97 ring buffer.
	 * rdi = buf (user ptr), rsi = byte count.
	 * Returns bytes accepted or <0 on error. */
	SYS_AUDIO_WRITE = 49,
	/* Fill a struct audio_info (user ptr in rdi) with driver state.
	 * Returns 0 on success or <0 on error. */
	SYS_AUDIO_INFO = 50,
	SYS_REBOOT = 51,
	SYS_SHUTDOWN = 52,
	SYS_GETPID = 53,
	SYS_GETPPID = 54,
	/* User/permission syscalls.
	 * SYS_GETUID / SYS_GETGID: return effective uid/gid of the caller.
	 * SYS_SETUID: rdi = new uid; root (uid 0) may set any uid, others only
	 *             their own.  Returns 0 or -EPERM.
	 * SYS_CHMOD:  rdi = path (user str), rsi = mode (rwx bits, 0–0777).
	 * SYS_CHOWN:  rdi = path, rsi = new uid, rdx = new gid. */
	SYS_GETUID = 55,
	SYS_GETGID = 56,
	SYS_SETUID = 57,
	SYS_CHMOD  = 58,
	SYS_CHOWN  = 59,
	/* AC97 audio device control (rate/volume/buffer).
	 * rdi = request code (AUDIO_IOCTL_*), rsi = user ptr to uint32_t value. */
	SYS_AUDIO_IOCTL = 60,
	/* System-wide text clipboard accessible from any process.
	 * SET: rdi = data ptr (user), rsi = byte count.  Returns 0 or -EINVAL.
	 * GET: rdi = buf ptr (user),  rsi = buf capacity.  Returns bytes copied.
	 * LEN: no args.  Returns current clipboard length in bytes. */
	SYS_CLIPBOARD_SET = 61,
	SYS_CLIPBOARD_GET = 62,
	SYS_CLIPBOARD_LEN = 63,
	/* Query per-CPU information (SMP). rdi = struct cpu_info_snapshot* buf (user),
	 * rsi = max entries (capacity). Fills buf with up to max entries and returns
	 * the number of CPUs written (<= max), or <0 on failure. */
	SYS_CPU_INFO = 64,
	/* rdi = struct vos_fb_info* → fills addr/width/height/stride for a linear
	 * framebuffer the caller can draw into directly. 0 on success, <0 if no fb. */
	SYS_FB_INFO = 65
};

struct system_info_snapshot {
    uint64_t uptime_ticks;
    uint32_t timer_hz;
    uint32_t process_count;
    uint32_t process_max;
    uint32_t app_window_max;
    uint32_t cpu_count;
    uint64_t heap_used_bytes;
    uint64_t heap_total_bytes;
    char version[16];
    char build[32];
};

/* Per-CPU snapshot returned by SYS_CPU_INFO. */
struct cpu_info_snapshot {
    uint32_t index;      /* 0 = BSP, 1..N = APs                */
    uint32_t apic_id;    /* Local APIC ID                      */
    uint64_t ticks;      /* total local timer ticks on this CPU  */
    uint64_t busy;       /* ... that interrupted a user process  */
};

enum syscall_error {
    SYSCALL_OK = 0,
    SYSCALL_EPERM = 1,
    SYSCALL_ENOENT = 2,
    SYSCALL_EIO = 5,
    SYSCALL_EBADF = 9,
    SYSCALL_ENOMEM = 12,
    SYSCALL_EACCES = 13,
    SYSCALL_EFAULT = 14,
    SYSCALL_EEXIST = 17,
    SYSCALL_ENOTDIR = 20,
    SYSCALL_EISDIR = 21,
    SYSCALL_EINVAL = 22,
    SYSCALL_EMFILE = 24,
    SYSCALL_EFBIG = 27,
    SYSCALL_ENOSPC = 28,
    SYSCALL_EROFS = 30,
    SYSCALL_ENOSYS = 38,
    SYSCALL_ENAMETOOLONG = 36
};

struct syscall_context {
    struct fd_table *fd_table;
};

int64_t syscall_dispatch(struct syscall_context *context, uint64_t number, uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);

#endif
