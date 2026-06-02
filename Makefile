OUT_DIR := build
ISO_ROOT := $(OUT_DIR)/iso_root
CC := clang
CXX := clang++

LLVM_LLD := $(shell command -v ld.lld 2>/dev/null)
ifeq ($(LLVM_LLD),)
LLVM_LLD := $(firstword $(wildcard /opt/homebrew/opt/lld/bin/ld.lld /usr/local/opt/lld/bin/ld.lld /opt/homebrew/opt/llvm/bin/ld.lld /usr/local/opt/llvm/bin/ld.lld))
endif

X86_ELF_LD := $(shell command -v x86_64-elf-ld 2>/dev/null)

ifneq ($(filter undefined default,$(origin LD)),)
ifneq ($(LLVM_LLD),)
LD := $(LLVM_LLD)
else ifneq ($(X86_ELF_LD),)
LD := $(X86_ELF_LD)
else
LD := ld.lld
endif
endif

GRUB_MKRESCUE_DETECTED := $(shell command -v grub-mkrescue 2>/dev/null)
ifeq ($(GRUB_MKRESCUE_DETECTED),)
GRUB_MKRESCUE_DETECTED := $(shell command -v i686-elf-grub-mkrescue 2>/dev/null)
endif
ifeq ($(GRUB_MKRESCUE_DETECTED),)
GRUB_MKRESCUE_DETECTED := $(shell command -v x86_64-elf-grub-mkrescue 2>/dev/null)
endif

ifneq ($(filter undefined default,$(origin GRUB_MKRESCUE)),)
GRUB_MKRESCUE := $(GRUB_MKRESCUE_DETECTED)
endif

ifeq ($(GRUB_MKRESCUE),)
GRUB_MKRESCUE := grub-mkrescue
endif

QEMU ?= qemu-system-x86_64
# Intel e1000 NIC + user-mode (SLIRP) networking: gives the guest 10.0.2.15,
# gateway/DNS at 10.0.2.2/10.0.2.3, and emulated ICMP so ping works.
QEMU_NET ?= -netdev user,id=net0 -device e1000,netdev=net0
# Capture all traffic to a pcap for debugging: make run QEMU_NET_DUMP=-object...
QEMU_NET_DUMP ?=
# Persistent disk: a raw image attached as IDE primary master. The kernel
# auto-formats it as ext2 on first boot, so changes survive reboots.
# Kept outside build/ so `make clean` never wipes the user's persistent data.
DISK_IMG := vibeos-disk.img
DISK_SIZE_MB ?= 32
QEMU_DISK ?= -drive file=$(DISK_IMG),format=raw,if=ide,index=0,media=disk
# Hardware acceleration: hvf on macOS, kvm on Linux, tcg as fallback
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
QEMU_ACCEL ?= -accel hvf -accel tcg
else
QEMU_ACCEL ?= -accel kvm -accel tcg
endif

# "max" exposes RDRAND under TCG; the default qemu64 CPU does not, and the TLS
# layer needs it for entropy (otherwise it falls back to an insecure seed).
QEMU_CPU ?= max
QEMU_MEM ?= 512M

CFLAGS := -target x86_64-none-elf -ffreestanding -fno-stack-protector -fno-pie -mno-red-zone -mcmodel=kernel -mgeneral-regs-only -mno-mmx -mno-sse -mno-sse2 -fno-vectorize -fno-slp-vectorize -fno-builtin -Wall -Wextra -Wpedantic -std=c11 -Ikernel/include -Ithird_party/bearssl/inc -MMD -MP
KCXXFLAGS := -target x86_64-none-elf -ffreestanding -fno-stack-protector -fno-pie -mno-red-zone -mcmodel=kernel -mgeneral-regs-only -mno-mmx -mno-sse -mno-sse2 -fno-vectorize -fno-slp-vectorize -fno-builtin -Wall -Wextra -Wpedantic -std=c++20 -fno-exceptions -fno-rtti -Ikernel/include -MMD -MP
ASFLAGS := -target x86_64-none-elf -ffreestanding -D__ASSEMBLER__
LDFLAGS := -nostdlib -static -T linker.ld

