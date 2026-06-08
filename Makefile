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
DISK_SIZE_MB ?= 64
QEMU_DISK ?= -drive file=$(DISK_IMG),format=raw,if=ide,index=0,media=disk
# Hardware acceleration.
#   - On Apple Silicon (arm64) HVF cannot help VibeOS: HVF virtualizes the
#     *native* CPU, so it only accelerates arm64 guests. VibeOS is x86_64 and
#     must be emulated by TCG — there is no x86 hardware to virtualize, which is
#     why the homebrew qemu-system-x86_64 doesn't even build HVF in. The only
#     real speedups are an Intel Mac / x86 Linux (KVM), or porting VibeOS to
#     arm64. Tuning knobs were tried (tb-size, MTTCG) without a measurable win,
#     so we keep plain stable TCG. To experiment, override on the command line,
#     e.g.  make run QEMU_ACCEL="-accel tcg,tb-size=512".
#   - On Intel macOS, HVF works. On Linux/x86 use KVM. TCG is the fallback.
UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)
ifeq ($(UNAME_S),Darwin)
ifeq ($(UNAME_M),arm64)
QEMU_ACCEL ?= -accel tcg
else
QEMU_ACCEL ?= -accel hvf -accel tcg
endif
else
QEMU_ACCEL ?= -accel kvm -accel tcg
endif

# "max" exposes RDRAND under TCG; the default qemu64 CPU does not, and the TLS
# layer needs it for entropy (otherwise it falls back to an insecure seed).
QEMU_CPU ?= max
QEMU_MEM ?= 512M
# Number of CPU cores to expose. SMP support brings the APs online; override
# with e.g. `make run-serial QEMU_SMP="-smp 2"` or QEMU_SMP= to force one core.
QEMU_SMP ?= -smp 4
QEMU_AUDIO ?= -audiodev coreaudio,id=audio0,out.frequency=48000,out.mixing-engine=on -device AC97,audiodev=audio0

CFLAGS := -target x86_64-none-elf -ffreestanding -fno-stack-protector -fno-pie -mno-red-zone -mcmodel=kernel -mgeneral-regs-only -mno-mmx -mno-sse -mno-sse2 -fno-vectorize -fno-slp-vectorize -fno-builtin -Wall -Wextra -Wpedantic -std=c11 -Ikernel/include -Ithird_party/bearssl/inc -MMD -MP -O2
KCXXFLAGS := -target x86_64-none-elf -ffreestanding -fno-stack-protector -fno-pie -mno-red-zone -mcmodel=kernel -mgeneral-regs-only -mno-mmx -mno-sse -mno-sse2 -fno-vectorize -fno-slp-vectorize -fno-builtin -Wall -Wextra -Wpedantic -std=c++20 -fno-exceptions -fno-rtti -Ikernel/include -MMD -MP -O2
ASFLAGS := -target x86_64-none-elf -ffreestanding -D__ASSEMBLER__
LDFLAGS := -nostdlib -static -T linker.ld

KERNEL_SOURCES := kernel/src/alloc.c kernel/src/clipboard.c kernel/src/ramdisk.c kernel/src/ramdisk_demo.c kernel/src/app_builtin.c kernel/src/app_terminal.c kernel/src/app_task_manager.c kernel/src/audio.c kernel/src/bga.c kernel/src/e1000.c kernel/src/elf.c kernel/src/ext2_fs.c kernel/src/fd.c kernel/src/ide.c kernel/src/interrupts.c kernel/src/acpi.c kernel/src/apic.c kernel/src/cpu.c kernel/src/bkl.c kernel/src/smp.c kernel/src/kernel.c kernel/src/input.c kernel/src/multiboot2.c kernel/src/net.c kernel/src/paging.c kernel/src/process.c kernel/src/pty.c kernel/src/render.c kernel/src/serial.c kernel/src/settings.c kernel/src/string.c kernel/src/syscall.c kernel/src/timer.c kernel/src/tty.c kernel/src/ui.c kernel/src/vfs.c kernel/src/vga_text.c kernel/src/window.c kernel/src/net_tls.c kernel/src/journal.c kernel/src/font_atlas.c
KERNEL_CXX_SOURCES := kernel/src/cxx_runtime.cpp kernel/src/cxx_smoke.cpp
KERNEL_ASM := kernel/src/boot.S kernel/src/interrupt_stubs.S kernel/src/ap_boot.S kernel/src/user_init_blob.S kernel/src/user_hello_blob.S kernel/src/user_windowmgr_blob.S kernel/src/user_sh_blob.S kernel/src/user_edit_blob.S
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

.PHONY: all kernel iso run run-debug run-serial run-arm64 clean user disk newdisk verify-disk fsck-disk apps libc bearssl bump-version tcc tcc-src seed-headers kernel-arm64 arm64-user arm64-tcc

# ============================================================
# arm64 build — targeting QEMU virt machine.
#
# Portable kernel sources (no x86 port I/O, no APIC, no IDE):
ARM64_COMMON_SRCS := \
    kernel/src/alloc.c \
    kernel/src/string.c \
    kernel/src/ext2_fs.c \
    kernel/src/ramdisk.c \
    kernel/src/elf.c \
    kernel/src/render.c \
    kernel/src/font_atlas.c \
    kernel/src/window.c \
    kernel/src/bkl.c \
    kernel/src/keymap.c

# arm64-specific sources (in kernel/arch/arm64/):
ARM64_ARCH_SRCS := \
    kernel/arch/arm64/mmu.c \
    kernel/arch/arm64/uart.c \
    kernel/arch/arm64/gic.c \
    kernel/arch/arm64/timer.c \
    kernel/arch/arm64/virtio_blk.c \
    kernel/arch/arm64/virtio_input.c \
    kernel/arch/arm64/virtio_snd.c \
    kernel/arch/arm64/ramfb.c \
    kernel/arch/arm64/process.c \
    kernel/arch/arm64/stubs.c \
    kernel/arch/arm64/arch.c \
    kernel/arch/arm64/cpu.c \
    kernel/arch/arm64/smp.c

ARM64_ASM_SRCS := \
    kernel/arch/arm64/boot.S \
    kernel/arch/arm64/exceptions.S \
    kernel/arch/arm64/usermode.S \
    kernel/arch/arm64/user_demo.S

ARM64_CFLAGS := \
    -target aarch64-none-elf \
    -ffreestanding \
    -fno-stack-protector \
    -fno-pie \
    -O1 \
    -std=c11 \
    -Wall -Wextra \
    -DARCH_ARM64=1 \
    -Ikernel/include \
    -Ikernel/arch/arm64 \
    -mstrict-align \
    -fno-builtin-memcpy \
    -fno-builtin-memmove \
    -fno-builtin-memset \
     \
    -fno-slp-vectorize

ARM64_ASFLAGS := \
    -target aarch64-none-elf \
    -ffreestanding \
    -D__ASSEMBLER__ \
    -Ikernel/arch/arm64

# Use distinct subdirs to avoid stem collisions (e.g. both kernel/src/timer.c
# and kernel/arch/arm64/timer.c mapping to the same build/arm64/timer.o).
#   build/arm64/common/alloc.o   ← kernel/src/alloc.c
#   build/arm64/arch/boot.o      ← kernel/arch/arm64/boot.S
ARM64_OBJECTS := \
    $(patsubst kernel/src/%.c,$(OUT_DIR)/arm64/common/%.o,$(ARM64_COMMON_SRCS)) \
    $(patsubst kernel/arch/arm64/%.c,$(OUT_DIR)/arm64/arch/%.o,$(ARM64_ARCH_SRCS)) \
    $(patsubst kernel/arch/arm64/%.S,$(OUT_DIR)/arm64/arch/%.o,$(ARM64_ASM_SRCS))

$(OUT_DIR)/arm64/common/%.o: kernel/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(ARM64_CFLAGS) -c $< -o $@

$(OUT_DIR)/arm64/arch/%.o: kernel/arch/arm64/%.c
	@mkdir -p $(dir $@)
	$(CC) $(ARM64_CFLAGS) -c $< -o $@

$(OUT_DIR)/arm64/arch/%.o: kernel/arch/arm64/%.S
	@mkdir -p $(dir $@)
	$(CC) $(ARM64_ASFLAGS) -c $< -o $@

kernel-arm64: $(ARM64_OBJECTS)
	$(LLVM_LLD) -nostdlib -static -T linker-arm64.ld -o $(OUT_DIR)/vibeos-arm64.elf $(ARM64_OBJECTS)
	@echo "arm64 kernel built: $(OUT_DIR)/vibeos-arm64.elf"
	@$(firstword $(wildcard /opt/homebrew/opt/llvm/bin/llvm-size /usr/local/opt/llvm/bin/llvm-size) llvm-size) $(OUT_DIR)/vibeos-arm64.elf 2>/dev/null || true

# Run arm64 VibeOS on QEMU. On Apple Silicon this uses HVF for native speed!
QEMU_ARM64 := qemu-system-aarch64
ifeq ($(UNAME_M),arm64)
QEMU_ARM64_ACCEL ?= -accel hvf
else
QEMU_ARM64_ACCEL ?= -accel tcg
endif

run-arm64: kernel-arm64
	$(QEMU_ARM64) \
	  -machine virt,gic-version=2 \
	  -cpu $(if $(filter arm64,$(UNAME_M)),host,cortex-a72) \
	  $(QEMU_ARM64_ACCEL) \
	  -m 512M \
	  -smp 4 \
	  -kernel $(OUT_DIR)/vibeos-arm64.elf \
	  -drive file=$(DISK_IMG),if=none,id=hd0,format=raw \
	  -device virtio-blk-device,drive=hd0 \
	  -device ramfb \
	  -device virtio-tablet-device \
	  -device virtio-keyboard-device \
	  -audiodev coreaudio,id=snd0 \
	  -device virtio-sound-device,audiodev=snd0 \
	  -serial stdio \
	  -no-reboot

# Build aarch64 EL0 userspace from the SHARED libc + SHARED app sources, so the
# exact same C (user/libc/*, user/*.c) runs on both x86_64 and arm64 — only the
# syscall trampoline (svc vs int 0x80, behind ARCH_ARM64) differs. Linked at
# VA 0x90000000 (see user/arm64/link.ld).
ARM64_UCFLAGS := -target aarch64-none-elf -ffreestanding -fno-pie \
    -fno-stack-protector -fno-builtin -mstrict-align -O1 -std=c11 \
    -DARCH_ARM64 -Iuser/libc/include -Iuser
