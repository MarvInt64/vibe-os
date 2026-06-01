# VibeOS

A hobby x86_64 operating system vibed with AI from scratch in C, C++20 and x86 assembly.
It boots via GRUB/Multiboot2, runs entirely bare-metal, and includes a working graphical desktop, networking stack, persistent ext2 file system, and a userspace with a shell, text editor, browser and GUI apps.

![VibeOS desktop](docs/screen1.jpg)

---

## Features

| Area | Details |
|---|---|
| **Boot** | GRUB Multiboot2 → 32-bit protected mode → 64-bit long mode; VGA text-mode fallback during early init |
| **Memory** | Physical frame allocator, 4-level paging, kernel heap (`kmalloc`/`kfree`), per-process page tables with full address-space isolation |
| **Interrupts** | IDT, PIC remapping, IRQ handlers, `int 0x80` syscall interface (34 syscalls) |
| **Scheduler** | Round-robin preemptive multi-process; FPU/SSE context switch per process (`fxsave`/`fxrstor`) so userspace can freely use floats and SSE |
| **Process management** | `SYS_PROCESS_SPAWN` / `SYS_WAITPID` / `SYS_PROCESS_KILL`; per-process file-descriptor table; `SYS_GETARG` argument passing |
| **Storage** | IDE/PIO driver, ramdisk (in-memory block device for early boot), persistent raw disk image |
| **File system** | ext2 read/write; full VFS layer; `open`/`close`/`read`/`write`/`stat`/`readdir`/`chdir`/`getcwd`/`unlink`/`creat`/`mkdir` syscalls; survives reboots |
| **Networking** | Intel e1000 NIC driver; ARP, IPv4, ICMP (`ping`), UDP, DNS, TCP; HTTP (`curl`); TLS via BearSSL (freestanding port) — `SYS_NET_HTTPS_GET` syscall exists and encrypts the connection, but certificate validation is disabled (accept-all, no trust store) and the shell `curl` command currently only uses plain HTTP |
| **Graphics** | BGA (Bochs Graphics Adapter) driver for arbitrary resolutions; double-buffered framebuffer with dirty-rect partial redraws; runtime resolution switching via `display <w> <h>`; VGA text fallback |
| **Window server** | Multi-window compositor; drag, resize, close; dock/taskbar with app icons; PS/2 mouse + keyboard |
| **VexUI toolkit** | Retained-mode widget library: labels, buttons, panels, progress bars; **VBox/HBox layout containers** with `expand`/`fill`/`gap`/`padding`; **in-window menu bar** with hover, dropdowns and separators |
| **Font rendering** | Built-in bitmap font atlas (`font_atlas.c`) used by VexUI and the kernel terminal; DejaVu Sans TTF embedded via `.incbin` + stb_truetype glyph cache (`appfont.c`) used by the browser for anti-aliased proportional text |
| **Userspace libc** | Freestanding libc: stdio/stdlib/string/ctype; `crt0` (entry, `.init_array` ctors, `_exit`); user heap (`umalloc`) |
| **C++20 userspace** | Full freestanding C++20 runtime: vtables, RTTI, `typeid`, global/local statics, `new`/`delete`, `__cxa_guard_*`, `__cxa_atexit`; standard headers `<array>`, `<span>`, `<algorithm>`, `<utility>`, `<type_traits>`, `<new>`, `<typeinfo>`, etc. |
| **Kernel C++ runtime** | Kernel-side C++20 subset (no exceptions/RTTI); `new`/`delete` via `kmalloc`; `.init_array` global ctors; automatic boot self-test |
| **Logging** | Kernel event journal (`dmesg`); serial debug output; `SYS_LOG` / `vos_log` for userspace; crash persistence to `/journal.log` |
| **Apps** | `sh` (interactive shell), `edit` (text editor), `browser` (HTTP/HTTPS reader), `taskmgr` (process manager, written in C++20), `uidemo`, `hello`, `cpptest` |

---

## Prerequisites

### macOS

```sh
brew install llvm qemu xorriso
```

Make sure the LLVM tools are on your `PATH` or that `clang`/`clang++`/`ld.lld` from the brew prefix are reachable. The Makefile auto-detects common Homebrew paths.

You also need `grub-mkrescue`. The easiest way on macOS is via a cross-compilation tap:

```sh
brew install --cask mxe   # or use i686-elf-grub from a cross-toolchain
# alternative: build grub from source with --target=x86_64-elf
```

> **Tip:** If `grub-mkrescue` is not in `PATH`, set `GRUB_MKRESCUE=/path/to/grub-mkrescue` when invoking `make`.

### Linux (Debian / Ubuntu)

```sh
sudo apt install clang lld llvm qemu-system-x86 grub-pc-bin xorriso python3
```

### Linux (Arch)

```sh
sudo pacman -S clang lld llvm qemu grub xorriso python
```

### All platforms

- Python 3 is required for `scripts/ext2_put.py` (installs apps onto the disk image).
- `make` (GNU Make).

---

## Building

```sh
# 1. Build the kernel ELF and all userspace apps, create the bootable ISO
make all

# 2. Create the persistent disk image (only needed once)
make disk        # creates vibeos-disk.img (32 MB ext2, survives reboots)

# 3. Install userspace apps onto the disk image
make apps

# 4. Boot in QEMU
make run
```

**All-in-one first build:**

```sh
make all && make disk && make apps && make run
```

On subsequent builds, `make run` is enough — it rebuilds whatever changed.

### Individual targets

