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
| **Interrupts** | IDT, PIC remapping, IRQ handlers, `int 0x80` syscall interface (39 syscalls) |
| **Scheduler** | Round-robin preemptive multi-process; FPU/SSE context switch per process (`fxsave`/`fxrstor`) so userspace can freely use floats and SSE |
| **Process management** | `SYS_PROCESS_SPAWN` / `SYS_WAITPID` / `SYS_PROCESS_KILL`; per-process file-descriptor table; `SYS_GETARG` argument passing |
| **Storage** | IDE/PIO driver, ramdisk (in-memory block device for early boot), persistent raw disk image |
| **File system** | ext2 read/write; full VFS layer; `open`/`close`/`read`/`write`/`stat`/`readdir`/`chdir`/`getcwd`/`unlink`/`creat`/`mkdir` syscalls; survives reboots |
| **Networking** | Intel e1000 NIC driver; ARP, IPv4, ICMP (`ping`), UDP, DNS, TCP; HTTP (`curl`); TLS via BearSSL (freestanding port) — `SYS_NET_HTTPS_GET` syscall exists and encrypts the connection, but certificate validation is disabled (accept-all, no trust store) and the shell `curl` command currently only uses plain HTTP |
| **Graphics** | BGA (Bochs Graphics Adapter) driver for arbitrary resolutions; double-buffered framebuffer with dirty-rect partial redraws; runtime resolution switching via `display <w> <h>`; VGA text fallback |
| **Window server** | Multi-window compositor; drag, resize, close; PS/2 mouse + keyboard; **per-window alpha** (glass windows) and **partial damaged-rect presents** (`SYS_WINDOW_PRESENT_RECT`) so periodic updaters don't force full-window recomposites |
| **Desktop chrome** | Flat blue-gray glass theme; thin line window controls; global top bar with V-logo, app menu bar, status indicators + sparkline + power glyph; floating rounded **dock** (translucent pill, line-art icons) |
| **Top-bar menu bar** | The focused app declares its menus via `SYS_WINDOW_SET_MENUBAR` / `vos_window_set_menubar` (titles, items, shortcuts, dividers, checkmarks, danger style); the compositor draws the dropdowns and reports picks back as `VOS_EV_MENU_ACTION` |
| **Theming** | Central design-token theme for both kernel chrome and VexUI (`bg`/`surface`/`border`/`text`/`accent`/`ok`/`warn`/`danger`/`menu_*`/`window_alpha`), overridable at runtime from `/home/user/.config/vibeos.theme` |
| **Wallpaper** | Userspace decodes an image (shared `user/libimage`, stb_image) and hands pixels to the kernel via `SYS_SET_WALLPAPER`; `/bin/wallpaper [path]` (default `/wallpapers/default.png`); plain theme-blue backdrop when none set |
| **VexUI toolkit** | Retained-mode widget library: labels, rounded buttons, **pill buttons**, panels, **cards**, **status badges/pills**, **rounded inputs**, **tabs** (accent underline), progress bars, **sparklines**, **dock tiles**; **VBox/HBox layout containers** with `expand`/`fill`/`gap`/`padding`; in-window menu bar; **dirty-on-change setters + damaged-rect partial presents** for efficient redraws |
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

A VibeOS app is an ordinary freestanding ELF that starts at `int main()` (the
`crt0` from `user/libc` runs global constructors and calls `_exit` for you).
There are two flavours:

- **CLI app** — reads/writes via the libc (`printf`, `read`, files, sockets).
  Runs in a terminal/shell, no window.
- **GUI app** — opens a window through the **VexUI** toolkit (`user/vexui.h`)
  *or* presents its own pixel buffer directly (like the browser).

All the syscalls are wrapped by the libc in [`user/libc/include/vibeos.h`](user/libc/include/vibeos.h)
(`vos_*`) and [`user/libc/include/sys/syscall.h`](user/libc/include/sys/syscall.h)
(raw `__scN`). GUI helpers live in [`user/vexui.h`](user/vexui.h).

