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
    SYS_SEEK = 48
};

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

#endif