KERNEL_SOURCES := kernel/src/alloc.c kernel/src/ramdisk.c kernel/src/ramdisk_demo.c kernel/src/app_builtin.c kernel/src/app_terminal.c kernel/src/app_task_manager.c kernel/src/bga.c kernel/src/e1000.c kernel/src/elf.c kernel/src/ext2_fs.c kernel/src/fd.c kernel/src/ide.c kernel/src/interrupts.c kernel/src/kernel.c kernel/src/input.c kernel/src/multiboot2.c kernel/src/net.c kernel/src/paging.c kernel/src/process.c kernel/src/render.c kernel/src/serial.c kernel/src/string.c kernel/src/syscall.c kernel/src/timer.c kernel/src/tty.c kernel/src/ui.c kernel/src/vfs.c kernel/src/vga_text.c kernel/src/window.c kernel/src/net_tls.c kernel/src/journal.c kernel/src/font_atlas.c
KERNEL_CXX_SOURCES := kernel/src/cxx_runtime.cpp kernel/src/cxx_smoke.cpp
KERNEL_ASM := kernel/src/boot.S kernel/src/interrupt_stubs.S kernel/src/user_init_blob.S kernel/src/user_hello_blob.S kernel/src/user_windowmgr_blob.S kernel/src/user_sh_blob.S kernel/src/user_edit_blob.S
KERNEL_OBJECTS := $(patsubst kernel/src/%.c,$(OUT_DIR)/kernel/%.o,$(KERNEL_SOURCES)) $(patsubst kernel/src/%.cpp,$(OUT_DIR)/kernel/%.o,$(KERNEL_CXX_SOURCES)) $(patsubst kernel/src/%.S,$(OUT_DIR)/kernel/%.o,$(KERNEL_ASM))

# ---- BearSSL (vendored TLS) built freestanding for the kernel target ----
# Hardware backends (AES-NI/SSE2/PCLMUL) and the OS entropy gatherer are forced
# off via -DBR_*=0; BearSSL falls back to its portable integer implementations,
# which is what -mgeneral-regs-only/-mno-sse requires. <string.h> resolves to
# kernel/include/string.h (memcpy/memset/... provided by kernel/src/string.c).
BEARSSL_DIR := third_party/bearssl
BEARSSL_LIB := $(OUT_DIR)/bearssl/libbearssl.a
BEARSSL_CFLAGS := -target x86_64-none-elf -ffreestanding -fno-stack-protector -fno-pie -mno-red-zone -mcmodel=kernel -mgeneral-regs-only -mno-mmx -mno-sse -mno-sse2 -fno-vectorize -fno-slp-vectorize -fno-builtin -std=c11 -O2 -I$(BEARSSL_DIR)/inc -I$(BEARSSL_DIR)/src -Ikernel/include -DBR_AES_X86NI=0 -DBR_SSE2=0 -DBR_POWER8=0 -DBR_RDRAND=0
BEARSSL_SRCS := $(shell find $(BEARSSL_DIR)/src -name '*.c' 2>/dev/null)
BEARSSL_OBJS := $(patsubst $(BEARSSL_DIR)/src/%.c,$(OUT_DIR)/bearssl/%.o,$(BEARSSL_SRCS))

$(OUT_DIR)/bearssl/%.o: $(BEARSSL_DIR)/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(BEARSSL_CFLAGS) -c $< -o $@

$(BEARSSL_LIB): $(BEARSSL_OBJS)
	@mkdir -p $(dir $@)
	$(UAR) rcs $@ $(BEARSSL_OBJS)

bearssl: $(BEARSSL_LIB)

.PHONY: all kernel iso run run-debug run-serial clean user disk newdisk apps libc bearssl

