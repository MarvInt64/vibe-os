# Syscall Unification

## Problem

The arm64 syscall handler (`kernel/arch/arm64/arch.c`) duplicates many syscall
implementations that already exist in the shared `kernel/src/process.c`.
x86 calls into `process.c`'s `process_handle_syscall()` (via `process_run_ready_slice`),
but arm64 has its own `switch` statement in `arch.c`.

## Why

`kernel/src/process.c` uses x86-specific types:
- `struct interrupt_frame` (has `rax`, `rdi`, `rsi`, `rflags`, `cs`, `rsp`, `ss`)
- x87 FPU save/restore (`fxsave`/`fxrstor`)
- `TSS` for kernel stack switching
- `lapic_eoi()` / `apic_timer` for preemption

Arm64 needs different mechanisms for all of these.

## Duplicated syscalls

At minimum these are handled in BOTH `arch.c` (arm64) and `process.c` (x86):

- SYS_WRITE (1)
- SYS_READ (0)
- SYS_OPEN (7)
- SYS_CLOSE (8)
- SYS_STAT (9)
- SYS_READDIR (10)
- SYS_CHDIR (11)
- SYS_GETCWD (12)
- SYS_MKDIR (26)
- SYS_SEEK (48)
- SYS_EXIT (4)
- SYS_YIELD (3)
- SYS_PROCESS_SPAWN (5)
- SYS_PROCESS_KILL (29)
- SYS_TIMER_SLEEP (20)
- SYS_PTY_OPEN (45)
- SYS_SPAWN_PTY (46)
- ... and more

## Plan

1. Abstract `interrupt_frame` into an arch-neutral syscall context struct
   with fields like `num`, `arg0`–`arg5`, `ret`.
2. Extract the syscall dispatch chain from `process.c` into a separate
   `kernel/src/syscall.c` that takes the abstracted context.
3. Both x86 and arm64 call into `syscall_dispatch()` with their arch-specific
   frame converted.
4. Keep arm64-specific additions (PTY ring buffers, ELR yield hack) in arch.c
   or make them cross-arch.

## Status

- [ ] Abstract interrupt_frame
- [ ] Extract syscall dispatch
- [ ] Route arm64 SVC through shared dispatch
- [ ] Remove duplicated handlers from arch.c
