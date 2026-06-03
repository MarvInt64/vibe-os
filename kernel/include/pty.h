#ifndef VIBEOS_PTY_H
#define VIBEOS_PTY_H

#include "types.h"
#include "fd.h"

/*
 * Pseudo-terminal: a raw bidirectional byte pipe between a "master" (held by a
 * terminal emulator) and a "slave" (bound to a child process's stdin/stdout).
 *
 *   master write ──▶ input ring  ──▶ slave read   (keystrokes to the shell)
 *   slave  write ──▶ output ring ──▶ master read   (shell output to the screen)
 *
 * Reads on an empty ring return 0 (would-block); callers poll. This keeps the
 * kernel side a pure transport and leaves line editing / local echo to the
 * terminal emulator. There is no line discipline here.
 */
struct pty;

/* Reserve a pty from the fixed pool. Returns 0 when none are free. */
struct pty *pty_alloc(void);

/* Release a pty back to the pool (both endpoints are done with it). */
void pty_release(struct pty *pty);

/* fd_ops for the two endpoints; the fd object is the struct pty * itself. */
extern const struct fd_ops PTY_MASTER_FD_OPS;
extern const struct fd_ops PTY_SLAVE_FD_OPS;

#endif
