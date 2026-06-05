/*
 * AP (application processor) boot trampoline.
 *
 * An AP starts in 16-bit real mode at physical (SIPI_vector << 12), with
 * CS = vector<<8 and IP = 0. The BSP copies this blob to TRAMP (0x8000) and
 * SIPIs the APs with vector 0x08, so each AP begins at ap_trampoline_start
 * with CS base = TRAMP. The trampoline walks the same 16 -> 32 -> 64 bit
 * bring-up the BSP did in boot.S, reusing the BSP's page tables (pml4_table),
 * then jumps to the C entry point ap_main on a per-AP stack the BSP published
 * in ap_stack_top.
 *
 * The blob is linked into the high kernel .text but executes at TRAMP, so
 * control flow is position-independent: real-mode data is reached through
 * segment-relative offsets (a single .set constant — the LLVM assembler
 * rejects label arithmetic inside a memory operand), and the far-jump targets
 * are absolute TRAMP-relative immediates. References to kernel symbols
 * (pml4_table, ap_main, ap_stack_top) use absolute relocations the linker
 * bakes in, which stay correct wherever the bytes run.
 */
.intel_syntax noprefix

.set TRAMP, 0x8000

.section .text, "ax"
.global ap_trampoline_start
.global ap_trampoline_end
.extern ap_main
.extern ap_stack_top
.extern pml4_table

.code16
ap_trampoline_start:
    jmp ap_real_start

/* ---- trampoline-local GDT: null / 32-bit code / 32-bit data / 64-bit code -- */
.align 16
ap_gdt:
    .quad 0x0000000000000000
    .quad 0x00cf9a000000ffff   /* 0x08: 32-bit code */
    .quad 0x00cf92000000ffff   /* 0x10: 32-bit data */
    .quad 0x00af9a000000ffff   /* 0x18: 64-bit code */
ap_gdt_end:
ap_gdt_ptr:
    .word ap_gdt_end - ap_gdt - 1
    .long TRAMP + (ap_gdt - ap_trampoline_start)

/* Segment-relative offset of the GDT pointer (single absolute symbol so it is
 * legal inside the lgdt memory operand). */
.set GDT_PTR_OFF, ap_gdt_ptr - ap_trampoline_start

ap_real_start:
    cli
    cld
    /* CS base = TRAMP; mirror it into the data segments so blob-relative
     * offsets address our own bytes. */
    mov ax, cs
    mov ds, ax
    mov es, ax
    mov ss, ax

    lgdt [GDT_PTR_OFF]

    /* Enter protected mode. */
    mov eax, cr0
    or  eax, 1
    mov cr0, eax

    /* Far jump into 32-bit code (selector 0x08 = 32-bit code). */
    .att_syntax
    ljmpl $0x08, $(TRAMP + (ap_prot_mode - ap_trampoline_start))
    .intel_syntax noprefix

.code32
ap_prot_mode:
    mov ax, 0x10           /* 32-bit data selector */
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    /* Enable PAE + SSE (OSFXSR | OSXMMEXCPT), mirroring boot.S so this core can
     * run the same userspace/FPU code the boot CPU does. Without OSFXSR/EM-clear
     * an SSE instruction or fxsave/fxrstor in a process dispatched here faults. */
    mov eax, cr4
    or  eax, (1 << 5) | (1 << 9) | (1 << 10)
    mov cr4, eax

    /* Reuse the BSP's page tables (identity 0-4 GB). */
    mov eax, offset pml4_table
    mov cr3, eax

    /* Enable long mode (EFER.LME). */
    mov ecx, 0xc0000080
    rdmsr
    or  eax, (1 << 8)
    wrmsr

    /* Enable paging -> activate long mode; set MP and clear EM for SSE. */
    mov eax, cr0
    or  eax, (1 << 31) | (1 << 0) | (1 << 1)   /* PG | PE | MP */
    and eax, ~(1 << 2)                          /* clear EM */
    mov cr0, eax

    /* Far jump into 64-bit code (selector 0x18 = 64-bit code). */
    .att_syntax
    ljmp $0x18, $(TRAMP + (ap_long_mode - ap_trampoline_start))
    .intel_syntax noprefix

.code64
ap_long_mode:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    /* Load the per-AP stack the BSP published, then enter C. */
    movabs rax, offset ap_stack_top
    mov rsp, [rax]
    xor rbp, rbp
    movabs rax, offset ap_main
    call rax

ap_hang:
    cli
    hlt
    jmp ap_hang

ap_trampoline_end:
