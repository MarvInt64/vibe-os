.intel_syntax noprefix

.section .rodata, "a"
.global user_init_elf_start
.global user_init_elf_end

user_init_elf_start:
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

    .fill (0x100 - (. - user_init_elf_start)), 1, 0

payload_start:
    lea rsi, [rip + banner]
    mov edx, 67
    call write_stdout

shell_loop:
    lea rsi, [rip + prompt]
    mov edx, 2
    call write_stdout

wait_input:
    mov edi, 0
    lea rsi, [rip + input_buffer]
    mov edx, 95
    mov eax, 0
    int 0x80
    test eax, eax
    jg .have_input
    mov edi, 1
    mov eax, 19
    int 0x80
    jmp wait_input

.have_input:

    mov ecx, eax
    lea rbx, [rip + input_buffer]
    dec ecx
    js shell_loop
    cmp byte ptr [rbx + rcx], 10
    jne .line_ready
    mov byte ptr [rbx + rcx], 0
    jmp .line_sized

.line_ready:
    inc ecx
    mov byte ptr [rbx + rcx], 0

.line_sized:
    cmp ecx, 0
    je shell_loop

    cmp ecx, 9
    jne .check_help
    mov al, byte ptr [rbx]
    or al, 0x20
    cmp al, 'w'
    jne .check_help
    mov al, byte ptr [rbx + 1]
    or al, 0x20
    cmp al, 'i'
    jne .check_help
    mov al, byte ptr [rbx + 2]
    or al, 0x20
    cmp al, 'n'
    jne .check_help
    mov al, byte ptr [rbx + 3]
    or al, 0x20
    cmp al, 'd'
    jne .check_help
    mov al, byte ptr [rbx + 4]
    or al, 0x20
    cmp al, 'o'
    jne .check_help
    mov al, byte ptr [rbx + 5]
    or al, 0x20
    cmp al, 'w'
    jne .check_help
    mov al, byte ptr [rbx + 6]
    or al, 0x20
    cmp al, 'm'
    jne .check_help
    mov al, byte ptr [rbx + 7]
    or al, 0x20
    cmp al, 'g'
    jne .check_help
    mov al, byte ptr [rbx + 8]
    or al, 0x20
    cmp al, 'r'
    jne .check_help
    lea rdi, [rip + windowmgr_path]
    mov eax, 5
    int 0x80
    jmp shell_loop

.check_help:
    cmp ecx, 4
    jne check_about
    mov al, byte ptr [rbx]
    or al, 0x20
    cmp al, 'h'
    jne check_about
    mov al, byte ptr [rbx + 1]
    or al, 0x20
    cmp al, 'e'
    jne check_about
    mov al, byte ptr [rbx + 2]
    or al, 0x20
    cmp al, 'l'
    jne check_about
    mov al, byte ptr [rbx + 3]
    or al, 0x20
    cmp al, 'p'
    jne check_about
    lea rsi, [rip + help_text]
    mov edx, 57
    call write_stdout
    jmp shell_loop

check_about:
    cmp ecx, 5
    jne check_clear
    mov al, byte ptr [rbx]
    or al, 0x20
    cmp al, 'a'
    jne check_clear
    mov al, byte ptr [rbx + 1]
    or al, 0x20
    cmp al, 'b'
    jne check_clear
    mov al, byte ptr [rbx + 2]
    or al, 0x20
    cmp al, 'o'
    jne check_clear
    mov al, byte ptr [rbx + 3]
    or al, 0x20
    cmp al, 'u'
    jne check_clear
    mov al, byte ptr [rbx + 4]
    or al, 0x20
    cmp al, 't'
    jne check_clear
    lea rsi, [rip + about_text]
    mov edx, 32
    call write_stdout
    jmp shell_loop

check_clear:
    cmp ecx, 5
    jne check_ls
    mov al, byte ptr [rbx]
    or al, 0x20
    cmp al, 'c'
    jne check_exit
    mov al, byte ptr [rbx + 1]
    or al, 0x20
    cmp al, 'l'
    jne check_exit
    mov al, byte ptr [rbx + 2]
    or al, 0x20
    cmp al, 'e'
    jne check_exit
    mov al, byte ptr [rbx + 3]
    or al, 0x20
    cmp al, 'a'
    jne check_exit
    mov al, byte ptr [rbx + 4]
    or al, 0x20
    cmp al, 'r'
    jne check_exit
    mov edi, 1
    mov esi, 1
    xor edx, edx
    mov eax, 2
    int 0x80
    jmp shell_loop

