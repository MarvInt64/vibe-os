/* VibeOS — arch-neutral syscall frame access.
 *
 * The x86_64 kernel uses `struct interrupt_frame` with named members
 * (rax, rdi, rsi, rdx, r8, r9, r10) while the arm64 kernel uses a
 * `uint64_t regs[31]` array from the exception handler.  These macros
 * let the shared kernel code (process.c, window.c) access syscall
 * arguments and return values without arch-specific register names.
 *
 * Syscall ABI (both arches share the same SYS_* numbers):
 *   x86_64: num=rax, args: rdi rsi rdx r10 r8 r9, ret=rax
 *   arm64:  num=x8,  args: x0  x1  x2  x3  x4 x5,  ret=x0
 *
 * Usage:
 *   uint64_t num = SCF_NUM(frame);
 *   uint64_t arg0 = SCF_ARG0(frame);
 *   SCF_SET_RET(frame, result);
 */
#ifndef VIBEOS_ARCH_FRAME_H
#define VIBEOS_ARCH_FRAME_H

#include <stdint.h>

#ifdef ARCH_ARM64

/* arm64: the exception handler saves regs[0..30] on the stack */
struct interrupt_frame {
    uint64_t regs[31];   /* x0–x30 */
    uint64_t elr_el1;    /* exception link register (user pc) */
    uint64_t spsr_el1;   /* saved processor state */
    uint64_t sp_el0;     /* user stack pointer */
    uint64_t esr_el1;    /* exception syndrome */
    uint64_t far_el1;    /* fault address */
};

#define SCF_ARG0(f)  ((f)->regs[0])
#define SCF_ARG1(f)  ((f)->regs[1])
#define SCF_ARG2(f)  ((f)->regs[2])
#define SCF_ARG3(f)  ((f)->regs[3])
#define SCF_ARG4(f)  ((f)->regs[4])
#define SCF_ARG5(f)  ((f)->regs[5])
#define SCF_NUM(f)   ((f)->regs[8])
#define SCF_RET(f)   ((f)->regs[0])
#define SCF_SET_RET(f, v) do { (f)->regs[0] = (uint64_t)(v); } while (0)

/* PC at time of exception (for fault logging) */
#define SCF_PC(f)    ((f)->elr_el1)
/* Fault address (for page fault reporting) */
#define SCF_FAR(f)   ((f)->far_el1)
/* Managed user stack pointer */
#define SCF_USER_SP(f) ((f)->sp_el0)

#else /* ARCH_X86_64 */

struct interrupt_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error_code;
    uint64_t rip, cs, rflags, rsp, ss;
};

#define SCF_ARG0(f)  ((f)->rdi)
#define SCF_ARG1(f)  ((f)->rsi)
#define SCF_ARG2(f)  ((f)->rdx)
#define SCF_ARG3(f)  ((f)->r10)
#define SCF_ARG4(f)  ((f)->r8)
#define SCF_ARG5(f)  ((f)->r9)
#define SCF_NUM(f)   ((f)->rax)
#define SCF_RET(f)   ((f)->rax)
#define SCF_SET_RET(f, v) do { (f)->rax = (uint64_t)(v); } while (0)

#define SCF_PC(f)    ((f)->rip)
#define SCF_FAR(f)   (0)   /* x86 passes cr2 separately */
#define SCF_USER_SP(f) ((f)->rsp)

#endif /* ARCH_X86_64 */

#endif /* VIBEOS_ARCH_FRAME_H */
