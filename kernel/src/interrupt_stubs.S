.intel_syntax noprefix

.section .text, "ax"
.code64

.global interrupt_load_runtime_tables
.global interrupt_restore_user_context
.global process_run_slice
.global process_run_slice_return
.global syscall_interrupt_stub
.global timer_interrupt_stub
.global pagefault_stub
.global gpfault_stub
.global spurious_interrupt_stub
.global ap_load_tables
.global ap_timer_stub
.extern process_ap_timer
.extern process_run_slice_return

.extern syscall_handle_interrupt
.extern timer_handle_interrupt
.extern fault_handler
.extern bkl_acquire
.extern bkl_release
/* The scheduler's park/resume slot is per-CPU: it lives in this CPU's struct
 * cpu, reached through the GS base. These offsets must match struct cpu in
 * cpu.h (guarded by _Static_assert in cpu.c). */
.set CPU_RESUME_RSP, 48
.set CPU_RESUME_RESULT, 56
.extern g_fault_regs

interrupt_load_runtime_tables:
    lgdt [rdi]
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    push 0x08
    lea rax, [rip + .reload_done]
    push rax
    retfq
.reload_done:
    lidt [rsi]
    mov ax, dx
    ltr ax
    ret

/* Local APIC spurious-interrupt handler (vector 0xFF). A spurious interrupt
 * does not need an EOI and carries no error code, so we simply return. */
spurious_interrupt_stub:
    iretq

/* Load this CPU's GDT and IDT and reload the segment registers, WITHOUT
 * loading a task register (ltr) — an AP shares the runtime GDT whose single TSS
 * descriptor is already busy on the boot CPU, and a parked/idle AP never makes
 * a privilege transition, so it needs no TSS of its own yet. rdi = gdt
 * descriptor, rsi = idt descriptor. NB: this reloads gs to selector 0x10, which
 * resets the gs base; the caller must (re)program the gs base afterwards. */
ap_load_tables:
    lgdt [rdi]
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    push 0x08
    lea rax, [rip + .ap_tables_reloaded]
    push rax
    retfq
.ap_tables_reloaded:
    lidt [rsi]
    ret

/* Local APIC timer handler for application processors (vector 0x40). Mirrors
 * timer_interrupt_stub: save the full register frame, take the big kernel lock,
 * run process_ap_timer, then either resume the interrupted process or, if it
 * preempted one, unwind to the AP's scheduler loop via process_run_slice_return
 * (the per-CPU resume slot makes this land back in this AP's dispatch call). */
ap_timer_stub:
    push r15
    push r14
    push r13
    push r12
    push r11
    push r10
    push r9
    push r8
    push rsi
    push rdi
    push rbp
    push rdx
    push rcx
    push rbx
    push rax

    call bkl_acquire

    mov rdi, rsp
    call process_ap_timer
    test eax, eax
    jnz .ap_timer_deschedule

    mov rdi, rsp
    jmp interrupt_restore_user_context

.ap_timer_deschedule:
    jmp process_run_slice_return

interrupt_restore_user_context:
    /* Leaving the kernel: drop the big kernel lock. rdi holds the frame pointer
     * we still need, so preserve it across the call. (bkl_release only truly
     * unlocks when this CPU's recursion depth reaches zero.) */
    push rdi
    call bkl_release
    pop rdi
    mov rsp, rdi
    pop rax
    pop rbx
    pop rcx
    pop rdx
    pop rbp
    pop rdi
    pop rsi
    pop r8
    pop r9
    pop r10
    pop r11
    pop r12
    pop r13
    pop r14
    pop r15
    iretq

process_run_slice:
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15
    mov qword ptr gs:[CPU_RESUME_RSP], rsp
    jmp interrupt_restore_user_context

process_run_slice_return:
    mov rsp, qword ptr gs:[CPU_RESUME_RSP]
    mov rax, qword ptr gs:[CPU_RESUME_RESULT]
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

