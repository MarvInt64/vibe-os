#ifndef VIBEOS_INTERRUPTS_H
#define VIBEOS_INTERRUPTS_H

#include "types.h"

#define KERNEL_CODE_SELECTOR 0x08u
#define KERNEL_DATA_SELECTOR 0x10u
#define USER_DATA_SELECTOR 0x18u
#define USER_CODE_SELECTOR 0x20u
#define TSS_SELECTOR 0x28u

struct interrupt_frame {
    uint64_t rax;
    uint64_t rbx;
    uint64_t rcx;
    uint64_t rdx;
    uint64_t rbp;
    uint64_t rdi;
    uint64_t rsi;
    uint64_t r8;
    uint64_t r9;
    uint64_t r10;
    uint64_t r11;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
};

void interrupts_init(void);
void interrupt_restore_user_context(const struct interrupt_frame *frame);
int process_run_slice(const struct interrupt_frame *frame);

#endif