| Command | Description |
|---|---|
| `make kernel` | Compile kernel + userspace blobs → `build/vibeos.elf` |
| `make iso` | Wrap ELF in a GRUB ISO → `build/vibeos.iso` |
| `make disk` | Create blank `vibeos-disk.img` (ext2, formatted on first boot) |
| `make apps` | Build all userspace binaries and `ext2_put` them onto the disk |
| `make run` | Boot ISO in QEMU with HVF/KVM acceleration and e1000 networking |
| `make run-serial` | Same, but pipe serial port to stdout (for kernel log) |
| `make run-debug` | Same as `run` but with `-no-shutdown` (QEMU stays open on triple fault) |
| `make clean` | Remove `build/` (disk image and ISO are kept) |

---

## Running

QEMU launches with:
- **256 MB RAM**, `vga std` framebuffer
- **HVF** acceleration on macOS / **KVM** on Linux (falls back to TCG)
- **Intel e1000** NIC with SLIRP user networking (guest IP `10.0.2.15`, DNS at `10.0.2.3`)
- Persistent **IDE disk** backed by `vibeos-disk.img`

The desktop starts automatically. Click the **TERM** icon on the taskbar to open a terminal.

### Shell commands

**File system**
```
ls [path]              list directory
cd <path>              change directory
pwd                    print working directory
cat <file>             print file contents
stat <path>            show file metadata (size, type, inode)
touch <file>           create empty file
mkdir <dir>            create directory
rm <path>              remove file
cp <src> <dst>         copy file
mv <src> <dst>         move / rename
echo <text> [> file]   print text or redirect to file
```

**Network**
```
ping <host/ip>         ICMP ping (e.g. ping 8.8.8.8)
ifconfig / ip          show IP address, MAC, link status
curl <url>             HTTP GET (plain-text only; strips http:// prefix)
```

**System**
```
display <w> <h>        switch screen resolution at runtime (e.g. display 1024 768)
dmesg / journal        print kernel event journal
about                  OS version info
clear                  clear terminal
exit                   exit shell
```

**GUI apps** (launch from shell or desktop icons)
```
gui / desktop / wm     start graphical desktop
taskmgr / tasks        process manager
browser / web          HTTP/HTTPS text browser
edit <file>            text editor
uidemo                 VexUI widget demo
hello                  minimal hello-world app
cpptest / c++test      C++20 runtime smoke test
```

---

## Project Layout

```
VibeOS/
├── kernel/
│   ├── include/          — kernel headers
│   └── src/              — kernel C/C++ and assembly source
│       ├── boot.S        — Multiboot2 entry, long-mode switch
│       ├── kernel.c      — main kernel init
│       ├── process.c     — scheduler + process management
│       ├── paging.c      — virtual memory / page tables
│       ├── net.c         — networking stack (ARP/IP/ICMP/UDP/TCP)
│       ├── ext2_fs.c     — ext2 file system
│       ├── window.c      — window server
│       ├── cxx_runtime.cpp — kernel-side C++ ABI
│       └── ...
├── user/
│   ├── libc/             — freestanding libc + C++20 runtime + crt0
│   │   └── include/      — standard headers (stdio, stdlib, string, new, …)
│   ├── taskmgr/          — Task Manager (C++20, VexUI)
│   ├── vexui.c/h         — retained-mode GUI toolkit
│   ├── sh.c              — interactive shell
│   ├── browser.c         — HTTP text browser
│   ├── edit.c            — text editor
│   └── ...
├── third_party/
│   ├── bearssl/          — TLS library (freestanding port)
│   └── stb/              — stb_truetype / stb_image
├── boot/grub/grub.cfg    — GRUB menu config
├── linker.ld             — kernel linker script
├── scripts/              — build helper scripts (ext2_put.py, …)
└── Makefile
```

---

## Architecture overview

```
┌─────────────────────────────────────────────────────────┐
│  Userspace (ring 3)                                     │
│  sh  edit  browser  taskmgr  uidemo  hello  cpptest     │
│  ↑                                                      │
│  user/libc  (stdio/stdlib/string, crt0, C++20 ABI)      │
│  user/vexui (retained-mode GUI toolkit, VBox/HBox)      │
├─────────────────────────────────────────────────────────┤
│  Kernel (ring 0)                                        │
│  scheduler · paging · heap · VFS · ext2                 │
│  e1000 · ARP/IP/ICMP/UDP/DNS/TCP · BearSSL TLS          │
│  framebuffer renderer · window server · PS/2 input      │
│  IDT · PIC · IRQ · syscall (int 0x80)                   │
├─────────────────────────────────────────────────────────┤
│  GRUB Multiboot2 → 32-bit → 64-bit long mode           │
└─────────────────────────────────────────────────────────┘
        QEMU  ·  x86_64 bare metal
```

---

## Writing a userspace app

Create `user/myapp/myapp.cpp`:

```cpp
#include <cstdio>
#include "../vexui.h"

int main() {
    vui_window *win = vui_window_open("My App", 400, 300);

    auto *mb  = vui_menubar(win);
    auto *app = vui_menu(win, mb, "App");
    vui_on_click(vui_menuitem(win, app, "Quit"),
                 [](vui_widget *) { /* vui_quit */ });

    vui_label(win, 20, 40, "Hello from C++20!");
    vui_run(win);
}
```

Add to `Makefile` (apps section):

```makefile
$(CXX) $(UCXXFLAGS) $(LIBC_INC) -Iuser -c user/myapp/myapp.cpp -o build/user/myapp.o
$(LD) -nostdlib -static -T user/linker.ld -o build/user/myapp.elf \
    $(LIBC_CRT0) build/user/myapp.o build/user/vexui.o $(LIBC_A)
$(USTRIP) --strip-all build/user/myapp.elf
python3 scripts/ext2_put.py $(DISK_IMG) build/user/myapp.elf /bin/myapp
```

Then `make apps && make run` and type `myapp` in the shell.

---

## License

This project is experimental / educational. No license is attached; all rights reserved by the authors.
