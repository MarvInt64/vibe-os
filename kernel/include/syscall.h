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
	SYS_WINDOW_SET_MENUBAR = 37
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