### 1. Minimal CLI app

`user/hello/hello.c`:

```c
#include <stdio.h>
#include <vibeos.h>

int main(void) {
    printf("hello from VibeOS\n");
    char arg[128];
    if (vos_getarg(arg, sizeof arg) > 0)   /* `hello world` -> arg = "world" */
        printf("you said: %s\n", arg);
    return 0;
}
```

### 2. A GUI app with VexUI

VexUI is **retained-mode**: you build a widget tree once, then `vui_run()`
takes over — it polls input, repaints only when something actually changes
(dirty-on-change), and presents just the damaged region. Widgets are positioned
absolutely or arranged by `VBox`/`HBox` containers.

`user/myapp/myapp.cpp`:

```cpp
#include "vexui.h"
#include <vibeos.h>

static vui_widget *g_count;
static int         g_clicks;

static void on_inc(vui_widget *) {
    char buf[32];
    __builtin_snprintf(buf, sizeof buf, "Clicks: %d", ++g_clicks);
    vui_set_text(g_count, buf);          // dirty-on-change: repaints only if changed
}

int main() {
    vui_window *win = vui_window_open("My App", 420, 300);

    // --- top-bar menu bar (shown while this window is focused) ---
    static const vos_menubar_item menu[] = {
        {"App", "", VOS_MB_TITLE, 0},
        {"Increment", "Ctrl+I", 0, 1},
        {"", "", VOS_MB_DIVIDER, 0},
        {"Quit", "Ctrl+Q", VOS_MB_DANGER, 2},
    };
    vui_window_set_menubar(vui_window_id(win), menu,
                           sizeof menu / sizeof menu[0]);

    // --- widgets ---
    vui_card(win, 16, 16, 388, 70, "Counter");
    g_count = vui_label(win, 28, 48, "Clicks: 0");

    vui_widget *btn = vui_button(win, 16, 100, "Increment");
    vui_on_click(btn, on_inc);

    vui_pill_button(win, 150, 100, "Docs");      // rounded tag-style button
    vui_badge(win, 16, 140, "READY");            // status pill (colour via vui_set_color)
    vui_input(win, 16, 170, 388, "Search...");   // rounded text/search field
    vui_bar(win, 16, 210, 388, 8, 100);          // progress bar

    vui_run(win);                                 // never returns
}
```

Key VexUI calls (see `user/vexui.h` for the full list):

| Call | Widget |
|---|---|
| `vui_label / vui_button / vui_pill_button` | text, rounded button, pill button |
| `vui_card / vui_panel` | titled surfaces |
| `vui_badge` | status pill (`vui_set_color` → ok/warn/danger variant) |
| `vui_input` | rounded text / search field |
| `vui_tabs(win,x,y,w,"A\|B\|C",active)` | tab strip with accent underline |
| `vui_bar / vui_sparkline` | progress bar / mini graph |
| `vui_vbox / vui_hbox` + `vui_box_add` | layout containers (`vui_set_gap/padding/expand/fill`) |
| `vui_set_text/int/value/color/visible` | update a widget (repaints on change only) |
| `vui_on_click(w, cb)` | click handler |

### 3. App-defined top-bar menus

The global top bar shows the **focused** window's menu bar. Declare it once with
`vui_window_set_menubar(id, items, count)`. The list is flat: an item flagged
`VOS_MB_TITLE` opens a new top-level menu, the items after it are its entries.
Flags: `VOS_MB_DIVIDER`, `VOS_MB_DANGER`, `VOS_MB_CHECK` / `VOS_MB_CHECKED`,
`VOS_MB_ARROW`. When the user picks an entry the app receives a
`VOS_EV_MENU_ACTION` event whose `key` is the item's `action_id` — handle it in
a custom event loop, or (with VexUI's in-window menu) via `vui_on_click`.

### 4. Theming

Colours come from a central theme (design tokens), not hardcoded values, so apps
inherit the system look automatically. A theme file at
`/home/user/.config/vibeos.theme` overrides both the kernel chrome and VexUI at
runtime:

```
accent=4DA3FF
surface=1B3048
menu_bg=0C1B2A
window_alpha=234      # 0-255; <255 makes app windows glassy
```

### 5. Drawing your own pixels (custom canvas)

Apps that need full pixel control (e.g. the browser) skip VexUI and present a
raw `0x00RRGGBB` buffer:

```c
int id = vos_window_create("Canvas", 640, 480);
static uint32_t fb[640 * 480];
/* ... draw into fb ... */
vos_window_present(id, fb, 640, 480);              // full present
vos_window_present_rect(id, fb, 640, 480,          // present only a damaged rect
                        x, y, w, h);               // (cheap incremental updates)

vos_event ev;
while (vos_event_poll(id, &ev) == 1) {             // VOS_EV_KEY / MOUSE_* / RESIZE / CLOSE
    /* ... */
}
```

### 6. Wiring it into the build

Add to the `apps:` target in the [`Makefile`](Makefile):

```makefile
$(CXX) $(UCXXFLAGS) $(LIBC_INC) -Iuser -c user/myapp/myapp.cpp -o build/user/myapp.o
$(LD) -nostdlib -static -T user/linker.ld -o build/user/myapp.elf \
    $(LIBC_CRT0) build/user/myapp.o build/user/vexui.o $(LIBC_A)
$(USTRIP) --strip-all build/user/myapp.elf
python3 scripts/ext2_put.py $(DISK_IMG) build/user/myapp.elf /bin/myapp
```

(A CLI app drops `build/user/vexui.o`. To bundle assets, `ext2_put.py` also
creates any missing parent directories, e.g. `/wallpapers/default.png`.)

Then:

```bash
make apps          # compile + install ALL apps onto the disk image
make run           # boot; type `gui` for the desktop, then `myapp` (or launch from the dock)
```

`make apps` installs to the persistent disk image, so a plain `make run` picks up
the new app without rebuilding the ISO.

### Compiling a single app

`make apps` rebuilds every app. To iterate on just one, run that app's three
steps directly (compile → link → install). The toolchain variables come from the
Makefile: `$(CXX)`/`$(CC)`, `$(UCXXFLAGS)`/`$(UCFLAGS)`, `$(LIBC_INC)`,
`$(LIBC_CRT0)`, `$(LIBC_A)`, `$(USTRIP)`, `$(DISK_IMG)`.

```bash
# C++ GUI app (links VexUI + libc):
clang++ -target x86_64-none-elf -ffreestanding -fno-stack-protector -fno-pie \
    -mno-red-zone -mcmodel=small -fno-builtin -Wall -Wextra -std=c++20 -O2 \
    -fno-exceptions -Iuser/libc/include -Iuser \
    -c user/myapp/myapp.cpp -o build/user/myapp.o
ld.lld -nostdlib -static -T user/linker.ld -o build/user/myapp.elf \
    build/user/libc/crt0.o build/user/myapp.o build/user/vexui.o build/user/libc.a
llvm-strip --strip-all build/user/myapp.elf
python3 scripts/ext2_put.py vibeos-disk.img build/user/myapp.elf /bin/myapp
```

This reuses the already-built `build/user/vexui.o` and `build/user/libc.a`; if
you changed the toolkit or libc, run `make libc` (and `make apps` once) first.
For a CLI C app drop `build/user/vexui.o` and use `clang`/`-std=c11`. After the
`ext2_put.py` step the app is on the disk image — just `make run` (no full
`make apps`, no ISO rebuild).

> The built-in shell/editor (`sh`, `edit`) are different: they are embedded into
> the kernel image as blobs, so changing them needs `make kernel` (which runs the
> `user` target), not `make apps`.

---

## License

This project is experimental / educational. No license is attached; all rights reserved by the authors.
