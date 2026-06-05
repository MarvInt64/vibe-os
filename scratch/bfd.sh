#!/bin/bash
set -e
CC="clang -target x86_64-none-elf -ffreestanding -fno-stack-protector -fno-pie -mno-red-zone -mcmodel=small -fno-builtin -Wall -Wextra -std=c11 -O2 -g -Iuser/libc/include -Iuser -Ilib/svg"
CXX="clang++ -target x86_64-none-elf -ffreestanding -fno-stack-protector -fno-pie -mno-red-zone -mcmodel=small -fno-builtin -Wall -Wextra -std=c++20 -O2 -fno-exceptions -g -Iuser/libc/include -Iuser"
LD=/opt/homebrew/bin/ld.lld; ST=/opt/homebrew/opt/llvm/bin/llvm-strip; CRT0=build/user/libc/crt0.o; A=build/user/libc.a
$CC -Ilib/svg -c user/vexui.c -o build/user/vexui.o
$CC -Ilib/svg -c lib/svg/svg.c -o build/user/svg.o
$CXX -c user/filedialog/filedialog.cpp -o build/user/filedialog.o
$CXX -c user/filedialog/main.cpp -o build/user/filedialog_main.o
$LD -nostdlib -static -T user/linker.ld -o build/user/filedialog.elf $CRT0 build/user/filedialog.o build/user/filedialog_main.o build/user/vexui.o build/user/svg.o $A
$ST --strip-all build/user/filedialog.elf
python3 scripts/ext2_put.py vibeos-disk.img build/user/filedialog.elf /bin/filedialog
echo "filedialog OK $(ls -la build/user/filedialog.elf | awk '{print $5}')"