/* ====================================================================== *
 *  Cooperative kernel-thread park / resume (preemptive blocking I/O).
 *
 *  These two routines implement a symmetric context switch between a
 *  process's *private kernel stack* and the scheduler's kernel stack.
 *  They are the mechanism that lets a blocking syscall (e.g. a network
 *  wait) suspend itself mid-execution, hand the CPU back to the scheduler
 *  so the desktop and other processes keep running, and later resume
 *  exactly where it left off.
 *
 *  They mirror the push/pop discipline of process_run_slice /
 *  process_run_slice_return so the two pairs interoperate: whichever of
 *  process_run_slice or process_resume_blocked last entered a process is
 *  the one process_block_current returns into.
 *
 *  Callee-saved set (System V AMD64): rbx, rbp, r12, r13, r14, r15.
 * ====================================================================== */

/* void process_block_current(uintptr_t *save_rsp)
 *   rdi = address to store this process's parked kernel stack pointer.
 *
 * Saves the callee-saved registers on the *current* (process) kernel stack,
 * records rsp into *save_rsp, then unwinds onto the scheduler stack saved in
 * g_kernel_resume_rsp and returns there, delivering g_kernel_resume_result as
 * the run-result code (in rax). Execution continues in the scheduler as if
 * process_run_slice / process_resume_blocked had just returned. */
.global process_block_current
process_block_current:
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15
    mov qword ptr [rdi], rsp                       /* park process kernel rsp */
    mov rsp, qword ptr gs:[CPU_RESUME_RSP] /* back to scheduler stack  */
    mov rax, qword ptr gs:[CPU_RESUME_RESULT]
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

/* int process_resume_blocked(uintptr_t parked_rsp)
 *   rdi = a kernel stack pointer previously parked by process_block_current.
 *
 * Saves the scheduler's callee-saved state into g_kernel_resume_rsp (exactly
 * like process_run_slice does), switches onto the parked process kernel stack
 * and returns into the body of process_block_current — which then returns to
 * its caller (the blocking syscall), resuming it. The eventual return value
 * (when the process next blocks or leaves) is delivered through rax. */
.global process_resume_blocked
process_resume_blocked:
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15
    mov qword ptr gs:[CPU_RESUME_RSP], rsp /* save scheduler stack */
    mov rsp, rdi                                   /* load parked process stack */
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

syscall_interrupt_stub:
    push r15
    push r14
    push r13
    push r12
    push r11
    push r10
    push r9
    push r8
    push rsi
    push rdi
    push rbp
    push rdx
    push rcx
    push rbx
    push rax

    /* Entering the kernel from a syscall: take the big kernel lock. All GP regs
     * are already saved on the stack, so the call clobbering caller-saved
     * registers is harmless. */
    call bkl_acquire

    mov rdi, rsp
    call syscall_handle_interrupt
    test eax, eax
    jnz .return_kernel

    mov rdi, rsp
    jmp interrupt_restore_user_context

.return_kernel:
    jmp process_run_slice_return

/* CPU exceptions with an error code (#PF=14, #GP=13). Stack on entry:
 *   [rsp+0]=error code, [rsp+8]=RIP, [rsp+16]=CS,
 *   [rsp+24]=RFLAGS,    [rsp+32]=user RSP, [rsp+40]=SS
 * Kernel faults still halt. User faults are converted into a process exit. */
