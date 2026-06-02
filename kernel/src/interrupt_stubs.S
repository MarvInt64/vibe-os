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

.extern syscall_handle_interrupt
.extern timer_handle_interrupt
.extern fault_handler
.extern g_kernel_resume_rsp
.extern g_kernel_resume_result
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

interrupt_restore_user_context:
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
    mov qword ptr [rip + g_kernel_resume_rsp], rsp
    jmp interrupt_restore_user_context

process_run_slice_return:
    mov rsp, qword ptr [rip + g_kernel_resume_rsp]
    mov rax, qword ptr [rip + g_kernel_resume_result]
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
    mov rsp, qword ptr [rip + g_kernel_resume_rsp] /* back to scheduler stack  */
    mov rax, qword ptr [rip + g_kernel_resume_result]
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
    mov qword ptr [rip + g_kernel_resume_rsp], rsp /* save scheduler stack */
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

    mov rdi, rsp
    call syscall_handle_interrupt
    test eax, eax
    jnz .return_kernel

    mov rdi, rsp
    jmp interrupt_restore_user_context

.return_kernel:
    jmp process_run_slice_return

/* CPU exceptions with an error code (#PF=14, #GP=13). Stack on entry:
 *   [rsp]=error code, [rsp+8]=RIP, [rsp+16]=CS, ...
 * Kernel faults still halt. User faults are converted into a process exit. */
pagefault_stub:
    /* Snapshot the interrupted general-purpose registers into a global scratch
     * area for diagnostics (the fault handler logs them). Done with absolute
     * stores so the existing [rsp]-relative error-code/iret reads are unchanged. */
    mov [rip + g_fault_regs + 0],  rax
    mov [rip + g_fault_regs + 8],  rbx
    mov [rip + g_fault_regs + 16], rcx
    mov [rip + g_fault_regs + 24], rdx
    mov [rip + g_fault_regs + 32], rsi
    mov [rip + g_fault_regs + 40], rdi
    mov [rip + g_fault_regs + 48], rbp
    /* Also snapshot the interrupted RIP and user RSP from the CPU-pushed
     * exception frame so the journal shows a fully consistent fault state
     * (index 7 = rip, 8 = user rsp). rax is already saved, so reuse it. */
    mov rax, [rsp + 8]                  /* faulting RIP  */
    mov [rip + g_fault_regs + 56], rax
    mov rax, [rsp + 32]                 /* interrupted RSP */
    mov [rip + g_fault_regs + 64], rax
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

    mov rdi, rsp
    call timer_handle_interrupt
    test eax, eax
    jnz .timer_return_kernel

    mov rdi, rsp
    jmp interrupt_restore_user_context

.timer_return_kernel:
    jmp process_run_slice_return