# Userspace app toolchain (separate ELF binaries, NOT compiled into the kernel).
UCC := clang
# Userspace MAY use SSE/float (the kernel enables SSE at boot and saves XMM per
# process across switches), which stb_truetype/stb_image and any float code need.
UCFLAGS := -target x86_64-none-elf -ffreestanding -fno-stack-protector -fno-pie -mno-red-zone -mcmodel=small -fno-builtin -Wall -Wextra -std=c11 -O2
UCXXFLAGS := -target x86_64-none-elf -ffreestanding -fno-stack-protector -fno-pie -mno-red-zone -mcmodel=small -fno-builtin -Wall -Wextra -std=c++20 -O2 -fno-exceptions
USTRIP := $(firstword $(wildcard /opt/homebrew/opt/llvm/bin/llvm-strip /usr/local/opt/llvm/bin/llvm-strip) llvm-strip)
UAR := $(firstword $(wildcard /opt/homebrew/opt/llvm/bin/llvm-ar /usr/local/opt/llvm/bin/llvm-ar) llvm-ar)

# VibeOS libc: standard headers + crt0. New apps link crt0.o + app.o + libc.a.
LIBC_INC := -Iuser/libc/include -Iuser
LIBC_A := build/user/libc.a
LIBC_CRT0 := build/user/libc/crt0.o

