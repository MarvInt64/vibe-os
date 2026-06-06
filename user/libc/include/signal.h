#ifndef VIBEOS_SIGNAL_H
#define VIBEOS_SIGNAL_H
#include <sys/types.h>
typedef void (*sighandler_t)(int);
typedef int sig_atomic_t;
typedef unsigned int sigset_t;
struct sigaction { void (*sa_handler)(int); void (*sa_sigaction)(int,void*,void*); sigset_t sa_mask; int sa_flags; };
#define SIGINT 2
#define SIGTERM 15
#define SIGWINCH 28
#define SIGCONT 18
#define SIGTSTP 20
#define SIGSEGV 11
#define SIGABRT 6
#define SIGFPE  8
#define SIGBUS  10
#define SIGILL  4
#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)
#define SIG_BLOCK 0
#define SIG_UNBLOCK 1
#define SA_RESETHAND 0x02000000

/* siginfo_t — minimal stub for TCC backtrace support */
typedef struct { int si_signo; int si_code; void *si_addr; } siginfo_t;

/* FPE_* codes */
#define FPE_INTDIV 1
#define FPE_FLTDIV 3
static inline sighandler_t signal(int s, sighandler_t h) { (void)s;(void)h; return SIG_DFL; }
static inline int sigaction(int s, const struct sigaction *a, struct sigaction *o) { (void)s;(void)a;(void)o; return 0; }
static inline int sigfillset(sigset_t *s) { (void)s; return 0; }
static inline int sigemptyset(sigset_t *s) { (void)s; return 0; }
static inline int sigaddset(sigset_t *s, int sig) { (void)s;(void)sig; return 0; }
static inline int sigprocmask(int h, const sigset_t *s, sigset_t *o) { (void)h;(void)s;(void)o; return 0; }
#endif
