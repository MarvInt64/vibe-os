#!/bin/bash
# Build + install /bin/filebrowser (isolated; full `make` is blocked by foreign WIP).
set -e
cd "$(dirname "$0")/.."
CC="clang -target x86_64-none-elf -ffreestanding -fno-stack-protector -fno-pie -mno-red-zone -mcmodel=small -fno-builtin -Wall -Wextra -std=c11 -O2 -g -Iuser/libc/include -Iuser -Ilib/svg"
CXX="clang++ -target x86_64-none-elf -ffreestanding -fno-stack-protector -fno-pie -mno-red-zone -mcmodel=small -fno-builtin -Wall -Wextra -std=c++20 -O2 -fno-exceptions -g -Iuser/libc/include -Iuser -Ilib/svg"
LD=/opt/homebrew/bin/ld.lld; ST=/opt/homebrew/opt/llvm/bin/llvm-strip; CRT0=build/user/libc/crt0.o; A=build/user/libc.a
$CC  -c user/vexui.c -o build/user/vexui.o
$CC  -c lib/svg/svg.c -o build/user/svg.o
$CXX -Iuser/filebrowser -c user/filebrowser/filebrowser.cpp -o build/user/filebrowser.o
$CXX -Iuser/filebrowser -c user/filebrowser/main.cpp        -o build/user/filebrowser_main.o
$LD -nostdlib -static -T user/linker.ld -o build/user/filebrowser.elf $CRT0 build/user/filebrowser.o build/user/filebrowser_main.o build/user/vexui.o build/user/svg.o $A
$ST --strip-all build/user/filebrowser.elf
python3 scripts/ext2_put.py vibeos-disk.img build/user/filebrowser.elf /bin/filebrowser
echo "filebrowser OK $(ls -la build/user/filebrowser.elf | awk '{print $5}')"
