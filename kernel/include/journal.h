#ifndef VIBEOS_JOURNAL_H
#define VIBEOS_JOURNAL_H

#include "types.h"

/* Kernel event journal: a ring buffer of recent system events (process
 * lifecycle, faults, networking, app logs). Mirrored to the serial port and
 * readable from userspace via SYS_JOURNAL_READ (e.g. the `dmesg` command). */

enum journal_level {
    JOURNAL_INFO = 0,
    JOURNAL_WARN = 1,
    JOURNAL_ERROR = 2,
    JOURNAL_FAULT = 3,
    JOURNAL_APP = 4    /* userspace log line */
};

#define JOURNAL_MSG_MAX 96
#define JOURNAL_CAPACITY 256

struct journal_entry {
    uint64_t seq;      /* monotonically increasing record number */
    uint64_t tick;     /* timer tick when logged */
    uint32_t level;
    uint32_t pid;      /* originating pid, or 0 for kernel */
    char msg[JOURNAL_MSG_MAX];
};

void journal_init(void);

/* Append a message. `msg` is copied (truncated to JOURNAL_MSG_MAX-1). */
void journal_log(enum journal_level level, uint32_t pid, const char *msg);

/* Convenience: message followed by a hex number (e.g. "fault rip=", value). */
void journal_log_hex(enum journal_level level, uint32_t pid, const char *msg, uint64_t value);

/* Total records ever logged (seq of the next entry). */
uint64_t journal_total(void);

/* Copy the record with the given absolute seq into *out. Returns 1 if the
 * record is still in the ring, 0 if it has scrolled out or doesn't exist. */
int journal_get(uint64_t seq, struct journal_entry *out);

/* ---- persistence ---- *
 * Important records (WARN/ERROR/FAULT) set a "dirty" flag; the kernel main loop
 * checks it and flushes from a SAFE context (never from the fault handler).
 * journal_format_all renders the whole ring to a human-readable text buffer. */
int  journal_persist_pending(void);   /* 1 if a flush is wanted */
void journal_persist_clear(void);
size_t journal_format_all(char *buf, size_t cap);   /* returns bytes written */

#endif
