.intel_syntax noprefix

.section .multiboot, "a"
.align 8
multiboot_header_start:
    .long 0xe85250d6
    .long 0
    .long multiboot_header_end - multiboot_header_start
    .long -(0xe85250d6 + 0 + (multiboot_header_end - multiboot_header_start))

    .align 8
    .short 0
    .short 0
    .long 8
multiboot_header_end:

.section .bss, "aw", @nobits
.align 16
stack_bottom:
    .skip 65536          /* 64 KB: BearSSL's TLS handshake (EC/RSA) runs on the
                            kernel stack during the SYS_NET_HTTPS_GET syscall */
stack_top:

.section .data, "aw"
.align 8
.global pml4_table
.global pdp_table
.global pd_table0
.global pd_table1
.global pd_table2
.global pd_table3
.global framebuffer_pt0
.global framebuffer_pt1
.global framebuffer_pt2
.global framebuffer_pt3
boot_magic:
    .long 0
    .long 0
boot_info_ptr:
    .quad 0

.align 4096
pml4_table:
    .quad pdp_table + 0x3
    .fill 511, 8, 0

.align 4096
pdp_table:
    .quad pd_table0 + 0x3
    .quad pd_table1 + 0x3
    .quad pd_table2 + 0x3
    .quad pd_table3 + 0x3
    .fill 508, 8, 0

.align 4096
pd_table0:
    .set page_index, 0
    .rept 512
        .quad (page_index * 0x200000) + 0x83
        .set page_index, page_index + 1
    .endr

.align 4096
pd_table1:
    .set page_index, 0
    .rept 512
        .quad 0x40000000 + (page_index * 0x200000) + 0x83
        .set page_index, page_index + 1
    .endr

.align 4096
pd_table2:
    .set page_index, 0
    .rept 512
        .quad 0x80000000 + (page_index * 0x200000) + 0x83
        .set page_index, page_index + 1
    .endr

.align 4096
pd_table3:
    .set page_index, 0
    .rept 512
        .quad 0xc0000000 + (page_index * 0x200000) + 0x83
        .set page_index, page_index + 1
    .endr

.align 4096
framebuffer_pt0:
    .fill 512, 8, 0

.align 4096
framebuffer_pt1:
    .fill 512, 8, 0

.align 4096
framebuffer_pt2:
    .fill 512, 8, 0

.align 4096
framebuffer_pt3:
    .fill 512, 8, 0

.section .rodata, "a"
.align 16
gdt64:
    .quad 0x0000000000000000
    .quad 0x00af9a000000ffff
    .quad 0x00af92000000ffff
    .quad 0x00aff2000000ffff
    .quad 0x00affa000000ffff
gdt64_end:

gdt64_descriptor:
    .word gdt64_end - gdt64 - 1
    .long gdt64

.section .text, "ax"
.code32
.global _start
.extern kernel_main

_start:
    cli
    mov dword ptr [boot_magic], eax
    mov dword ptr [boot_info_ptr], ebx
    mov dword ptr [boot_info_ptr + 4], 0
    lea esp, [stack_top]

    lgdt [gdt64_descriptor]

    mov eax, cr4
    or eax, (1 << 5) | (1 << 9) | (1 << 10)   /* PAE | OSFXSR | OSXMMEXCPT (enable SSE) */
    mov cr4, eax

    mov ecx, 0xc0000080
    rdmsr
    or eax, 1 << 8
    wrmsr

    lea eax, [pml4_table]
    mov cr3, eax

    mov eax, cr0
    or eax, (1 << 31) | (1 << 0) | (1 << 1)   /* PG | PE | MP */
    and eax, ~(1 << 2)                          /* clear EM (enable SSE) */
    mov cr0, eax

    .att_syntax
    ljmp $0x08, $long_mode_entry
    .intel_syntax noprefix

.code64
long_mode_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    lea rsp, [stack_top]
    xor rbp, rbp

    mov edi, dword ptr [boot_magic]
    mov rsi, qword ptr [boot_info_ptr]
    call kernel_main

halt:
    hlt
    jmp halt