pagefault_stub:
    /* Save all 16 GP registers + RFLAGS + fault RIP + user RSP into g_fault_regs.
     * Layout (indices 0-17):
     *   0=rax 1=rbx 2=rcx 3=rdx 4=rsi 5=rdi 6=rbp
     *   7=r8  8=r9  9=r10 10=r11 11=r12 12=r13 13=r14 14=r15
     *   15=rflags  16=fault_rip  17=fault_rsp
     * rax is saved first so it can be reused as scratch to read the exception frame. */
    mov [rip + g_fault_regs +   0], rax
    mov [rip + g_fault_regs +   8], rbx
    mov [rip + g_fault_regs +  16], rcx
    mov [rip + g_fault_regs +  24], rdx
    mov [rip + g_fault_regs +  32], rsi
    mov [rip + g_fault_regs +  40], rdi
    mov [rip + g_fault_regs +  48], rbp
    mov [rip + g_fault_regs +  56], r8
    mov [rip + g_fault_regs +  64], r9
    mov [rip + g_fault_regs +  72], r10
    mov [rip + g_fault_regs +  80], r11
    mov [rip + g_fault_regs +  88], r12
    mov [rip + g_fault_regs +  96], r13
    mov [rip + g_fault_regs + 104], r14
    mov [rip + g_fault_regs + 112], r15
    mov rax, [rsp + 24]                  /* RFLAGS from exception frame */
    mov [rip + g_fault_regs + 120], rax
    mov rax, [rsp + 8]                   /* faulting RIP from exception frame */
    mov [rip + g_fault_regs + 128], rax
    mov rax, [rsp + 32]                  /* user RSP from exception frame */
    mov [rip + g_fault_regs + 136], rax
    /* Set up fault_handler(cr2, error_code, rip, vector, cs) and call it. */
    mov rsi, [rsp]      /* error code */
    mov rdx, [rsp + 8]  /* faulting RIP */
    mov r8, [rsp + 16]  /* CS */
    mov rdi, cr2        /* faulting address */
    mov rcx, 14
    call fault_handler
    test eax, eax
    jnz process_run_slice_return
.pf_hang:
    cli
    hlt
    jmp .pf_hang

gpfault_stub:
    /* Identical register save as pagefault_stub — same exception frame layout. */
    mov [rip + g_fault_regs +   0], rax
    mov [rip + g_fault_regs +   8], rbx
    mov [rip + g_fault_regs +  16], rcx
    mov [rip + g_fault_regs +  24], rdx
    mov [rip + g_fault_regs +  32], rsi
    mov [rip + g_fault_regs +  40], rdi
    mov [rip + g_fault_regs +  48], rbp
    mov [rip + g_fault_regs +  56], r8
    mov [rip + g_fault_regs +  64], r9
    mov [rip + g_fault_regs +  72], r10
    mov [rip + g_fault_regs +  80], r11
    mov [rip + g_fault_regs +  88], r12
    mov [rip + g_fault_regs +  96], r13
    mov [rip + g_fault_regs + 104], r14
    mov [rip + g_fault_regs + 112], r15
    mov rax, [rsp + 24]                  /* RFLAGS from exception frame */
    mov [rip + g_fault_regs + 120], rax
    mov rax, [rsp + 8]                   /* faulting RIP from exception frame */
    mov [rip + g_fault_regs + 128], rax
    mov rax, [rsp + 32]                  /* user RSP from exception frame */
    mov [rip + g_fault_regs + 136], rax
    mov rsi, [rsp]
    mov rdx, [rsp + 8]
    mov r8, [rsp + 16]
    xor rdi, rdi
    mov rcx, 13
    call fault_handler
    test eax, eax
    jnz process_run_slice_return
.gp_hang:
    cli
    hlt
    jmp .gp_hang

timer_interrupt_stub:
    push r15
    push r14
    push r13
    push r12
    push r11
    push r10
    push r9
    push r8
    push rsi
    push rdi
    push rbp
    push rdx
    push rcx
    push rbx
    push rax

    /* Entering the kernel from a timer IRQ: take the big kernel lock. If this
     * CPU already holds it (the timer fired in kernel context) bkl_acquire just
     * deepens the recursion, so this never self-deadlocks. */
    call bkl_acquire

    mov rdi, rsp
    call timer_handle_interrupt
    test eax, eax
    jnz .timer_return_kernel

    mov rdi, rsp
    jmp interrupt_restore_user_context

.timer_return_kernel:
    jmp process_run_slice_return
