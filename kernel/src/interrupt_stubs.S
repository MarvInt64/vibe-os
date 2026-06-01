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