ARM64_UDIR := $(OUT_DIR)/arm64/user
ARM64_ULIBC_SRCS := sys.c string.c stdlib.c stdio.c

# $(call arm64app,<src>,<diskpath>) — compile one shared C app + link with libc
define arm64app
	$(CC) $(ARM64_UCFLAGS) -c $(1) -o $(ARM64_UDIR)/app.o
	$(LLVM_LLD) -nostdlib -static -T user/arm64/link.ld -o $(ARM64_UDIR)/app.elf \
	    $(ARM64_UDIR)/crt0.o $(ARM64_UDIR)/app.o $(ARM64_UDIR)/libc.a
	python3 scripts/ext2_put.py $(DISK_IMG) $(ARM64_UDIR)/app.elf $(2)
endef

arm64-user: $(DISK_IMG)
	@mkdir -p $(ARM64_UDIR)
	# --- shared libc (crt0 + sys/string/stdlib/stdio + umalloc) for aarch64 ---
	$(CC) $(ARM64_UCFLAGS) -c user/libc/crt0.c -o $(ARM64_UDIR)/crt0.o
	@for s in $(ARM64_ULIBC_SRCS); do \
	    $(CC) $(ARM64_UCFLAGS) -c user/libc/$$s -o $(ARM64_UDIR)/$${s%.c}.o || exit 1; \
	done
	$(CC) $(ARM64_UCFLAGS) -c user/umalloc.c -o $(ARM64_UDIR)/umalloc.o
	$(CC) $(ARM64_UCFLAGS) -c user/arm64/libc_stubs.c -o $(ARM64_UDIR)/stubs.o
	# --- arm64 assembly support (setjmp/longjmp, soft-float stubs) ---
	$(CC) $(ARM64_UCFLAGS) -c user/arm64/setjmp.S    -o $(ARM64_UDIR)/setjmp.o
	$(CC) $(ARM64_UCFLAGS) -c user/arm64/softfloat.S -o $(ARM64_UDIR)/softfloat.o
	# --- C++ runtime for arm64 (must be in libc.a before any app links) ---
	clang++ $(ARM64_UCFLAGS) -std=c++20 -fno-exceptions -fno-rtti \
	    -c user/libc/cxxabi.cpp -o $(ARM64_UDIR)/cxxabi.o
	$(UAR) rcs $(ARM64_UDIR)/libc.a \
	    $(ARM64_UDIR)/sys.o $(ARM64_UDIR)/string.o \
	    $(ARM64_UDIR)/stdlib.o $(ARM64_UDIR)/stdio.o \
	    $(ARM64_UDIR)/umalloc.o $(ARM64_UDIR)/stubs.o \
	    $(ARM64_UDIR)/setjmp.o $(ARM64_UDIR)/softfloat.o $(ARM64_UDIR)/cxxabi.o
	# --- shared graphics lib for userspace: the SAME render.c/font_atlas.c the
	#     kernel uses, compiled against the user libc (one renderer everywhere) ---
	$(CC) $(ARM64_UCFLAGS) -Ikernel/include -c kernel/src/render.c     -o $(ARM64_UDIR)/u_render.o
	$(CC) $(ARM64_UCFLAGS) -Ikernel/include -c kernel/src/font_atlas.c -o $(ARM64_UDIR)/u_font.o
	$(UAR) rcs $(ARM64_UDIR)/libgfx.a $(ARM64_UDIR)/u_render.o $(ARM64_UDIR)/u_font.o
	# --- shared apps (same .c files x86 uses) ---
	$(call arm64app,user/hello.c,/bin/hello)
	$(call arm64app,user/gfxdemo.c,/bin/gfxdemo)
	# --- input test: polls virtio-input and prints to serial ---
	$(call arm64app,user/inputtest.c,/bin/inputtest)
	# --- desktop: uses the shared renderer as a userspace lib ---
	$(CC) $(ARM64_UCFLAGS) -Ikernel/include -c user/desktop.c -o $(ARM64_UDIR)/app.o
	$(LLVM_LLD) -nostdlib -static -T user/arm64/link.ld -o $(ARM64_UDIR)/app.elf \
	    $(ARM64_UDIR)/crt0.o $(ARM64_UDIR)/app.o $(ARM64_UDIR)/libgfx.a $(ARM64_UDIR)/libc.a
	python3 scripts/ext2_put.py $(DISK_IMG) $(ARM64_UDIR)/app.elf /bin/desktop
	# --- VexUI toolkit for arm64 ---
	$(CC) $(ARM64_UCFLAGS) -Ilib/svg -c user/vexui.c -o $(ARM64_UDIR)/vexui.o
	# --- wallpaper: minimal arm64 wallpaper (no libimage dependency) ---
	$(CC) $(ARM64_UCFLAGS) -c user/wallpaper_arm64.c -o $(ARM64_UDIR)/app.o
	$(LLVM_LLD) -nostdlib -static -T user/arm64/link.ld -o $(ARM64_UDIR)/app.elf \
	    $(ARM64_UDIR)/app.o $(ARM64_UDIR)/libc.a
	python3 scripts/ext2_put.py $(DISK_IMG) $(ARM64_UDIR)/app.elf /bin/wallpaper
		# --- SVG renderer for arm64 ---
	$(CC) $(ARM64_UCFLAGS) -Ilib/svg -c lib/svg/svg.c -o $(ARM64_UDIR)/svg.o
	# --- Dock (C++ GUI app using VexUI) ---
	clang++ $(ARM64_UCFLAGS) -std=c++20 -fno-exceptions -fno-rtti -Ilib/svg \
	    -c user/dock/dock.cpp -o $(ARM64_UDIR)/dock.o
	$(LLVM_LLD) -nostdlib -static -T user/arm64/link.ld -o $(ARM64_UDIR)/dock.elf \
	    $(ARM64_UDIR)/crt0.o $(ARM64_UDIR)/dock.o $(ARM64_UDIR)/vexui.o \
	    $(ARM64_UDIR)/svg.o $(ARM64_UDIR)/libc.a
	python3 scripts/ext2_put.py $(DISK_IMG) $(ARM64_UDIR)/dock.elf /bin/dock
	# --- Topbar (C++ GUI app, no VexUI dependency) ---
	clang++ $(ARM64_UCFLAGS) -std=c++20 -fno-exceptions -fno-rtti -Ilib/svg \
	    -c user/topbar/topbar.cpp -o $(ARM64_UDIR)/topbar.o
	$(LLVM_LLD) -nostdlib -static -T user/arm64/link.ld -o $(ARM64_UDIR)/topbar.elf \
	    $(ARM64_UDIR)/crt0.o $(ARM64_UDIR)/topbar.o $(ARM64_UDIR)/svg.o $(ARM64_UDIR)/libc.a
	python3 scripts/ext2_put.py $(DISK_IMG) $(ARM64_UDIR)/topbar.elf /bin/topbar
	# --- Task Manager (C++ GUI app using VexUI; shows live processes) ---
	clang++ $(ARM64_UCFLAGS) -std=c++20 -fno-exceptions -fno-rtti -Ilib/svg -Iuser \
	    -c user/taskmgr/taskmgr.cpp -o $(ARM64_UDIR)/taskmgr.o
	clang++ $(ARM64_UCFLAGS) -std=c++20 -fno-exceptions -fno-rtti -Ilib/svg -Iuser \
	    -c user/taskmgr/process_row.cpp -o $(ARM64_UDIR)/taskmgr_process_row.o
	clang++ $(ARM64_UCFLAGS) -std=c++20 -fno-exceptions -fno-rtti -Ilib/svg -Iuser \
	    -c user/taskmgr/task_manager.cpp -o $(ARM64_UDIR)/taskmgr_task_manager.o
	$(LLVM_LLD) -nostdlib -static -T user/arm64/link.ld -o $(ARM64_UDIR)/taskmgr.elf \
	    $(ARM64_UDIR)/crt0.o $(ARM64_UDIR)/taskmgr.o $(ARM64_UDIR)/taskmgr_process_row.o \
	    $(ARM64_UDIR)/taskmgr_task_manager.o $(ARM64_UDIR)/vexui.o \
	    $(ARM64_UDIR)/svg.o $(ARM64_UDIR)/libc.a
	python3 scripts/ext2_put.py $(DISK_IMG) $(ARM64_UDIR)/taskmgr.elf /bin/taskmgr
	# --- Shell (standalone, own _start, needs libc for memcpy) ---
	$(CC) $(ARM64_UCFLAGS) -c user/sh.c -o $(ARM64_UDIR)/sh.o
	$(LLVM_LLD) -nostdlib -static -T user/arm64/link.ld -o $(ARM64_UDIR)/sh.elf \
	    $(ARM64_UDIR)/sh.o $(ARM64_UDIR)/libc.a
	python3 scripts/ext2_put.py $(DISK_IMG) $(ARM64_UDIR)/sh.elf /bin/sh
	# --- Keymap files (text-based, loadable at runtime) ---
	python3 scripts/ext2_put.py $(DISK_IMG) keymaps/de.txt /etc/keymap.de
	# --- Basic UNIX utils (same .c files as x86, use libc wrappers) ---
	$(call arm64app,user/echo.c,/bin/echo)
	$(call arm64app,user/cat.c,/bin/cat)
	$(call arm64app,user/cp.c,/bin/cp)
	$(call arm64app,user/rm.c,/bin/rm)
	$(call arm64app,user/mv.c,/bin/mv)
	$(call arm64app,user/mkdir.c,/bin/mkdir)
	$(call arm64app,user/touch.c,/bin/touch)
	$(call arm64app,user/ln.c,/bin/ln)
	$(call arm64app,user/stat.c,/bin/stat)
	$(call arm64app,user/whoami.c,/bin/whoami)
	$(call arm64app,user/id.c,/bin/id)
	$(call arm64app,user/chmod.c,/bin/chmod)
	$(call arm64app,user/chown.c,/bin/chown)
	$(call arm64app,user/head.c,/bin/head)
	$(call arm64app,user/pwd.c,/bin/pwd)
	$(call arm64app,user/find.c,/bin/find)
	$(call arm64app,user/grep.c,/bin/grep)
	# --- Sysinfo (C++ GUI) ---
	clang++ $(ARM64_UCFLAGS) -std=c++20 -fno-exceptions -fno-rtti -Ilib/svg -Iuser \
	    -c user/sysinfo/sysinfo.cpp -o $(ARM64_UDIR)/sysinfo.o
	$(LLVM_LLD) -nostdlib -static -T user/arm64/link.ld -o $(ARM64_UDIR)/sysinfo.elf \
	    $(ARM64_UDIR)/crt0.o $(ARM64_UDIR)/sysinfo.o $(ARM64_UDIR)/vexui.o \
	    $(ARM64_UDIR)/svg.o $(ARM64_UDIR)/libc.a
	python3 scripts/ext2_put.py $(DISK_IMG) $(ARM64_UDIR)/sysinfo.elf /bin/sysinfo
	# --- Terminal (C++ GUI) ---
	clang++ $(ARM64_UCFLAGS) -std=c++20 -fno-exceptions -fno-rtti -Ilib/svg -Iuser \
	    -c user/terminal/terminal.cpp -o $(ARM64_UDIR)/terminal.o
	clang++ $(ARM64_UCFLAGS) -std=c++20 -fno-exceptions -fno-rtti -Ilib/svg -Iuser \
	    -c user/terminal/text_grid.cpp -o $(ARM64_UDIR)/terminal_text_grid.o
	$(LLVM_LLD) -nostdlib -static -T user/arm64/link.ld -o $(ARM64_UDIR)/terminal.elf \
	    $(ARM64_UDIR)/crt0.o $(ARM64_UDIR)/terminal.o $(ARM64_UDIR)/terminal_text_grid.o \
	    $(ARM64_UDIR)/vexui.o $(ARM64_UDIR)/svg.o $(ARM64_UDIR)/libc.a
		python3 scripts/ext2_put.py $(DISK_IMG) $(ARM64_UDIR)/terminal.elf /bin/terminal
		# --- Text Editor (C++ GUI using VexUI) ---
		clang++ $(ARM64_UCFLAGS) -std=c++20 -fno-exceptions -fno-rtti -Ilib/svg -Iuser \
		    -c user/texteditor/texteditor.cpp -o $(ARM64_UDIR)/texteditor.o
		clang++ $(ARM64_UCFLAGS) -std=c++20 -fno-exceptions -fno-rtti -Ilib/svg -Iuser \
		    -c user/texteditor/main.cpp -o $(ARM64_UDIR)/texteditor_main.o
		$(LLVM_LLD) -nostdlib -static -T user/arm64/link.ld -o $(ARM64_UDIR)/texteditor.elf \
		    $(ARM64_UDIR)/crt0.o $(ARM64_UDIR)/texteditor.o $(ARM64_UDIR)/texteditor_main.o \
		    $(ARM64_UDIR)/vexui.o $(ARM64_UDIR)/svg.o $(ARM64_UDIR)/libc.a
			python3 scripts/ext2_put.py $(DISK_IMG) $(ARM64_UDIR)/texteditor.elf /bin/texteditor
		# --- Filedialog (C++ GUI using VexUI) ---
		clang++ $(ARM64_UCFLAGS) -std=c++20 -fno-exceptions -fno-rtti -Ilib/svg -Iuser \
		    -c user/filedialog/filedialog.cpp -o $(ARM64_UDIR)/filedialog.o
		$(LLVM_LLD) -nostdlib -static -T user/arm64/link.ld -o $(ARM64_UDIR)/filedialog.elf \
		    $(ARM64_UDIR)/crt0.o $(ARM64_UDIR)/filedialog.o \
		    $(ARM64_UDIR)/vexui.o $(ARM64_UDIR)/svg.o $(ARM64_UDIR)/libc.a
		python3 scripts/ext2_put.py $(DISK_IMG) $(ARM64_UDIR)/filedialog.elf /bin/filedialog
		# --- uidemo (C GUI demo) ---
		$(CC) $(ARM64_UCFLAGS) -Ilib/svg -c user/uidemo.c -o $(ARM64_UDIR)/uidemo.o
		$(LLVM_LLD) -nostdlib -static -T user/arm64/link.ld -o $(ARM64_UDIR)/uidemo.elf \
		    $(ARM64_UDIR)/uidemo.o $(ARM64_UDIR)/vexui.o $(ARM64_UDIR)/svg.o $(ARM64_UDIR)/libc.a
		python3 scripts/ext2_put.py $(DISK_IMG) $(ARM64_UDIR)/uidemo.elf /bin/uidemo
		# --- More UNIX utils ---
		$(call arm64app,user/wc.c,/bin/wc)
		$(call arm64app,user/tail.c,/bin/tail)
		$(call arm64app,user/su.c,/bin/su)
			$(call arm64app,user/clipboard.c,/bin/clipboard)
			$(call arm64app,user/audiocfg.c,/bin/audiocfg)
			$(call arm64app,user/audiotest.c,/bin/audiotest)
			# --- mp3play (needs mp3dec header) ---
			$(CC) $(ARM64_UCFLAGS) -Ilib/mp3 -c user/mp3play.c -o $(ARM64_UDIR)/mp3play.o
			$(LLVM_LLD) -nostdlib -static -T user/arm64/link.ld -o $(ARM64_UDIR)/mp3play.elf \
			    $(ARM64_UDIR)/crt0.o $(ARM64_UDIR)/mp3play.o $(ARM64_UDIR)/mp3dec.o $(ARM64_UDIR)/libc.a
			python3 scripts/ext2_put.py $(DISK_IMG) $(ARM64_UDIR)/mp3play.elf /bin/mp3play
				# --- adduser (C++) ---
				clang++ $(ARM64_UCFLAGS) -std=c++20 -fno-exceptions -fno-rtti \
				    -c user/adduser.cpp -o $(ARM64_UDIR)/adduser.o
				$(LLVM_LLD) -nostdlib -static -T user/arm64/link.ld -o $(ARM64_UDIR)/adduser.elf \
				    $(ARM64_UDIR)/crt0.o $(ARM64_UDIR)/adduser.o $(ARM64_UDIR)/libc.a
				python3 scripts/ext2_put.py $(DISK_IMG) $(ARM64_UDIR)/adduser.elf /bin/adduser
		# --- File Browser (C++ GUI) ---
	clang++ $(ARM64_UCFLAGS) -std=c++20 -fno-exceptions -fno-rtti -Ilib/svg -Iuser \
	    -c user/filebrowser/filebrowser.cpp -o $(ARM64_UDIR)/filebrowser.o
	clang++ $(ARM64_UCFLAGS) -std=c++20 -fno-exceptions -fno-rtti -Ilib/svg -Iuser \
	    -c user/filebrowser/main.cpp -o $(ARM64_UDIR)/filebrowser_main.o
	$(LLVM_LLD) -nostdlib -static -T user/arm64/link.ld -o $(ARM64_UDIR)/filebrowser.elf \
	    $(ARM64_UDIR)/crt0.o $(ARM64_UDIR)/filebrowser.o $(ARM64_UDIR)/filebrowser_main.o \
	    $(ARM64_UDIR)/vexui.o $(ARM64_UDIR)/svg.o $(ARM64_UDIR)/libc.a
	python3 scripts/ext2_put.py $(DISK_IMG) $(ARM64_UDIR)/filebrowser.elf /bin/filebrowser
	# --- Audio Player (C++ GUI, audio stubbed on arm64) ---
	$(CC) $(ARM64_UCFLAGS) -Ilib/mp3 -c lib/mp3/mp3dec.c -o $(ARM64_UDIR)/mp3dec.o
	clang++ $(ARM64_UCFLAGS) -std=c++20 -fno-exceptions -fno-rtti -Ilib/svg -Iuser -Ilib/mp3 \
	    -c user/audioplayer/audioplayer.cpp -o $(ARM64_UDIR)/audioplayer.o
	$(LLVM_LLD) -nostdlib -static -T user/arm64/link.ld -o $(ARM64_UDIR)/audioplayer.elf \
	    $(ARM64_UDIR)/crt0.o $(ARM64_UDIR)/audioplayer.o $(ARM64_UDIR)/vexui.o \
	    $(ARM64_UDIR)/svg.o $(ARM64_UDIR)/mp3dec.o $(ARM64_UDIR)/libc.a
	python3 scripts/ext2_put.py $(DISK_IMG) $(ARM64_UDIR)/audioplayer.elf /bin/audioplayer
	@echo "arm64 user programs: /bin/hello /bin/gfxdemo /bin/inputtest /bin/desktop /bin/wallpaper /bin/dock /bin/topbar /bin/taskmgr /bin/sh /bin/terminal /bin/filebrowser /bin/sysinfo /bin/audioplayer + utils"
	# --- Stubs for apps not yet ported (standalone, own _start, replaces x86 ELFs) ---
	$(CC) $(ARM64_UCFLAGS) -c user/arm64/stub_doom.c -o $(ARM64_UDIR)/stub.o
	$(LLVM_LLD) -nostdlib -static -T user/arm64/link.ld -o $(ARM64_UDIR)/stub.elf \
	    $(ARM64_UDIR)/stub.o
	python3 scripts/ext2_put.py $(DISK_IMG) $(ARM64_UDIR)/stub.elf /bin/doom
	python3 scripts/ext2_put.py $(DISK_IMG) $(ARM64_UDIR)/stub.elf /bin/browser
	python3 scripts/ext2_put.py $(DISK_IMG) $(ARM64_UDIR)/stub.elf /bin/cpptest
	# --- DOOM (doomgeneric + VibeOS platform layer) ---
	@mkdir -p $(ARM64_UDIR)/doom
	@for src in $$(ls third_party/doomgeneric/doomgeneric/*.c 2>/dev/null | grep -v 'doomgeneric_\|i_sdl\|i_allegro\|i_sound\|i_music\|midifile\|mus2mid\|gusconf\|i_cd'); do \
	    $(CC) $(ARM64_DOOM_CFLAGS) -c $$src -o $(ARM64_UDIR)/doom/$$(basename $$src .c).o || exit 1; \
	done
	$(CC) $(ARM64_DOOM_CFLAGS) -c user/doom/doomgeneric_vibeos.c -o $(ARM64_UDIR)/doom/doomgeneric_vibeos.o
	$(CC) $(ARM64_DOOM_CFLAGS) -c user/doom/i_sound.c            -o $(ARM64_UDIR)/doom/i_sound.o
	$(LLVM_LLD) -nostdlib -static -T user/arm64/link.ld -o $(ARM64_UDIR)/doom/doom.elf \
	    $(ARM64_UDIR)/crt0.o $(ARM64_UDIR)/doom/*.o $(ARM64_UDIR)/libc.a
	python3 scripts/ext2_put.py $(DISK_IMG) $(ARM64_UDIR)/doom/doom.elf /bin/doom

# ARM64 DOOM flags: same sources as x86, but with arm64 target + doom-specific warnings
ARM64_DOOM_CFLAGS := $(ARM64_UCFLAGS) -Ithird_party/doomgeneric/doomgeneric -Iuser/doom \
  -Wno-unused-function -Wno-unused-variable -Wno-unused-parameter \
  -Wno-missing-field-initializers -Wno-missing-braces \
  -Wno-gnu-zero-variadic-macro-arguments -Wno-shift-negative-value \
  -Wno-implicit-int-conversion -Wno-sign-conversion -Wno-conversion

# ============================================================

bump-version:
	@./scripts/bump_version.sh

# Userspace app toolchain (separate ELF binaries, NOT compiled into the kernel).
UCC := clang
# Userspace MAY use SSE/float (the kernel enables SSE at boot and saves XMM per
# process across switches), which stb_truetype/stb_image and any float code need.
UCFLAGS := -target x86_64-none-elf -ffreestanding -fno-stack-protector -fno-pie -mno-red-zone -mcmodel=small -fno-builtin -Wall -Wextra -std=c11 -O2 -g
UCXXFLAGS := -target x86_64-none-elf -ffreestanding -fno-stack-protector -fno-pie -mno-red-zone -mcmodel=small -fno-builtin -Wall -Wextra -std=c++20 -O2 -fno-exceptions -g
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
	$(UCC) $(UCFLAGS) $(LIBC_INC) -c user/libc/math.S   -o build/user/libc/math.o
	$(UCC) $(UCFLAGS) $(LIBC_INC) -c user/libc/setjmp.S -o build/user/libc/setjmp.o
	$(CXX) $(UCXXFLAGS) $(LIBC_INC) -c user/libc/cxxabi.cpp -o build/user/libc/cxxabi.o
	$(UCC) $(UCFLAGS) $(LIBC_INC) -c user/umalloc.c     -o build/user/libc/umalloc.o
	$(UAR) rcs $(LIBC_A) build/user/libc/sys.o build/user/libc/string.o build/user/libc/stdlib.o build/user/libc/stdio.o build/user/libc/cxxabi.o build/user/libc/umalloc.o build/user/libc/math.o build/user/libc/setjmp.o

libc: $(LIBC_A)

apps: $(DISK_IMG) $(LIBC_A)
	@# If the disk is unformatted (ext2 magic absent) — i.e. a fresh
	@# `make newdisk` chain — format it now so we can install apps
	@# without a QEMU round-trip. The kernel's on-disk format is identical
	@# (format_disk.py mirrors ext2_format() in kernel/src/ext2_fs.c).
	@if [ "$$(python3 -c "import struct; d=open('$(DISK_IMG)','rb'); d.seek(0x438); print(struct.unpack('<H', d.read(2))[0])")" != "61395" ]; then \
	    python3 scripts/format_disk.py $(DISK_IMG); \
	fi
	@mkdir -p build/user
	$(UCC) $(UCFLAGS) $(LIBC_INC) -Ilib/svg -c lib/svg/svg.c -o build/user/svg.o
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
	$(CXX) $(UCXXFLAGS) $(LIBC_INC) -Iuser -c user/filedialog/filedialog.cpp -o build/user/filedialog.o
	$(CXX) $(UCXXFLAGS) $(LIBC_INC) -Iuser -c user/filedialog/main.cpp -o build/user/filedialog_main.o
	$(LD) -nostdlib -static -T user/linker.ld -o build/user/filedialog.elf \
		$(LIBC_CRT0) build/user/filedialog.o build/user/filedialog_main.o \
		build/user/vexui.o build/user/svg.o $(LIBC_A)
	$(USTRIP) --strip-all build/user/filedialog.elf
	python3 scripts/ext2_put.py $(DISK_IMG) build/user/filedialog.elf /bin/filedialog
	$(CXX) $(UCXXFLAGS) $(LIBC_INC) -Iuser -c user/texteditor/texteditor.cpp -o build/user/texteditor.o
	$(CXX) $(UCXXFLAGS) $(LIBC_INC) -Iuser -c user/texteditor/main.cpp -o build/user/texteditor_main.o
	$(LD) -nostdlib -static -T user/linker.ld -o build/user/texteditor.elf \
		$(LIBC_CRT0) build/user/texteditor.o build/user/texteditor_main.o \
		build/user/vexui.o build/user/svg.o $(LIBC_A)
	$(USTRIP) --strip-all build/user/texteditor.elf
	python3 scripts/ext2_put.py $(DISK_IMG) build/user/texteditor.elf /bin/texteditor
	$(CXX) $(UCXXFLAGS) $(LIBC_INC) -Iuser -Ilib/svg -c user/filebrowser/filebrowser.cpp -o build/user/filebrowser.o
	$(CXX) $(UCXXFLAGS) $(LIBC_INC) -Iuser -Ilib/svg -c user/filebrowser/main.cpp        -o build/user/filebrowser_main.o
	$(LD) -nostdlib -static -T user/linker.ld -o build/user/filebrowser.elf \
		$(LIBC_CRT0) build/user/filebrowser.o build/user/filebrowser_main.o \
		build/user/vexui.o build/user/svg.o $(LIBC_A)
#	$(USTRIP) --strip-all build/user/filebrowser.elf
	python3 scripts/ext2_put.py $(DISK_IMG) build/user/filebrowser.elf /bin/filebrowser
	$(CXX) $(UCXXFLAGS) $(LIBC_INC) -Iuser -c user/dock/dock.cpp -o build/user/dock.o
	$(LD) -nostdlib -static -T user/linker.ld -o build/user/dock.elf \
		$(LIBC_CRT0) build/user/dock.o build/user/vexui.o build/user/svg.o $(LIBC_A)
	$(USTRIP) --strip-all build/user/dock.elf
	python3 scripts/ext2_put.py $(DISK_IMG) build/user/dock.elf /bin/dock
	$(CXX) $(UCXXFLAGS) $(LIBC_INC) -Iuser -c user/terminal/terminal.cpp  -o build/user/terminal.o
	$(CXX) $(UCXXFLAGS) $(LIBC_INC) -Iuser -c user/terminal/text_grid.cpp -o build/user/terminal_text_grid.o
	$(LD) -nostdlib -static -T user/linker.ld -o build/user/terminal.elf \
		$(LIBC_CRT0) build/user/terminal.o build/user/terminal_text_grid.o build/user/vexui.o build/user/svg.o $(LIBC_A)
	$(USTRIP) --strip-all build/user/terminal.elf
	python3 scripts/ext2_put.py $(DISK_IMG) build/user/terminal.elf /bin/terminal
	$(CXX) $(UCXXFLAGS) $(LIBC_INC) -Ilib/svg -c user/topbar/topbar.cpp -o build/user/topbar.o
	$(LD) -nostdlib -static -T user/linker.ld -o build/user/topbar.elf \
		$(LIBC_CRT0) build/user/topbar.o build/user/svg.o $(LIBC_A)
	$(USTRIP) --strip-all build/user/topbar.elf
	python3 scripts/ext2_put.py $(DISK_IMG) build/user/topbar.elf /bin/topbar
	$(UCC) $(UCFLAGS) $(LIBC_INC) -Iuser/browser -c user/browser/weblayout.c  -o build/user/weblayout.o
	$(UCC) $(UCFLAGS) $(LIBC_INC) -Iuser/browser -c user/browser/dom.c        -o build/user/dom.o
	$(UCC) $(UCFLAGS) $(LIBC_INC) -Iuser/browser -c user/browser/css.c        -o build/user/css.o
	$(UCC) $(UCFLAGS) $(LIBC_INC) -Iuser/browser -Ithird_party/stb -c user/browser/appfont.c   -o build/user/appfont.o
	$(UCC) $(UCFLAGS) $(LIBC_INC) -Iuser/libimage -Ithird_party/stb -c user/libimage/image.c  -o build/user/image.o
	$(CC)  $(ASFLAGS)                             -c user/browser/font_data.S  -o build/user/font_data.o
	$(CXX) $(UCXXFLAGS) $(LIBC_INC) -Iuser -Iuser/browser -c user/browser/layout_engine.cpp -o build/user/browser_layout_engine.o
	$(UCC) $(UCFLAGS) $(LIBC_INC) -Ithird_party/quickjs -DCONFIG_VERSION="\"2025-01-05\"" -c third_party/quickjs/quickjs.c -o build/user/quickjs.o
	$(UCC) $(UCFLAGS) $(LIBC_INC) -Ithird_party/quickjs -c third_party/quickjs/cutils.c -o build/user/quickjs_cutils.o
	$(UCC) $(UCFLAGS) $(LIBC_INC) -Ithird_party/quickjs -c third_party/quickjs/dtoa.c -o build/user/quickjs_dtoa.o
	$(UCC) $(UCFLAGS) $(LIBC_INC) -Ithird_party/quickjs -c third_party/quickjs/libregexp.c -o build/user/quickjs_libregexp.o
	$(UCC) $(UCFLAGS) $(LIBC_INC) -Ithird_party/quickjs -c third_party/quickjs/libunicode.c -o build/user/quickjs_libunicode.o
	$(CXX) $(UCXXFLAGS) $(LIBC_INC) -Iuser -Iuser/browser -Iuser/libimage -Ilib/svg -Ilib/mp3 -Ithird_party/quickjs -c user/browser/browser.cpp       -o build/user/browser_main.o
	$(UCC) $(UCFLAGS) $(LIBC_INC) -Ilib/mp3 -c lib/mp3/mp3dec.c -o build/user/mp3dec.o
	$(LD) -nostdlib -static -T user/linker.ld -o build/user/browser.elf \
		$(LIBC_CRT0) build/user/browser_main.o build/user/browser_layout_engine.o \
		build/user/weblayout.o build/user/dom.o build/user/css.o \
		build/user/appfont.o build/user/image.o build/user/font_data.o \
		build/user/vexui.o build/user/svg.o build/user/mp3dec.o \
		build/user/quickjs.o build/user/quickjs_cutils.o build/user/quickjs_dtoa.o \
		build/user/quickjs_libregexp.o build/user/quickjs_libunicode.o $(LIBC_A)
	$(USTRIP) --strip-all build/user/browser.elf
	python3 scripts/ext2_put.py $(DISK_IMG) build/user/browser.elf /bin/browser
	$(CXX) $(UCXXFLAGS) $(LIBC_INC) -Iuser -Iuser/libimage -Ilib/svg -Ilib/mp3 -c user/audioplayer/audioplayer.cpp -o build/user/audioplayer.o
	$(LD) -nostdlib -static -T user/linker.ld -o build/user/audioplayer.elf \
		$(LIBC_CRT0) build/user/audioplayer.o build/user/vexui.o build/user/svg.o build/user/mp3dec.o $(LIBC_A)
	$(USTRIP) --strip-all build/user/audioplayer.elf
	python3 scripts/ext2_put.py $(DISK_IMG) build/user/audioplayer.elf /bin/audioplayer
	$(UCC) $(UCFLAGS) $(LIBC_INC) -Iuser/libimage -c user/wallpaper/wallpaper.c -o build/user/wallpaper.o
	$(LD) -nostdlib -static -T user/linker.ld -o build/user/wallpaper.elf \
		$(LIBC_CRT0) build/user/wallpaper.o build/user/image.o $(LIBC_A)
	$(USTRIP) --strip-all build/user/wallpaper.elf
	python3 scripts/ext2_put.py $(DISK_IMG) build/user/wallpaper.elf /bin/wallpaper
	python3 scripts/png_to_vwp.py assets/wallpapers/default.png build/user/default.vwp
	python3 scripts/ext2_put.py $(DISK_IMG) build/user/default.vwp /wallpapers/default.vwp
	python3 scripts/ext2_put.py $(DISK_IMG) assets/wallpapers/default.png /wallpapers/default.png
	python3 scripts/ext2_put.py $(DISK_IMG) assets/icons/dock/browser.svg /icons/dock/browser.svg
	python3 scripts/ext2_put.py $(DISK_IMG) assets/icons/dock/filebrowser.svg /icons/dock/filebrowser.svg
	python3 scripts/ext2_put.py $(DISK_IMG) assets/icons/dock/taskmgr.svg /icons/dock/taskmgr.svg
	python3 scripts/ext2_put.py $(DISK_IMG) assets/icons/dock/terminal.svg /icons/dock/terminal.svg
	python3 scripts/ext2_put.py $(DISK_IMG) assets/icons/dock/player.svg /icons/dock/player.svg
	python3 scripts/ext2_put.py $(DISK_IMG) assets/icons/search.svg /icons/search.svg
	python3 scripts/ext2_put.py $(DISK_IMG) assets/icons/vibeos-logo.svg /icons/vibeos-logo.svg
	python3 scripts/ext2_put.py $(DISK_IMG) assets/icons/power.svg /icons/power.svg
	$(USTRIP) --strip-all build/user/sh.elf
	python3 scripts/ext2_put.py $(DISK_IMG) build/user/sh.elf /bin/sh.tmp
	# Use a trick to rename sh.tmp to sh by running a shell command inside the ext2 image if possible, or just overwrite it if the tool supports it.
	# Assuming ext2_put overwrites, the issue might be that /bin/sh still exists.
	# Let's trust ext2_put.py or just rename it.
	python3 scripts/ext2_put.py $(DISK_IMG) build/user/sh.elf /bin/sh
	$(UCC) $(UCFLAGS) $(LIBC_INC) -c user/hello.c -o build/user/hello.o
	$(LD) -nostdlib -static -T user/linker.ld -o build/user/hello.elf $(LIBC_CRT0) build/user/hello.o $(LIBC_A)
	$(USTRIP) --strip-all build/user/hello.elf
	python3 scripts/ext2_put.py $(DISK_IMG) build/user/hello.elf /bin/hello
	$(UCC) $(UCFLAGS) $(LIBC_INC) -c user/echo.c -o build/user/echo.o
	$(LD) -nostdlib -static -T user/linker.ld -o build/user/echo.elf $(LIBC_CRT0) build/user/echo.o $(LIBC_A)
	$(USTRIP) --strip-all build/user/echo.elf
	python3 scripts/ext2_put.py $(DISK_IMG) build/user/echo.elf /bin/echo
	$(UCC) $(UCFLAGS) $(LIBC_INC) -c user/mkdir.c -o build/user/mkdir.o
	$(LD) -nostdlib -static -T user/linker.ld -o build/user/mkdir.elf $(LIBC_CRT0) build/user/mkdir.o $(LIBC_A)
	$(USTRIP) --strip-all build/user/mkdir.elf
	python3 scripts/ext2_put.py $(DISK_IMG) build/user/mkdir.elf /bin/mkdir
	$(UCC) $(UCFLAGS) $(LIBC_INC) -c user/rm.c -o build/user/rm.o
	$(LD) -nostdlib -static -T user/linker.ld -o build/user/rm.elf $(LIBC_CRT0) build/user/rm.o $(LIBC_A)
	$(USTRIP) --strip-all build/user/rm.elf
	python3 scripts/ext2_put.py $(DISK_IMG) build/user/rm.elf /bin/rm
	$(UCC) $(UCFLAGS) $(LIBC_INC) -c user/touch.c -o build/user/touch.o
	$(LD) -nostdlib -static -T user/linker.ld -o build/user/touch.elf $(LIBC_CRT0) build/user/touch.o $(LIBC_A)
	$(USTRIP) --strip-all build/user/touch.elf
	python3 scripts/ext2_put.py $(DISK_IMG) build/user/touch.elf /bin/touch
	$(UCC) $(UCFLAGS) $(LIBC_INC) -c user/cat.c -o build/user/cat.o
	$(LD) -nostdlib -static -T user/linker.ld -o build/user/cat.elf $(LIBC_CRT0) build/user/cat.o $(LIBC_A)
	$(USTRIP) --strip-all build/user/cat.elf
	python3 scripts/ext2_put.py $(DISK_IMG) build/user/cat.elf /bin/cat
	$(UCC) $(UCFLAGS) $(LIBC_INC) -c user/cp.c -o build/user/cp.o
	$(LD) -nostdlib -static -T user/linker.ld -o build/user/cp.elf $(LIBC_CRT0) build/user/cp.o $(LIBC_A)
	$(USTRIP) --strip-all build/user/cp.elf
	python3 scripts/ext2_put.py $(DISK_IMG) build/user/cp.elf /bin/cp
	$(UCC) $(UCFLAGS) $(LIBC_INC) -c user/mv.c -o build/user/mv.o
	$(LD) -nostdlib -static -T user/linker.ld -o build/user/mv.elf $(LIBC_CRT0) build/user/mv.o $(LIBC_A)
	$(USTRIP) --strip-all build/user/mv.elf
	python3 scripts/ext2_put.py $(DISK_IMG) build/user/mv.elf /bin/mv
	$(UCC) $(UCFLAGS) $(LIBC_INC) -c user/ln.c -o build/user/ln.o
	$(LD) -nostdlib -static -T user/linker.ld -o build/user/ln.elf $(LIBC_CRT0) build/user/ln.o $(LIBC_A)
	$(USTRIP) --strip-all build/user/ln.elf
	python3 scripts/ext2_put.py $(DISK_IMG) build/user/ln.elf /bin/ln
	$(UCC) $(UCFLAGS) $(LIBC_INC) -c user/stat.c -o build/user/stat.o
	$(LD) -nostdlib -static -T user/linker.ld -o build/user/stat.elf $(LIBC_CRT0) build/user/stat.o $(LIBC_A)
	$(USTRIP) --strip-all build/user/stat.elf
	python3 scripts/ext2_put.py $(DISK_IMG) build/user/stat.elf /bin/stat
	$(UCC) $(UCFLAGS) $(LIBC_INC) -c user/su.c -o build/user/su.o
	$(LD) -nostdlib -static -T user/linker.ld -o build/user/su.elf $(LIBC_CRT0) build/user/su.o $(LIBC_A)
	$(USTRIP) --strip-all build/user/su.elf
	python3 scripts/ext2_put.py $(DISK_IMG) build/user/su.elf /bin/su
	python3 scripts/ext2_put.py $(DISK_IMG) assets/shadow /etc/shadow
	$(UCC) $(UCFLAGS) $(LIBC_INC) -c user/chmod.c  -o build/user/chmod.o
	$(LD) -nostdlib -static -T user/linker.ld -o build/user/chmod.elf  $(LIBC_CRT0) build/user/chmod.o  $(LIBC_A)
	$(USTRIP) --strip-all build/user/chmod.elf
	python3 scripts/ext2_put.py $(DISK_IMG) build/user/chmod.elf /bin/chmod
	$(UCC) $(UCFLAGS) $(LIBC_INC) -c user/chown.c  -o build/user/chown.o
	$(LD) -nostdlib -static -T user/linker.ld -o build/user/chown.elf  $(LIBC_CRT0) build/user/chown.o  $(LIBC_A)
	$(USTRIP) --strip-all build/user/chown.elf
	python3 scripts/ext2_put.py $(DISK_IMG) build/user/chown.elf /bin/chown
	$(UCC) $(UCFLAGS) $(LIBC_INC) -c user/whoami.c -o build/user/whoami.o
	$(LD) -nostdlib -static -T user/linker.ld -o build/user/whoami.elf $(LIBC_CRT0) build/user/whoami.o $(LIBC_A)
	$(USTRIP) --strip-all build/user/whoami.elf
	python3 scripts/ext2_put.py $(DISK_IMG) build/user/whoami.elf /bin/whoami
	$(UCC) $(UCFLAGS) $(LIBC_INC) -c user/id.c     -o build/user/id.o
	$(LD) -nostdlib -static -T user/linker.ld -o build/user/id.elf     $(LIBC_CRT0) build/user/id.o     $(LIBC_A)
	$(USTRIP) --strip-all build/user/id.elf
	python3 scripts/ext2_put.py $(DISK_IMG) build/user/id.elf /bin/id
	# Seed /etc/filetypes (file type → action mapping for the file browser).
	python3 scripts/ext2_put.py $(DISK_IMG) assets/filetypes /etc/filetypes
	# Seed /etc/passwd with the root user entry if not already present
	python3 scripts/ext2_put.py $(DISK_IMG) assets/passwd /etc/passwd
	# Seed /tmp so it exists from first boot (ext2_put creates the dir automatically)
	python3 scripts/ext2_put.py $(DISK_IMG) assets/empty /tmp/.keep
	$(UCC) $(UCFLAGS) $(LIBC_INC) -c user/wc.c    -o build/user/wc.o
	$(LD) -nostdlib -static -T user/linker.ld -o build/user/wc.elf    $(LIBC_CRT0) build/user/wc.o    $(LIBC_A)
	$(USTRIP) --strip-all build/user/wc.elf
	python3 scripts/ext2_put.py $(DISK_IMG) build/user/wc.elf /bin/wc
	$(UCC) $(UCFLAGS) $(LIBC_INC) -c user/clipboard.c -o build/user/clipboard.o
	$(LD) -nostdlib -static -T user/linker.ld -o build/user/clipboard.elf $(LIBC_CRT0) build/user/clipboard.o $(LIBC_A)
	$(USTRIP) --strip-all build/user/clipboard.elf
	python3 scripts/ext2_put.py $(DISK_IMG) build/user/clipboard.elf /bin/clipboard
	$(UCC) $(UCFLAGS) $(LIBC_INC) -c user/head.c  -o build/user/head.o
	$(LD) -nostdlib -static -T user/linker.ld -o build/user/head.elf  $(LIBC_CRT0) build/user/head.o  $(LIBC_A)
	$(USTRIP) --strip-all build/user/head.elf
	python3 scripts/ext2_put.py $(DISK_IMG) build/user/head.elf /bin/head
	$(UCC) $(UCFLAGS) $(LIBC_INC) -c user/tail.c  -o build/user/tail.o
	$(LD) -nostdlib -static -T user/linker.ld -o build/user/tail.elf  $(LIBC_CRT0) build/user/tail.o  $(LIBC_A)
	$(USTRIP) --strip-all build/user/tail.elf
	python3 scripts/ext2_put.py $(DISK_IMG) build/user/tail.elf /bin/tail
	$(UCC) $(UCFLAGS) $(LIBC_INC) -c user/grep.c  -o build/user/grep.o
	$(LD) -nostdlib -static -T user/linker.ld -o build/user/grep.elf  $(LIBC_CRT0) build/user/grep.o  $(LIBC_A)
	$(USTRIP) --strip-all build/user/grep.elf
	python3 scripts/ext2_put.py $(DISK_IMG) build/user/grep.elf /bin/grep
	$(UCC) $(UCFLAGS) $(LIBC_INC) -c user/find.c  -o build/user/find.o
	$(LD) -nostdlib -static -T user/linker.ld -o build/user/find.elf  $(LIBC_CRT0) build/user/find.o  $(LIBC_A)
	$(USTRIP) --strip-all build/user/find.elf
	python3 scripts/ext2_put.py $(DISK_IMG) build/user/find.elf /bin/find
	$(UCC) $(UCFLAGS) $(LIBC_INC) -c user/pwd.c   -o build/user/pwd.o
	$(LD) -nostdlib -static -T user/linker.ld -o build/user/pwd.elf   $(LIBC_CRT0) build/user/pwd.o   $(LIBC_A)
	$(USTRIP) --strip-all build/user/pwd.elf
	python3 scripts/ext2_put.py $(DISK_IMG) build/user/pwd.elf /bin/pwd
	$(UCC) $(UCFLAGS) $(LIBC_INC) -c user/audiotest.c -o build/user/audiotest.o
	$(LD) -nostdlib -static -T user/linker.ld -o build/user/audiotest.elf \
		$(LIBC_CRT0) build/user/audiotest.o $(LIBC_A)
	$(USTRIP) --strip-all build/user/audiotest.elf
	python3 scripts/ext2_put.py $(DISK_IMG) build/user/audiotest.elf /bin/audiotest
	$(UCC) $(UCFLAGS) $(LIBC_INC) -Ilib/mp3 -c lib/mp3/mp3dec.c  -o build/user/mp3dec.o
	$(UCC) $(UCFLAGS) $(LIBC_INC) -Ilib/mp3 -c user/mp3play.c    -o build/user/mp3play.o
	$(LD) -nostdlib -static -T user/linker.ld -o build/user/mp3play.elf \
		$(LIBC_CRT0) build/user/mp3play.o build/user/mp3dec.o $(LIBC_A)
	$(USTRIP) --strip-all build/user/mp3play.elf
	python3 scripts/ext2_put.py $(DISK_IMG) build/user/mp3play.elf /bin/mp3play
	python3 scripts/ext2_put.py $(DISK_IMG) assets/music/becorbal-town.mp3 /music/becorbal-town.mp3
	python3 scripts/ext2_put.py $(DISK_IMG) assets/music/player.html /music/player.html
	python3 scripts/ext2_put.py $(DISK_IMG) assets/test.html /test.html
	$(UCC) $(UCFLAGS) $(LIBC_INC) -c user/audiocfg.c -o build/user/audiocfg.o
	$(LD) -nostdlib -static -T user/linker.ld -o build/user/audiocfg.elf \
		$(LIBC_CRT0) build/user/audiocfg.o build/user/libc.a
	$(USTRIP) --strip-all build/user/audiocfg.elf
	python3 scripts/ext2_put.py $(DISK_IMG) build/user/audiocfg.elf /bin/audiocfg
	$(CXX) $(UCXXFLAGS) $(LIBC_INC) -c user/threadtest.cpp -o build/user/threadtest.o
	$(LD) -nostdlib -static -T user/linker.ld -o build/user/threadtest.elf $(LIBC_CRT0) build/user/threadtest.o $(LIBC_A)
	$(USTRIP) --strip-all build/user/threadtest.elf
	python3 scripts/ext2_put.py $(DISK_IMG) build/user/threadtest.elf /bin/threadtest
	$(CXX) $(UCXXFLAGS) $(LIBC_INC) -c user/cpptest.cpp -o build/user/cpptest.o
	$(LD) -nostdlib -static -T user/linker.ld -o build/user/cpptest.elf $(LIBC_CRT0) build/user/cpptest.o $(LIBC_A)
	$(USTRIP) --strip-all build/user/cpptest.elf
	python3 scripts/ext2_put.py $(DISK_IMG) build/user/cpptest.elf /bin/cpptest
	$(CXX) $(UCXXFLAGS) $(LIBC_INC) -c user/adduser.cpp -o build/user/adduser.o
	$(LD) -nostdlib -static -T user/linker.ld -o build/user/adduser.elf $(LIBC_CRT0) build/user/adduser.o $(LIBC_A)
	$(USTRIP) --strip-all build/user/adduser.elf
	python3 scripts/ext2_put.py $(DISK_IMG) build/user/adduser.elf /bin/adduser
	$(MAKE) doom
	@echo "Installed apps to $(DISK_IMG). They load from disk at runtime."

DOOM_SRCS := $(shell ls third_party/doomgeneric/doomgeneric/*.c 2>/dev/null \
  | grep -v 'doomgeneric_\|i_sdl\|i_allegro\|i_sound\|i_music\|midifile\|mus2mid\|gusconf\|i_cd')
DOOM_OBJS := $(patsubst third_party/doomgeneric/doomgeneric/%.c,build/user/doom/%.o,$(DOOM_SRCS)) \
             build/user/doom/doomgeneric_vibeos.o build/user/doom/i_sound.o
DOOM_CFLAGS := $(UCFLAGS) $(LIBC_INC) \
  -Ithird_party/doomgeneric/doomgeneric -Iuser/doom \
  -Wno-unused-function -Wno-unused-variable -Wno-unused-parameter \
  -Wno-missing-field-initializers -Wno-missing-braces \
  -Wno-gnu-zero-variadic-macro-arguments -Wno-shift-negative-value \
  -Wno-implicit-int-conversion -Wno-sign-conversion -Wno-conversion

doom: $(DISK_IMG) $(LIBC_A)
	@mkdir -p build/user/doom
	$(foreach src,$(DOOM_SRCS),$(UCC) $(DOOM_CFLAGS) \
		-c $(src) -o $(patsubst third_party/doomgeneric/doomgeneric/%.c,build/user/doom/%.o,$(src)) &&) true
	$(UCC) $(DOOM_CFLAGS) -c user/doom/doomgeneric_vibeos.c -o build/user/doom/doomgeneric_vibeos.o
	$(UCC) $(DOOM_CFLAGS) -c user/doom/i_sound.c            -o build/user/doom/i_sound.o
	$(LD) -nostdlib -static -T user/linker.ld -o build/user/doom.elf \
		$(LIBC_CRT0) $(DOOM_OBJS) $(LIBC_A)
	$(USTRIP) --strip-all build/user/doom.elf
	python3 scripts/ext2_put.py $(DISK_IMG) build/user/doom.elf /bin/doom
	python3 scripts/ext2_put.py $(DISK_IMG) assets/doom1.wad /games/doom1.wad
	@echo "DOOM installed to $(DISK_IMG)."

# ---- arm64 TCC (Tiny C Compiler) -------------------------------------------
# Native aarch64 TCC that generates aarch64 code. ONE_SOURCE build from
# the same tcc.c as x86, with TCC_TARGET_ARM64 config. Needs setjmp/longjmp
# (arm64 setjmp.S) and soft-float stubs (arm64 softfloat.S) already in libc.
ARM64_TCC_CFLAGS := $(ARM64_UCFLAGS) -Ibuild/arm64/tcc_config -Ithird_party/tcc
arm64-tcc: arm64-user
	@mkdir -p build/arm64/tcc_config
	@echo '#define TCC_TARGET_ARM64 1'   >  build/arm64/tcc_config/config.h
	@echo '#define TCC_VERSION "0.9.28rc-vibeos"' >> build/arm64/tcc_config/config.h
	@echo '#define CONFIG_TCC_STATIC 1'   >> build/arm64/tcc_config/config.h
	@echo '#define CONFIG_TCCDIR "/lib/tcc"' >> build/arm64/tcc_config/config.h
	@echo '#define CONFIG_TCC_CRTPREFIX "/lib"' >> build/arm64/tcc_config/config.h
	@echo '#define CONFIG_TCC_SYSINCLUDEPATHS "{B}/include:/usr/include"' >> build/arm64/tcc_config/config.h
	@echo '#define CONFIG_TCC_LIBPATHS "{B}:/lib"' >> build/arm64/tcc_config/config.h
	@echo '#define CONFIG_TCC_ELFINTERP ""' >> build/arm64/tcc_config/config.h
	@echo '#define CONFIG_TCC_SEMLOCK 0'   >> build/arm64/tcc_config/config.h
	@echo '#define CONFIG_DWARF_VERSION 4' >> build/arm64/tcc_config/config.h
	$(CC) $(ARM64_TCC_CFLAGS) -c third_party/tcc/tcc.c -o $(ARM64_UDIR)/tcc.o
	$(LLVM_LLD) -nostdlib -static -T user/arm64/link.ld -o $(ARM64_UDIR)/tcc.elf \
	    $(ARM64_UDIR)/crt0.o $(ARM64_UDIR)/tcc.o $(ARM64_UDIR)/libc.a
	python3 scripts/ext2_put.py $(DISK_IMG) $(ARM64_UDIR)/tcc.elf /bin/tcc
	# Predefs header (same file for all targets — loaded at runtime)
	python3 scripts/ext2_put.py $(DISK_IMG) third_party/tcc/include/tccdefs.h /usr/include/tccdefs.h
	# TCC's own freestanding headers
	@for f in third_party/tcc/include/*.h; do \
	    bn=$$(basename "$$f"); \
	    python3 scripts/ext2_put.py $(DISK_IMG) "$$f" "/lib/tcc/include/$$bn" ; \
	done
	# arm64 libtcc1.a (TCC runtime library)
	$(CC) $(ARM64_TCC_CFLAGS) -c third_party/tcc/lib/libtcc1.c   -o $(ARM64_UDIR)/lt1_libtcc1.o
	$(CC) $(ARM64_TCC_CFLAGS) -c third_party/tcc/lib/lib-arm64.c  -o $(ARM64_UDIR)/lt1_lib-arm64.o
	$(CC) $(ARM64_TCC_CFLAGS) -c third_party/tcc/lib/va_list.c   -o $(ARM64_UDIR)/lt1_va_list.o
	$(CC) $(ARM64_TCC_CFLAGS) -c third_party/tcc/lib/builtin.c   -o $(ARM64_UDIR)/lt1_builtin.o
	$(CC) $(ARM64_TCC_CFLAGS) -c third_party/tcc/lib/alloca.S    -o $(ARM64_UDIR)/lt1_alloca.o
	$(CC) $(ARM64_TCC_CFLAGS) -c third_party/tcc/lib/alloca-bt.S -o $(ARM64_UDIR)/lt1_alloca_bt.o
	$(CC) $(ARM64_TCC_CFLAGS) -c third_party/tcc/lib/stdatomic.c -o $(ARM64_UDIR)/lt1_stdatomic.o
	$(CC) $(ARM64_TCC_CFLAGS) -c third_party/tcc/lib/atomic.S    -o $(ARM64_UDIR)/lt1_atomic.o
	$(CC) $(ARM64_TCC_CFLAGS) -c third_party/tcc/lib/armflush.c  -o $(ARM64_UDIR)/lt1_armflush.o
	$(CC) $(ARM64_TCC_CFLAGS) -c third_party/tcc/lib/dsohandle.c -o $(ARM64_UDIR)/lt1_dsohandle.o
	$(UAR) rcs $(ARM64_UDIR)/libtcc1.a \
	    $(ARM64_UDIR)/lt1_libtcc1.o $(ARM64_UDIR)/lt1_lib-arm64.o \
	    $(ARM64_UDIR)/lt1_va_list.o $(ARM64_UDIR)/lt1_builtin.o \
	    $(ARM64_UDIR)/lt1_alloca.o $(ARM64_UDIR)/lt1_alloca_bt.o \
	    $(ARM64_UDIR)/lt1_stdatomic.o $(ARM64_UDIR)/lt1_atomic.o \
	    $(ARM64_UDIR)/lt1_armflush.o $(ARM64_UDIR)/lt1_dsohandle.o
	python3 scripts/ext2_put.py $(DISK_IMG) $(ARM64_UDIR)/libtcc1.a /lib/tcc/libtcc1.a
	@echo "arm64 TCC (515 KB) installed to $(DISK_IMG)."

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

# Check the disk image for ext2 inconsistencies (double-allocated blocks, bitmap
# vs free-count drift, broken directories, orphaned inodes). Read-only.
verify-disk: $(DISK_IMG)
	python3 scripts/ext2_fsck.py $(DISK_IMG)

# Repair the disk image in place: rebuild bitmaps + free counts from the live
# inode/directory tree (drops orphaned inodes and stale bitmap bits). Run with
# QEMU stopped. Directory rec_len damage is reported but not auto-fixed.
fsck-disk: $(DISK_IMG)
	python3 scripts/ext2_fsck.py $(DISK_IMG) --repair

all: bump-version kernel user apps

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
	$(QEMU) -boot d -cdrom $(OUT_DIR)/vibeos.iso -m $(QEMU_MEM) -vga std -no-reboot -cpu $(QEMU_CPU) $(QEMU_SMP) $(QEMU_ACCEL) $(QEMU_NET) $(QEMU_NET_DUMP) $(QEMU_AUDIO) $(QEMU_DISK)

run-disk: $(OUT_DIR)/vibeos.elf
	$(QEMU) -boot d -cdrom $(OUT_DIR)/vibeos.iso -m $(QEMU_MEM) -vga std -no-reboot -cpu $(QEMU_CPU) $(QEMU_SMP) $(QEMU_ACCEL) $(QEMU_NET) $(QEMU_NET_DUMP) $(QEMU_AUDIO) -drive file=$(OUT_DIR)/disk.img,format=raw,index=0,media=disk

run-disk-serial: $(OUT_DIR)/vibeos.elf
	$(QEMU) -boot d -cdrom $(OUT_DIR)/vibeos.iso -m $(QEMU_MEM) -vga std -no-reboot -cpu $(QEMU_CPU) $(QEMU_SMP) $(QEMU_ACCEL) $(QEMU_NET) $(QEMU_NET_DUMP) $(QEMU_AUDIO) -hda $(OUT_DIR)/disk.img -serial stdio -monitor none

run-debug: $(OUT_DIR)/vibeos.iso $(DISK_IMG)
	$(QEMU) -boot d -cdrom $(OUT_DIR)/vibeos.iso -m $(QEMU_MEM) -vga std -no-reboot -cpu $(QEMU_CPU) $(QEMU_SMP) $(QEMU_ACCEL) $(QEMU_NET) $(QEMU_NET_DUMP) $(QEMU_AUDIO) $(QEMU_DISK) -no-shutdown

run-serial: $(OUT_DIR)/vibeos.iso $(DISK_IMG)
	$(QEMU) -boot d -cdrom $(OUT_DIR)/vibeos.iso -m $(QEMU_MEM) -vga std -no-reboot -cpu $(QEMU_CPU) $(QEMU_SMP) $(QEMU_ACCEL) $(QEMU_NET) $(QEMU_NET_DUMP) $(QEMU_AUDIO) $(QEMU_DISK) -serial stdio -monitor none

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

$(KERNEL_OBJECTS): kernel/include/version.h

kernel/include/version.h:
	@$(MAKE) bump-version

# ---- nano (GNU nano editor, VibeOS port) --------------------------------
NANO_SRCS := $(wildcard third_party/nano/src/*.c)
NANO_OBJS := $(patsubst third_party/nano/src/%.c,build/user/nano/%.o,$(NANO_SRCS))
NANO_CFLAGS := $(UCFLAGS) $(LIBC_INC) \
  -include user/nano/nano_config.h -UHAVE_CONFIG_H -DNDEBUG \
  -Iuser -Iuser/nano -Ithird_party/nano/src \
  -Wno-unused-function -Wno-unused-variable -Wno-unused-parameter \
  -Wno-missing-field-initializers -Wno-missing-braces

nano: $(DISK_IMG) $(LIBC_A)
	@mkdir -p build/user/nano
	$(foreach src,$(NANO_SRCS),$(UCC) $(NANO_CFLAGS) \
		-c $(src) -o $(patsubst third_party/nano/src/%.c,build/user/nano/%.o,$(src)) &&) true
	$(UCC) $(UCFLAGS) -Iuser/libc/include -Iuser/nano -Iuser \
		-c user/nano/vibeos_ncurses.c -o build/user/nano/vibeos_ncurses.o
	$(UCC) $(UCFLAGS) -Iuser/libc/include -Iuser/nano -Iuser \
		-c user/nano/getopt_stub.c -o build/user/nano/getopt_stub.o
	$(UCC) $(NANO_CFLAGS) -Iuser/nano \
		-c user/nano/vibeos_nano_main.c -o build/user/nano/main.o
		$(LD) -nostdlib -static -T user/linker.ld -o build/user/nano.elf \
			$(LIBC_CRT0) build/user/nano/main.o $(NANO_OBJS) \
			build/user/nano/vibeos_ncurses.o build/user/nano/getopt_stub.o \
			$(LIBC_A)
	$(USTRIP) --strip-all build/user/nano.elf
	python3 scripts/ext2_put.py $(DISK_IMG) build/user/nano.elf /bin/nano
	@echo "nano installed to $(DISK_IMG)."

# ---- tcc (Tiny C Compiler, VibeOS port) ---------------------------------
# ONE_SOURCE mode: tcc.c includes all other .c files → single compilation.
# -DCONFIG_TCC_PREDEFS=0: load predefines from tccdefs.h at runtime.
TCC_CFLAGS := $(UCFLAGS) $(LIBC_INC) -Ithird_party/tcc \
  -Wno-incompatible-function-pointer-types -Wno-unused-function \
  -DCONFIG_TCC_PREDEFS=0

tcc: $(DISK_IMG) $(LIBC_A)
	@mkdir -p build/user
	# Pre-build step: generate tccdefs_.h from tccdefs.h (c2str tool)
	cd third_party/tcc && clang -DC2STR conftest.c -o c2str && ./c2str include/tccdefs.h tccdefs_.h
	$(UCC) $(TCC_CFLAGS) -c third_party/tcc/tcc.c -o build/user/tcc.o
	$(LD) -nostdlib -static -T user/linker.ld -o build/user/tcc.elf \
		$(LIBC_CRT0) build/user/tcc.o $(LIBC_A)
	$(USTRIP) --strip-all build/user/tcc.elf
	python3 scripts/ext2_put.py $(DISK_IMG) build/user/tcc.elf /bin/tcc
	# Seed toolchain: crt0, libc, headers, tccdefs.h
	python3 scripts/ext2_put.py $(DISK_IMG) build/user/libc/crt0.o /lib/crt0.o
	python3 scripts/ext2_put.py $(DISK_IMG) build/user/libc/crt0.o /lib/crt1.o
	# Minimal crti.o / crtn.o — empty objects (TCC needs them to exist but we
	# don't use .init/.fini sections on VibeOS).
	echo '/* empty */' > /tmp/_empty.c
	$(UCC) $(UCFLAGS) -c /tmp/_empty.c -o build/user/crti.o
	cp build/user/crti.o build/user/crtn.o
	python3 scripts/ext2_put.py $(DISK_IMG) build/user/crti.o /lib/crti.o
	python3 scripts/ext2_put.py $(DISK_IMG) build/user/crtn.o /lib/crtn.o
	# TCC runtime library. The x86_64 libtcc1.a is built from the full object
	# set tcc's own lib/Makefile uses (OBJ-x86_64): libtcc1 + COMMON_O + va_list
	# + dsohandle. va_list.o supplies __va_arg, which tcc-compiled code (e.g. tcc
	# itself) references — without it, linking a self-hosted tcc fails.
	$(UCC) $(UCFLAGS) -Ithird_party/tcc -c third_party/tcc/lib/libtcc1.c   -o build/user/libtcc1.o
	$(UCC) $(UCFLAGS) -Ithird_party/tcc -c third_party/tcc/lib/va_list.c   -o build/user/lt1_va_list.o
	$(UCC) $(UCFLAGS) -Ithird_party/tcc -c third_party/tcc/lib/builtin.c   -o build/user/lt1_builtin.o
	$(UCC) $(UCFLAGS) -Ithird_party/tcc -c third_party/tcc/lib/alloca.S    -o build/user/lt1_alloca.o
	$(UCC) $(UCFLAGS) -Ithird_party/tcc -c third_party/tcc/lib/alloca-bt.S -o build/user/lt1_alloca_bt.o
	$(UCC) $(UCFLAGS) -Ithird_party/tcc -c third_party/tcc/lib/stdatomic.c -o build/user/lt1_stdatomic.o
	$(UCC) $(UCFLAGS) -Ithird_party/tcc -c third_party/tcc/lib/atomic.S    -o build/user/lt1_atomic.o
	$(UCC) $(UCFLAGS) -Ithird_party/tcc -c third_party/tcc/lib/dsohandle.c -o build/user/lt1_dsohandle.o
	$(UAR) rcs build/user/libtcc1.a build/user/libtcc1.o build/user/lt1_va_list.o \
		build/user/lt1_builtin.o build/user/lt1_alloca.o build/user/lt1_alloca_bt.o \
		build/user/lt1_stdatomic.o build/user/lt1_atomic.o build/user/lt1_dsohandle.o
	python3 scripts/ext2_put.py $(DISK_IMG) build/user/libtcc1.a /lib/tcc/libtcc1.a
	python3 scripts/ext2_put.py $(DISK_IMG) build/user/libc.a /lib/libc.a
	python3 scripts/ext2_put.py $(DISK_IMG) third_party/tcc/include/tccdefs.h /usr/include/tccdefs.h
	# tcc's own freestanding headers (stdarg.h, stddef.h, ...) belong in
	# {CONFIG_TCCDIR}/include = /lib/tcc/include, which tcc searches FIRST. These
	# are the versions matched to tcc's codegen — essential for self-hosting,
	# where tcc must compile its own source with its own stdarg/stddef.
	@for f in third_party/tcc/include/*.h; do \
	    b=$$(basename $$f); \
	    python3 scripts/ext2_put.py $(DISK_IMG) "$$f" "/lib/tcc/include/$$b" ; \
	done
	# cc: friendly front-end for the on-device tcc (default output naming, -run).
	# Part of the toolchain, so it is built and installed alongside tcc — and
	# relinked here against the just-rebuilt libc.a (it needs vos_waitpid).
	$(UCC) $(UCFLAGS) $(LIBC_INC) -c user/cc.c -o build/user/cc.o
	$(LD) -nostdlib -static -T user/linker.ld -o build/user/cc.elf $(LIBC_CRT0) build/user/cc.o $(LIBC_A)
	$(USTRIP) --strip-all build/user/cc.elf
	python3 scripts/ext2_put.py $(DISK_IMG) build/user/cc.elf /bin/cc
	@echo "tcc installed to $(DISK_IMG)."

# ---- tcc-src: seed tcc's own source tree onto the disk for self-hosting -----
# Puts every tcc .c/.h/.def plus the generated tccdefs_.h under /src/tcc so the
# on-device tcc can compile tcc itself. Run with QEMU stopped. The x86_64
# ONE_SOURCE build only #includes a subset, but seeding all sources keeps the
# include resolution simple and the disk has ample room.
tcc-src: $(DISK_IMG)
	# Ensure the generated predefs header exists (mirrors the `tcc:` recipe).
	cd third_party/tcc && clang -DC2STR conftest.c -o c2str && ./c2str include/tccdefs.h tccdefs_.h
	@for f in third_party/tcc/*.c third_party/tcc/*.h third_party/tcc/*.def; do \
	    b=$$(basename $$f); \
	    python3 scripts/ext2_put.py $(DISK_IMG) "$$f" "/src/tcc/$$b" ; \
	done
	@for f in third_party/tcc/include/*.h; do \
	    b=$$(basename $$f); \
	    python3 scripts/ext2_put.py $(DISK_IMG) "$$f" "/src/tcc/include/$$b" ; \
	done
	# A trivial program to validate a freshly self-hosted tcc.
	printf '#include <stdio.h>\nint main(void){ printf("self-hosted tcc works\\n"); return 0; }\n' > /tmp/_shtest.c
	python3 scripts/ext2_put.py $(DISK_IMG) /tmp/_shtest.c /src/shtest.c
	python3 scripts/ext2_fsck.py $(DISK_IMG)
	@echo "tcc sources seeded to /src/tcc. On VibeOS, compile a single TU with:"
	@echo "  tcc -c -DONE_SOURCE=0 -DCONFIG_TCC_PREDEFS=0 -I/src/tcc -I/usr/include /src/tcc/tccpp.c -o /tmp/tccpp.o"

# ---- seed-headers: copy libc headers to the disk image /usr/include/ ----
seed-headers: $(DISK_IMG)
	@for f in user/libc/include/*.h; do \
		python3 scripts/ext2_put.py $(DISK_IMG) "$$f" "/usr/include/$$(basename $$f)" ; \
	done
	@for f in user/libc/include/sys/*.h; do \
		python3 scripts/ext2_put.py $(DISK_IMG) "$$f" "/usr/include/sys/$$(basename $$f)" ; \
	done
	@echo "Headers seeded to /usr/include/"

# ---- tcc-tests: seed TCC test .c files to /tests/ on the disk image ---------
tcc-tests: $(DISK_IMG)
	@mkdir -p $(OUT_DIR)/user
	# Seed all test source files so TCC can compile them inside VibeOS.
	@for f in user/tests/*.c; do \
		python3 scripts/ext2_put.py $(DISK_IMG) "$$f" "/tests/$$(basename $$f)" ; \
	done
	@echo "TCC test sources seeded to /tests/"

# ---- tcc-test-all: build test_all.elf with the cross-compiler for direct use ----
tcc-test-all: $(DISK_IMG) $(LIBC_A)
	@mkdir -p $(OUT_DIR)/user
	$(UCC) $(UCFLAGS) $(LIBC_INC) -c user/tests/test_all.c -o $(OUT_DIR)/user/test_all.o
	$(LD) -nostdlib -static -T user/linker.ld -o $(OUT_DIR)/user/test_all.elf \
		$(LIBC_CRT0) $(OUT_DIR)/user/test_all.o $(LIBC_A)
	$(USTRIP) --strip-all $(OUT_DIR)/user/test_all.elf
	python3 scripts/ext2_put.py $(DISK_IMG) $(OUT_DIR)/user/test_all.elf /bin/test_all
	@echo "test_all installed to /bin/test_all"

# ---- tcc-vexui: build vexui.o + test_vexui for TCC-based VexUI programs -----
# Seeds vexui.o onto the disk so TCC can link against it inside VibeOS.
UXCFLAGS := $(UCFLAGS) $(LIBC_INC) -Ilib/svg -Wno-unused-parameter -Wno-sign-compare
tcc-vexui: $(DISK_IMG)
	@mkdir -p $(OUT_DIR)/user
	$(UCC) $(UXCFLAGS) -c user/vexui.c -o $(OUT_DIR)/user/vexui.o
	# Also build svg.o (needed by vexui for icons)
	$(UCC) $(UXCFLAGS) -c lib/svg/svg.c -o $(OUT_DIR)/user/svg.o
	# Seed vexui.o + svg.o so TCC can link: tcc prog.c vexui.o svg.o libc.a
	python3 scripts/ext2_put.py $(DISK_IMG) $(OUT_DIR)/user/vexui.o /lib/vexui.o
	python3 scripts/ext2_put.py $(DISK_IMG) $(OUT_DIR)/user/svg.o /lib/svg.o
	# Seed vexui.h so programs can #include <vexui.h>
	python3 scripts/ext2_put.py $(DISK_IMG) user/vexui.h /usr/include/vexui.h
	# Seed test_vexui.c
	python3 scripts/ext2_put.py $(DISK_IMG) user/tests/test_vexui.c /tests/test_vexui.c
	@echo "VexUI .o + header seeded. Compile inside VibeOS:"
	@echo "  tcc /tests/test_vexui.c /lib/vexui.o /lib/svg.o /lib/libc.a -o /tmp/tv"
	@echo "  /tmp/tv"
