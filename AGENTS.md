# VibeOS Architecture

## Overview

VibeOS is a minimal operating system with a proper separation between kernel and userspace.

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                         USER SPACE                          │
├─────────────┬─────────────┬─────────────┬─────────────────┤
│ /bin/init   │  /bin/sh    │ /bin/editor │   /bin/ls       │
│  (PID 1)    │   (shell)   │   (editor)  │   (list files)  │
└──────┬──────┴──────┬──────┴──────┬──────┴────────┬──────────┘
       │             │             │               │
       └─────────────┴──────┬──────┴───────────────┘
                            │
                    ┌───────▼───────┐
                    │    libvex     │  (Userspace library)
                    │  - syscalls   │
                    │  - I/O        │
                    │  - string     │
                    └───────┬───────┘
                            │
       ┌────────────────────┼────────────────────┐
       │              KERNEL SPACE              │
       │                                        │
       │  ┌─────────────┐  ┌─────────────────┐  │
       │  │     VFS     │  │ Process Manager │  │
       │  │ (filesystem)│  │  - spawn/exec   │  │
       │  └─────────────┘  │  - fork/wait    │  │
       │                   └─────────────────┘  │
       │  ┌─────────────────────────────────┐  │
       │  │         Syscall Handler         │  │
       │  │  - exec(path)                   │  │
       │  │  - fork()                       │  │
       │  │  - read/write/open              │  │
       │  └─────────────────────────────────┘  │
       │  ┌─────────────────────────────────┐  │
       │  │         ELF Loader              │  │
       │  │  - Load executables from VFS    │  │
       │  └─────────────────────────────────┘  │
       └────────────────────────────────────────┘
```

## Key Principles

### 1. No Hardcoded Programs
- The kernel does NOT contain if-else chains for programs
- All programs are separate ELF binaries in `/bin/`
- New programs can be added without kernel changes

### 2. System Calls

| Syscall | Number | Description |
|---------|--------|-------------|
| exit    | 0      | Terminate process |
| exec    | 1      | Execute program |
| open    | 2      | Open file |
| read    | 3      | Read from FD |
| write   | 4      | Write to FD |
| close   | 5      | Close FD |
| getpid  | 6      | Get process ID |
| fork    | 7      | Create new process |
| wait    | 8      | Wait for child |

### 3. File System Layout

```
/bin/
  init      - First process (PID 1)
  sh        - Shell
  editor    - Text editor
  ls        - List files

/home/
  (user files)
```

## Building

```bash
# Build all components
cargo build --release

# Run (requires proper target setup for bare metal)
cargo run --bin init
```

## Adding New Programs

1. Create new crate in `bin/<name>/`
2. Add to workspace `Cargo.toml`
3. Link against `libvex`
4. Binary will be available at `/bin/<name>`

Example:
```rust
#![no_std]
#![no_main]

use libvex::{println, exit};

#[no_mangle]
pub extern "C" fn _start() -> ! {
    println!("Hello from my program!");
    exit(0);
}
```

## Design Philosophy

- **Separation of concerns**: Kernel handles resources, userspace handles functionality
- **Extensibility**: New programs require no kernel changes
- **Simplicity**: Minimal implementation showing core concepts
- **Unix-like**: Familiar patterns for developers
