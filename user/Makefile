CC = clang
LD = /opt/homebrew/bin/ld.lld
OBJCOPY = objcopy

CFLAGS = -target x86_64-none-elf -ffreestanding -fno-stack-protector -fno-pie \
         -mno-red-zone -mcmodel=kernel -mgeneral-regs-only \
         -mno-mmx -mno-sse -mno-sse2 \
         -fno-vectorize -fno-slp-vectorize -fno-builtin \
         -Wall -Wextra -std=c11 -O2

LDFLAGS = -nostdlib -static -T user/linker.ld

USER_PROGRAMS = sh

.PHONY: all clean

all: $(addprefix build/user/, $(addsuffix .elf, $(USER_PROGRAMS)))

build/user:
	mkdir -p build/user

build/user/%.elf: user/%.c user/linker.ld | build/user
	$(CC) $(CFLAGS) -c $< -o build/user/$*.o
	$(LD) $(LDFLAGS) -o $@ build/user/$*.o

clean:
	rm -rf build/user