# Build standalone GUI apps and install them onto the persistent disk image
# under /bin. Requires a formatted disk (boot once with `make run`).
LIBC_DEPS := $(wildcard user/libc/*.c user/libc/*.cpp user/libc/include/*.h user/libc/include/sys/*.h) user/umalloc.c user/umalloc.h
$(LIBC_A): $(LIBC_DEPS)
	@mkdir -p build/user/libc
	$(UCC) $(UCFLAGS) $(LIBC_INC) -c user/libc/crt0.c   -o build/user/libc/crt0.o
	$(UCC) $(UCFLAGS) $(LIBC_INC) -c user/libc/sys.c    -o build/user/libc/sys.o
	$(UCC) $(UCFLAGS) $(LIBC_INC) -c user/libc/string.c -o build/user/libc/string.o
	$(UCC) $(UCFLAGS) $(LIBC_INC) -c user/libc/stdlib.c -o build/user/libc/stdlib.o
	$(UCC) $(UCFLAGS) $(LIBC_INC) -c user/libc/stdio.c  -o build/user/libc/stdio.o
	$(CXX) $(UCXXFLAGS) $(LIBC_INC) -c user/libc/cxxabi.cpp -o build/user/libc/cxxabi.o
	$(UCC) $(UCFLAGS) $(LIBC_INC) -c user/umalloc.c     -o build/user/libc/umalloc.o
	$(UAR) rcs $(LIBC_A) build/user/libc/sys.o build/user/libc/string.o build/user/libc/stdlib.o build/user/libc/stdio.o build/user/libc/cxxabi.o build/user/libc/umalloc.o

libc: $(LIBC_A)

apps: $(DISK_IMG) $(LIBC_A)
	@mkdir -p build/user
	$(UCC) $(UCFLAGS) -Ilib/svg -c lib/svg/svg.c -o build/user/svg.o
	$(UCC) $(UCFLAGS) -Ilib/svg -c user/vexui.c -o build/user/vexui.o
	$(UCC) $(UCFLAGS) -c user/uidemo.c -o build/user/uidemo.o
	$(LD) -nostdlib -static -T user/linker.ld -o build/user/uidemo.elf build/user/uidemo.o build/user/vexui.o build/user/svg.o
	$(USTRIP) --strip-all build/user/uidemo.elf
	python3 scripts/ext2_put.py $(DISK_IMG) build/user/uidemo.elf /bin/uidemo
	$(CXX) $(UCXXFLAGS) $(LIBC_INC) -Iuser -c user/taskmgr/taskmgr.cpp     -o build/user/taskmgr.o
	$(CXX) $(UCXXFLAGS) $(LIBC_INC) -Iuser -c user/taskmgr/process_row.cpp  -o build/user/taskmgr_process_row.o
	$(CXX) $(UCXXFLAGS) $(LIBC_INC) -Iuser -c user/taskmgr/task_manager.cpp -o build/user/taskmgr_task_manager.o
	$(LD) -nostdlib -static -T user/linker.ld -o build/user/taskmgr.elf \
		$(LIBC_CRT0) build/user/taskmgr.o build/user/taskmgr_process_row.o \
		build/user/taskmgr_task_manager.o build/user/vexui.o build/user/svg.o $(LIBC_A)
	$(USTRIP) --strip-all build/user/taskmgr.elf
	python3 scripts/ext2_put.py $(DISK_IMG) build/user/taskmgr.elf /bin/taskmgr
	$(CXX) $(UCXXFLAGS) $(LIBC_INC) -Iuser -c user/sysinfo/sysinfo.cpp -o build/user/sysinfo.o
	$(LD) -nostdlib -static -T user/linker.ld -o build/user/sysinfo.elf \
		$(LIBC_CRT0) build/user/sysinfo.o build/user/vexui.o build/user/svg.o $(LIBC_A)
	$(USTRIP) --strip-all build/user/sysinfo.elf
	python3 scripts/ext2_put.py $(DISK_IMG) build/user/sysinfo.elf /bin/sysinfo
	$(CXX) $(UCXXFLAGS) $(LIBC_INC) -Iuser -c user/dock/dock.cpp -o build/user/dock.o
	$(LD) -nostdlib -static -T user/linker.ld -o build/user/dock.elf \
		$(LIBC_CRT0) build/user/dock.o build/user/vexui.o build/user/svg.o $(LIBC_A)
	$(USTRIP) --strip-all build/user/dock.elf
	python3 scripts/ext2_put.py $(DISK_IMG) build/user/dock.elf /bin/dock
	$(CXX) $(UCXXFLAGS) $(LIBC_INC) -Iuser -c user/topbar/topbar.cpp -o build/user/topbar.o
	$(LD) -nostdlib -static -T user/linker.ld -o build/user/topbar.elf \
		$(LIBC_CRT0) build/user/topbar.o build/user/vexui.o build/user/svg.o $(LIBC_A)
	$(USTRIP) --strip-all build/user/topbar.elf
	python3 scripts/ext2_put.py $(DISK_IMG) build/user/topbar.elf /bin/topbar
	$(UCC) $(UCFLAGS) $(LIBC_INC) -Iuser/browser -c user/browser/weblayout.c  -o build/user/weblayout.o
	$(UCC) $(UCFLAGS) $(LIBC_INC) -Iuser/browser -c user/browser/dom.c        -o build/user/dom.o
	$(UCC) $(UCFLAGS) $(LIBC_INC) -Iuser/browser -c user/browser/css.c        -o build/user/css.o
	$(UCC) $(UCFLAGS) $(LIBC_INC) -Iuser/browser -Ithird_party/stb -c user/browser/appfont.c   -o build/user/appfont.o
	$(UCC) $(UCFLAGS) $(LIBC_INC) -Iuser/libimage -Ithird_party/stb -c user/libimage/image.c  -o build/user/image.o
	$(CC)  $(ASFLAGS)                             -c user/browser/font_data.S  -o build/user/font_data.o
	$(CXX) $(UCXXFLAGS) $(LIBC_INC) -Iuser -Iuser/browser -c user/browser/layout_engine.cpp -o build/user/browser_layout_engine.o
	$(CXX) $(UCXXFLAGS) $(LIBC_INC) -Iuser -Iuser/browser -Iuser/libimage -c user/browser/browser.cpp       -o build/user/browser_main.o
	$(LD) -nostdlib -static -T user/linker.ld -o build/user/browser.elf \
		$(LIBC_CRT0) build/user/browser_main.o build/user/browser_layout_engine.o \
		build/user/weblayout.o build/user/dom.o build/user/css.o \
		build/user/appfont.o build/user/image.o build/user/font_data.o $(LIBC_A)
	$(USTRIP) --strip-all build/user/browser.elf
	python3 scripts/ext2_put.py $(DISK_IMG) build/user/browser.elf /bin/browser
	$(UCC) $(UCFLAGS) $(LIBC_INC) -Iuser/libimage -c user/wallpaper/wallpaper.c -o build/user/wallpaper.o
	$(LD) -nostdlib -static -T user/linker.ld -o build/user/wallpaper.elf \
		$(LIBC_CRT0) build/user/wallpaper.o build/user/image.o $(LIBC_A)
	$(USTRIP) --strip-all build/user/wallpaper.elf
	python3 scripts/ext2_put.py $(DISK_IMG) build/user/wallpaper.elf /bin/wallpaper
	python3 scripts/png_to_vwp.py assets/wallpapers/default.png build/user/default.vwp
	python3 scripts/ext2_put.py $(DISK_IMG) build/user/default.vwp /wallpapers/default.vwp
	python3 scripts/ext2_put.py $(DISK_IMG) assets/wallpapers/default.png /wallpapers/default.png
	python3 scripts/ext2_put.py $(DISK_IMG) assets/icons/dock/browser.svg /icons/dock/browser.svg
	python3 scripts/ext2_put.py $(DISK_IMG) assets/icons/dock/taskmgr.svg /icons/dock/taskmgr.svg
	python3 scripts/ext2_put.py $(DISK_IMG) assets/icons/dock/terminal.svg /icons/dock/terminal.svg
	python3 scripts/ext2_put.py $(DISK_IMG) assets/icons/vibeos-logo.svg /icons/vibeos-logo.svg
	$(UCC) $(UCFLAGS) $(LIBC_INC) -c user/hello.c -o build/user/hello.o
	$(LD) -nostdlib -static -T user/linker.ld -o build/user/hello.elf $(LIBC_CRT0) build/user/hello.o $(LIBC_A)
	$(USTRIP) --strip-all build/user/hello.elf
	python3 scripts/ext2_put.py $(DISK_IMG) build/user/hello.elf /bin/hello
	$(CXX) $(UCXXFLAGS) $(LIBC_INC) -c user/threadtest.cpp -o build/user/threadtest.o
	$(LD) -nostdlib -static -T user/linker.ld -o build/user/threadtest.elf $(LIBC_CRT0) build/user/threadtest.o $(LIBC_A)
	$(USTRIP) --strip-all build/user/threadtest.elf
	python3 scripts/ext2_put.py $(DISK_IMG) build/user/threadtest.elf /bin/threadtest
	$(CXX) $(UCXXFLAGS) $(LIBC_INC) -c user/cpptest.cpp -o build/user/cpptest.o
	$(LD) -nostdlib -static -T user/linker.ld -o build/user/cpptest.elf $(LIBC_CRT0) build/user/cpptest.o $(LIBC_A)
	$(USTRIP) --strip-all build/user/cpptest.elf
	python3 scripts/ext2_put.py $(DISK_IMG) build/user/cpptest.elf /bin/cpptest
	@echo "Installed apps to $(DISK_IMG). They load from disk at runtime."

# Create a blank persistent disk image if it does not exist yet. No external
# tools needed — the kernel formats it as ext2 on first boot.
disk: $(DISK_IMG)
$(DISK_IMG):
	dd if=/dev/zero of=$(DISK_IMG) bs=1M count=$(DISK_SIZE_MB)
	@echo "Blank disk image created ($(DISK_SIZE_MB)MB); kernel will format it as ext2 on first boot."

# Wipe and recreate the disk image (reformats the filesystem, erasing data).
newdisk:
	rm -f $(DISK_IMG)
	$(MAKE) disk

all: kernel

user:
	@mkdir -p build/user
	clang -target x86_64-none-elf -ffreestanding -fno-stack-protector -fno-pie -mno-red-zone -mcmodel=small -fno-builtin -Wall -Wextra -std=c11 -O2 -c user/sh.c -o build/user/sh.o
	$(LD) -nostdlib -static -T user/linker.ld -o build/user/sh.elf build/user/sh.o
	./scripts/elf_to_blob2.sh build/user/sh.elf kernel/src/user_sh_blob.S
	clang -target x86_64-none-elf -ffreestanding -fno-stack-protector -fno-pie -mno-red-zone -mcmodel=small -fno-builtin -Wall -Wextra -std=c11 -O2 -c user/edit.c -o build/user/edit.o
	$(LD) -nostdlib -static -T user/linker.ld -o build/user/edit.elf build/user/edit.o
	./scripts/elf_to_blob2.sh build/user/edit.elf kernel/src/user_edit_blob.S

kernel: user $(OUT_DIR)/vibeos.elf

iso: $(OUT_DIR)/vibeos.iso

run: $(OUT_DIR)/vibeos.iso $(DISK_IMG)
	$(QEMU) -boot d -cdrom $(OUT_DIR)/vibeos.iso -m $(QEMU_MEM) -vga std -no-reboot -cpu $(QEMU_CPU) $(QEMU_ACCEL) $(QEMU_NET) $(QEMU_NET_DUMP) $(QEMU_DISK)

run-disk: $(OUT_DIR)/vibeos.elf
	$(QEMU) -boot d -cdrom $(OUT_DIR)/vibeos.iso -m $(QEMU_MEM) -vga std -no-reboot -cpu $(QEMU_CPU) $(QEMU_ACCEL) $(QEMU_NET) $(QEMU_NET_DUMP) -drive file=$(OUT_DIR)/disk.img,format=raw,index=0,media=disk

run-disk-serial: $(OUT_DIR)/vibeos.elf
	$(QEMU) -boot d -cdrom $(OUT_DIR)/vibeos.iso -m $(QEMU_MEM) -vga std -no-reboot -cpu $(QEMU_CPU) $(QEMU_ACCEL) $(QEMU_NET) $(QEMU_NET_DUMP) -hda $(OUT_DIR)/disk.img -serial stdio -monitor none

run-debug: $(OUT_DIR)/vibeos.iso $(DISK_IMG)
	$(QEMU) -boot d -cdrom $(OUT_DIR)/vibeos.iso -m $(QEMU_MEM) -vga std -no-reboot -cpu $(QEMU_CPU) $(QEMU_ACCEL) $(QEMU_NET) $(QEMU_NET_DUMP) $(QEMU_DISK) -no-shutdown

run-serial: $(OUT_DIR)/vibeos.iso $(DISK_IMG)
	$(QEMU) -boot d -cdrom $(OUT_DIR)/vibeos.iso -m $(QEMU_MEM) -vga std -no-reboot -cpu $(QEMU_CPU) $(QEMU_ACCEL) $(QEMU_NET) $(QEMU_NET_DUMP) $(QEMU_DISK) -serial stdio -monitor none

clean:
	rm -rf $(OUT_DIR)

$(OUT_DIR)/vibeos.elf: $(KERNEL_OBJECTS) $(BEARSSL_LIB) linker.ld
	@mkdir -p $(OUT_DIR)
	$(LD) $(LDFLAGS) -o $@ $(KERNEL_OBJECTS) $(BEARSSL_LIB)

$(OUT_DIR)/vibeos.iso: $(OUT_DIR)/vibeos.elf boot/grub/grub.cfg
	@mkdir -p $(ISO_ROOT)/boot/grub
	cp $(OUT_DIR)/vibeos.elf $(ISO_ROOT)/boot/vibeos.elf
	cp boot/grub/grub.cfg $(ISO_ROOT)/boot/grub/grub.cfg
	$(GRUB_MKRESCUE) -o $@ $(ISO_ROOT)

$(OUT_DIR)/kernel/%.o: kernel/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT_DIR)/kernel/%.o: kernel/src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(KCXXFLAGS) -c $< -o $@

$(OUT_DIR)/kernel/%.o: kernel/src/%.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -c $< -o $@

# Header dependency tracking: -MMD emits a .d file per object listing the
# headers it includes, so editing e.g. window.h rebuilds every .c that uses it.
# Without this, changing a struct in a header left stale objects with a
# mismatched layout — a memory-corruption heisenbug (e.g. a frozen cursor).
-include $(KERNEL_OBJECTS:.o=.d)