check_ls:
    cmp ecx, 2
    jne .check_ls_arg
    mov al, byte ptr [rbx]
    or al, 0x20
    cmp al, 'l'
    jne check_stat
    mov al, byte ptr [rbx + 1]
    or al, 0x20
    cmp al, 's'
    jne check_stat
    lea r13, [rip + root_path]
    jmp .ls_begin

.check_ls_arg:
    cmp ecx, 4
    jl check_stat
    mov al, byte ptr [rbx]
    or al, 0x20
    cmp al, 'l'
    jne check_stat
    mov al, byte ptr [rbx + 1]
    or al, 0x20
    cmp al, 's'
    jne check_stat
    cmp byte ptr [rbx + 2], ' '
    jne check_stat
    lea r13, [rbx + 3]

.ls_begin:
    xor r14d, r14d

.ls_loop:
    mov rdi, r13
    mov esi, r14d
    lea rdx, [rip + entry_buffer]
    mov ecx, 64
    mov eax, 10
    int 0x80
    test eax, eax
    jz shell_loop
    js .ls_fail
    cmp edx, 2
    jne .ls_file
    lea rsi, [rip + dir_text]
    mov edx, 4
    call write_stdout
    jmp .ls_name

.ls_file:
    lea rsi, [rip + file_text]
    mov edx, 5
    call write_stdout

.ls_name:
    lea rsi, [rip + entry_buffer]
    call write_cstring
    lea rsi, [rip + newline]
    mov edx, 1
    call write_stdout
    inc r14d
    jmp .ls_loop

.ls_fail:
    lea rsi, [rip + list_fail_text]
    mov edx, 12
    call write_stdout
    jmp shell_loop

check_stat:
    cmp ecx, 6
    jl check_exit
    mov al, byte ptr [rbx]
    or al, 0x20
    cmp al, 's'
    jne check_exit
    mov al, byte ptr [rbx + 1]
    or al, 0x20
    cmp al, 't'
    jne check_exit
    mov al, byte ptr [rbx + 2]
    or al, 0x20
    cmp al, 'a'
    jne check_exit
    mov al, byte ptr [rbx + 3]
    or al, 0x20
    cmp al, 't'
    jne check_exit
    cmp byte ptr [rbx + 4], ' '
    jne check_exit
    lea rdi, [rbx + 5]
    mov eax, 9
    int 0x80
    test eax, eax
    jne .stat_fail
    mov r15, r8
    cmp edx, 2
    jne .stat_file
    lea rsi, [rip + dir_text]
    mov edx, 4
    call write_stdout
    jmp .stat_name

.stat_file:
    lea rsi, [rip + file_text]
    mov edx, 5
    call write_stdout

.stat_name:
    lea rsi, [rbx + 5]
    call write_cstring
    cmp r15, 0
    je .stat_newline
    lea rsi, [rip + size_text]
    mov edx, 6
    call write_stdout
    mov rax, r15
    call write_u64

.stat_newline:
    lea rsi, [rip + newline]
    mov edx, 1
    call write_stdout
    jmp shell_loop

.stat_fail:
    lea rsi, [rip + stat_fail_text]
    mov edx, 12
    call write_stdout
    jmp shell_loop

check_exit:
    cmp ecx, 4
    jne check_exec_path
    mov al, byte ptr [rbx]
    or al, 0x20
    cmp al, 'e'
    jne check_echo
    mov al, byte ptr [rbx + 1]
    or al, 0x20
    cmp al, 'x'
    jne check_echo
    mov al, byte ptr [rbx + 2]
    or al, 0x20
    cmp al, 'i'
    jne check_echo
    mov al, byte ptr [rbx + 3]
    or al, 0x20
    cmp al, 't'
    jne check_echo
    lea rsi, [rip + exit_text]
    mov edx, 11
    call write_stdout
    xor edi, edi
    mov eax, 4
    int 0x80
    hlt

check_exec_path:
    cmp byte ptr [rbx], '/'
    jne check_echo
    mov rdi, rbx
    mov eax, 5
    int 0x80
    test eax, eax
    jg .wait_child
    lea rsi, [rip + spawn_fail_text]
    mov edx, 13
    call write_stdout
    jmp shell_loop

.wait_child:
    mov edi, eax
    mov eax, 6
    int 0x80
    test eax, eax
    jle shell_loop
    lea rsi, [rip + wait_done_text]
    mov edx, 11
    call write_stdout
    jmp shell_loop

