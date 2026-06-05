#!/bin/bash
set -e
CXX="clang++ -target x86_64-none-elf -ffreestanding -fno-stack-protector -fno-pie -mno-red-zone -mcmodel=small -fno-builtin -Wall -Wextra -std=c++20 -O2 -fno-exceptions -g -Iuser/libc/include -Iuser"
LD=/opt/homebrew/bin/ld.lld; ST=/opt/homebrew/opt/llvm/bin/llvm-strip; CRT0=build/user/libc/crt0.o; A=build/user/libc.a
$CXX -c user/texteditor/texteditor.cpp -o build/user/texteditor.o
$CXX -c user/texteditor/main.cpp -o build/user/texteditor_main.o
$LD -nostdlib -static -T user/linker.ld -o build/user/texteditor.elf $CRT0 build/user/texteditor.o build/user/texteditor_main.o build/user/vexui.o build/user/svg.o $A
$ST --strip-all build/user/texteditor.elf
python3 scripts/ext2_put.py vibeos-disk.img build/user/texteditor.elf /bin/texteditor
echo "texteditor OK"
