#ifndef VIBEOS_SYS_UCONTEXT_H
#define VIBEOS_SYS_UCONTEXT_H

#include <signal.h>

/* Minimal stubs for TCC backtrace compilation — never used at runtime.
 * VibeOS has no signal delivery, so these types exist only to satisfy
 * the compiler when building tccrun.c. */

/* x86_64 general-purpose register context */
typedef struct mcontext_t {
    unsigned long gregs[32];
} mcontext_t;

/* User context (simplified for TCC). */
typedef struct ucontext_t {
    unsigned long    uc_flags;
    struct ucontext_t *uc_link;
    mcontext_t       uc_mcontext;
    sigset_t         uc_sigmask;
    unsigned long    __pad[16];
} ucontext_t;

/* Register indices (matching Linux gregs layout for x86_64). */
#define REG_RIP  16
#define REG_RBP   6
#define REG_RSP  15

#endif