check_echo:
    cmp ecx, 4
    jl unknown_command
    mov al, byte ptr [rbx]
    or al, 0x20
    cmp al, 'c'
    jne check_echo_body
    mov al, byte ptr [rbx + 1]
    or al, 0x20
    cmp al, 'a'
    jne check_echo_body
    mov al, byte ptr [rbx + 2]
    or al, 0x20
    cmp al, 't'
    jne check_echo_body
    cmp byte ptr [rbx + 3], ' '
    jne check_echo_body
    lea rdi, [rbx + 4]
    mov eax, 7
    int 0x80
    test eax, eax
    jl .cat_open_failed
    mov r12d, eax

.cat_read_loop:
    mov edi, r12d
    lea rsi, [rip + file_buffer]
    mov edx, 96
    mov eax, 0
    int 0x80
    test eax, eax
    jle .cat_close
    mov edi, 1
    lea rsi, [rip + file_buffer]
    mov edx, eax
    mov eax, 1
    int 0x80
    jmp .cat_read_loop

.cat_close:
    mov edi, r12d
    mov eax, 8
    int 0x80
    jmp shell_loop

.cat_open_failed:
    lea rsi, [rip + open_fail_text]
    mov edx, 12
    call write_stdout
    jmp shell_loop

check_echo_body:
    cmp ecx, 5
    jl unknown_command
    mov al, byte ptr [rbx]
    or al, 0x20
    cmp al, 'e'
    jne unknown_command
    mov al, byte ptr [rbx + 1]
    or al, 0x20
    cmp al, 'c'
    jne unknown_command
    mov al, byte ptr [rbx + 2]
    or al, 0x20
    cmp al, 'h'
    jne unknown_command
    mov al, byte ptr [rbx + 3]
    or al, 0x20
    cmp al, 'o'
    jne unknown_command
    cmp byte ptr [rbx + 4], ' '
    jne unknown_command
    lea rsi, [rbx + 5]
    mov edx, ecx
    sub edx, 5
    call write_stdout
    lea rsi, [rip + newline]
    mov edx, 1
    call write_stdout
    jmp shell_loop

unknown_command:
    lea rsi, [rip + unknown_text]
    mov edx, 16
    call write_stdout
    jmp shell_loop

write_u64:
    lea rbx, [rip + number_buffer + 20]
    mov byte ptr [rbx], 0
    mov rcx, 0
    cmp rax, 0
    jne .convert_loop
    dec rbx
    mov byte ptr [rbx], '0'
    mov edx, 1
    mov rsi, rbx
    call write_stdout
    ret

.convert_loop:
    xor rdx, rdx
    mov r8, 10
    div r8
    dec rbx
    add dl, '0'
    mov byte ptr [rbx], dl
    inc rcx
    test rax, rax
    jne .convert_loop
    mov edx, ecx
    mov rsi, rbx
    call write_stdout
    ret

write_cstring:
    xor edx, edx
.cstring_len:
    cmp byte ptr [rsi + rdx], 0
    je .cstring_emit
    inc edx
    jmp .cstring_len
.cstring_emit:
    call write_stdout
    ret

write_stdout:
    mov edi, 1
    mov eax, 1
    int 0x80
    ret

banner:
    .ascii "VIBEOS SH\nHELP ABOUT CLEAR LS STAT CAT WINDOWMGR /BIN/HELLO EXIT\n"
banner_end:
prompt:
    .ascii "$ "
prompt_end:
help_text:
    .ascii "HELP ABOUT CLEAR LS STAT CAT WINDOWMGR /BIN/HELLO EXIT\n"
help_text_end:
about_text:
    .ascii "USERSPACE SHELL OVER TTY FD API\n"
about_text_end:
exit_text:
    .ascii "SHELL EXIT\n"
exit_text_end:
unknown_text:
    .ascii "UNKNOWN COMMAND\n"
unknown_text_end:
spawn_fail_text:
    .ascii "SPAWN FAILED\n"
spawn_fail_text_end:
wait_done_text:
    .ascii "CHILD DONE\n"
wait_done_text_end:
open_fail_text:
    .ascii "OPEN FAILED\n"
open_fail_text_end:
stat_fail_text:
    .ascii "STAT FAILED\n"
stat_fail_text_end:
list_fail_text:
    .ascii "LIST FAILED\n"
list_fail_text_end:
dir_text:
    .ascii "DIR "
dir_text_end:
file_text:
    .ascii "FILE "
file_text_end:
size_text:
    .ascii " SIZE "
size_text_end:
newline:
    .ascii "\n"
root_path:
    .ascii "/\0"
windowmgr_path:
    .ascii "/bin/windowmgr\0"
input_buffer:
    .zero 96
file_buffer:
    .zero 96
entry_buffer:
    .zero 64
number_buffer:
    .zero 21
payload_end:
user_init_elf_end:
