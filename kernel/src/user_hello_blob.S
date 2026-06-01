.intel_syntax noprefix

.section .rodata, "a"
.global user_hello_elf_start
.global user_hello_elf_end

user_hello_elf_start:
    .byte 0x7f, 0x45, 0x4c, 0x46, 0x02, 0x01, 0x01, 0x00
    .byte 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    .short 0x0002
    .short 0x003e
    .long 0x00000001
    .quad 0x0000000002000000
    .quad 0x0000000000000040
    .quad 0x0000000000000000
    .long 0x00000000
    .short 0x0040
    .short 0x0038
    .short 0x0001
    .short 0x0000
    .short 0x0000
    .short 0x0000

    .long 0x00000001
    .long 0x00000005
    .quad 0x0000000000000100
    .quad 0x0000000002000000
    .quad 0x0000000000000000
    .quad payload_end - payload_start
    .quad payload_end - payload_start
    .quad 0x0000000000001000

    .fill (0x100 - (. - user_hello_elf_start)), 1, 0

payload_start:
    mov edi, 1
    lea rsi, [rip + hello_text]
    mov edx, 25
    mov eax, 1
    int 0x80
    xor edi, edi
    mov eax, 4
    int 0x80
    hlt

hello_text:
    .ascii "HELLO FROM CHILD PROCESS\n"
hello_text_end:
payload_end:
user_hello_elf_end:
