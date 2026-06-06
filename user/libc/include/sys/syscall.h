/* VibeOS raw syscall layer — the single source of truth for syscall numbers
 * and the int 0x80 trampolines. Every libc and app builds on this instead of
 * hand-rolling inline asm (which used to be copy-pasted into every app).
 *
 * ABI: rax = number, args in rdi, rsi, rdx, r10, r8, r9 (Linux-style); the
 * kernel reads exactly those registers in syscall_handle_interrupt. */
#ifndef VIBEOS_SYS_SYSCALL_H
#define VIBEOS_SYS_SYSCALL_H

#include <stdint.h>

enum {
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
    SYS_THREAD_CREATE = 34,
    SYS_WINDOW_CREATE_EX = 35,
    SYS_SET_WALLPAPER = 36,
    SYS_WINDOW_SET_MENUBAR = 37,
    SYS_WINDOW_PRESENT_RECT = 38,
    SYS_SYSTEM_INFO = 39,
    SYS_TEXT_DRAW = 40,
    SYS_TEXT_METRICS = 41,
    SYS_SBRK = 42,
    SYS_PTY_OPEN = 45,
    SYS_SPAWN_PTY = 46,
    SYS_PTY_INTERRUPT = 47,
    SYS_SEEK = 48,
    SYS_AUDIO_WRITE = 49,
    SYS_AUDIO_INFO = 50,
    SYS_REBOOT = 51,
    SYS_SHUTDOWN = 52,
    SYS_GETPID  = 53,
    SYS_GETPPID = 54,
    SYS_GETUID  = 55,  /* returns effective uid of caller */
    SYS_GETGID  = 56,  /* returns effective gid of caller */
    SYS_SETUID  = 57,  /* rdi = new uid; root can set any, others only own */
    SYS_CHMOD   = 58,  /* rdi = path, rsi = mode (0–0777) */
    SYS_CHOWN       = 59,  /* rdi = path, rsi = uid, rdx = gid; root only */
    SYS_DESKTOP_STATUS = 43,  /* rdi = struct vos_desktop_status* → 0 on success */
    SYS_MENU_DISPATCH  = 44,  /* rdi = action_id → delivers to focused window */
    SYS_AUDIO_IOCTL    = 60,  /* rdi = request, rsi = &uint32_t value */
    /* System-wide clipboard.
     * SET: rdi = data ptr, rsi = len   → 0 on success / -EINVAL if too large
     * GET: rdi = buf ptr,  rsi = cap   → bytes copied (buf is NUL-terminated)
     * LEN: (no args)                   → current clipboard length */
    SYS_CLIPBOARD_SET = 61,
    SYS_CLIPBOARD_GET = 62,
    SYS_CLIPBOARD_LEN = 63,
    SYS_CPU_INFO      = 64,
    SYS_FB_INFO       = 65,  /* rdi = struct vos_fb_info* → linear framebuffer */
    SYS_INPUT_POLL    = 66   /* rdi = struct vos_input_state* → mouse state */
};

#ifdef ARCH_ARM64
/* ---- aarch64 trampolines ---------------------------------------------- *
 * Same syscall numbers and argument order as x86_64, so all libc and app code
 * is identical across arches — only this thin asm layer differs. AArch64 Linux
 * convention: x8 = number, x0..x5 = args, return value in x0, `svc #0` traps. */
static inline long __sc0(long n) {
    register long x8 __asm__("x8") = n; register long x0 __asm__("x0");
    __asm__ volatile("svc #0":"=r"(x0):"r"(x8):"memory"); return x0;
}
static inline long __sc1(long n, uint64_t a0) {
    register long x8 __asm__("x8")=n; register long x0 __asm__("x0")=(long)a0;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8):"memory"); return x0;
}
static inline long __sc2(long n, uint64_t a0, uint64_t a1) {
    register long x8 __asm__("x8")=n; register long x0 __asm__("x0")=(long)a0;
    register long x1 __asm__("x1")=(long)a1;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1):"memory"); return x0;
}
static inline long __sc3(long n, uint64_t a0, uint64_t a1, uint64_t a2) {
    register long x8 __asm__("x8")=n; register long x0 __asm__("x0")=(long)a0;
    register long x1 __asm__("x1")=(long)a1; register long x2 __asm__("x2")=(long)a2;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2):"memory"); return x0;
}
static inline long __sc4(long n, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    register long x8 __asm__("x8")=n; register long x0 __asm__("x0")=(long)a0;
    register long x1 __asm__("x1")=(long)a1; register long x2 __asm__("x2")=(long)a2;
    register long x3 __asm__("x3")=(long)a3;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3):"memory"); return x0;
}
static inline long __sc5(long n, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    register long x8 __asm__("x8")=n; register long x0 __asm__("x0")=(long)a0;
    register long x1 __asm__("x1")=(long)a1; register long x2 __asm__("x2")=(long)a2;
    register long x3 __asm__("x3")=(long)a3; register long x4 __asm__("x4")=(long)a4;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4):"memory"); return x0;
}
static inline long __sc6(long n, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    register long x8 __asm__("x8")=n; register long x0 __asm__("x0")=(long)a0;
    register long x1 __asm__("x1")=(long)a1; register long x2 __asm__("x2")=(long)a2;
    register long x3 __asm__("x3")=(long)a3; register long x4 __asm__("x4")=(long)a4;
    register long x5 __asm__("x5")=(long)a5;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory"); return x0;
}
#else
/* Unused argument registers are explicitly zeroed: the kernel inspects rdi/rsi/
 * rdx for several syscalls (e.g. SYS_PROCESS_SPAWN reads rsi as an optional arg
 * pointer), so a stale caller value left in an unset arg register would be
 * misread as a real argument. Pin them to 0. */
static inline long __sc0(long n) {
    long r; __asm__ volatile("int $0x80":"=a"(r):"a"(n),"D"(0ull),"S"(0ull),"d"(0ull):"rcx","r11","memory"); return r;
}
static inline long __sc1(long n, uint64_t a0) {
    long r; __asm__ volatile("int $0x80":"=a"(r):"a"(n),"D"(a0),"S"(0ull),"d"(0ull):"rcx","r11","memory"); return r;
}
static inline long __sc2(long n, uint64_t a0, uint64_t a1) {
    long r; __asm__ volatile("int $0x80":"=a"(r):"a"(n),"D"(a0),"S"(a1),"d"(0ull):"rcx","r11","memory"); return r;
}
static inline long __sc3(long n, uint64_t a0, uint64_t a1, uint64_t a2) {
    long r; __asm__ volatile("int $0x80":"=a"(r):"a"(n),"D"(a0),"S"(a1),"d"(a2):"rcx","r11","memory"); return r;
}
static inline long __sc4(long n, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    long r; register uint64_t r10 __asm__("r10") = a3;
    __asm__ volatile("int $0x80":"=a"(r):"a"(n),"D"(a0),"S"(a1),"d"(a2),"r"(r10):"rcx","r11","memory"); return r;
}
static inline long __sc5(long n, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    long r; register uint64_t r10 __asm__("r10") = a3; register uint64_t r8 __asm__("r8") = a4;
    __asm__ volatile("int $0x80":"=a"(r):"a"(n),"D"(a0),"S"(a1),"d"(a2),"r"(r10),"r"(r8):"rcx","r11","memory"); return r;
}
static inline long __sc6(long n, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    long r; register uint64_t r10 __asm__("r10") = a3; register uint64_t r8 __asm__("r8") = a4; register uint64_t r9 __asm__("r9") = a5;
    __asm__ volatile("int $0x80":"=a"(r):"a"(n),"D"(a0),"S"(a1),"d"(a2),"r"(r10),"r"(r8),"r"(r9):"rcx","r11","memory"); return r;
}
#endif /* ARCH_ARM64 */

#endif /* VIBEOS_SYS_SYSCALL_H */
