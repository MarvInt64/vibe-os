/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Marvin Kicha <https://github.com/MarvInt64/> */

/* Kernel event journal — ring buffer + serial mirror. See journal.h. */
#include "journal.h"
#include "serial.h"
#include "timer.h"

static struct journal_entry g_ring[JOURNAL_CAPACITY];
static uint64_t g_next_seq;     /* seq of the next record to write */
static uint64_t g_boot_id;
static int g_persist_pending;   /* a WARN/ERROR/FAULT needs flushing to disk */

void journal_init(void) {
    uintptr_t stack_sample = (uintptr_t)&stack_sample;
    g_next_seq = 0;
    g_boot_id = 0x564942454f530000ull ^ stack_sample ^ timer_tick_count();
    g_persist_pending = 0;
}

int journal_persist_pending(void) { return g_persist_pending; }
void journal_persist_clear(void) { g_persist_pending = 0; }

static const char *level_tag(enum journal_level l) {
    switch (l) {
        case JOURNAL_INFO:  return "INFO ";
        case JOURNAL_WARN:  return "WARN ";
        case JOURNAL_ERROR: return "ERROR";
        case JOURNAL_FAULT: return "FAULT";
        case JOURNAL_APP:   return "APP  ";
        default:            return "?????";
    }
}

static void copy_msg(char *dst, const char *src) {
    int i;
    for (i = 0; i + 1 < JOURNAL_MSG_MAX && src && src[i]; ++i) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

void journal_log(enum journal_level level, uint32_t pid, const char *msg) {
    struct journal_entry *e = &g_ring[g_next_seq % JOURNAL_CAPACITY];

    e->seq = g_next_seq;
    e->tick = timer_tick_count();
    e->boot_id = g_boot_id;
    e->hz = timer_frequency_hz();
    e->level = (uint32_t)level;
    e->pid = pid;
    copy_msg(e->msg, msg);
    g_next_seq++;

    /* Mark the journal for persistence on important events. APP-level entries
     * (e.g. the browser's load results) are included so post-mortem inspection
     * of /journal.log reflects app activity, not just faults. The main loop
     * coalesces multiple pending entries into one disk write. */
    if (level == JOURNAL_WARN || level == JOURNAL_ERROR ||
        level == JOURNAL_FAULT || level == JOURNAL_APP) {
        g_persist_pending = 1;
    }

    /* Mirror to serial for host-side debugging. */
    serial_write("[");
    serial_write_hex_u64(g_boot_id);
    serial_write(" #");
    serial_write_hex_u64(e->seq);
    serial_write(" t=");
    serial_write_hex_u64(e->tick);
    serial_write(" ");
    serial_write(level_tag(level));
    serial_write(" pid=");
    serial_write_hex_u64(pid);
    serial_write("] ");
    serial_write(e->msg);
    serial_write("\n");
}

void journal_log_hex(enum journal_level level, uint32_t pid, const char *msg, uint64_t value) {
    char buf[JOURNAL_MSG_MAX];
    int i = 0, j;
    const char *hex = "0123456789abcdef";
    char tmp[17];
    int n = 0;

    for (i = 0; i + 1 < JOURNAL_MSG_MAX && msg && msg[i]; ++i) buf[i] = msg[i];
    /* append "0x...." */
    if (i + 3 < JOURNAL_MSG_MAX) { buf[i++] = '0'; buf[i++] = 'x'; }
    if (value == 0) { tmp[n++] = '0'; }
    else { while (value && n < 16) { tmp[n++] = hex[value & 0xf]; value >>= 4; } }
    for (j = n - 1; j >= 0 && i + 1 < JOURNAL_MSG_MAX; --j) buf[i++] = tmp[j];
    buf[i] = '\0';

    journal_log(level, pid, buf);
}

uint64_t journal_total(void) {
    return g_next_seq;
}

int journal_get(uint64_t seq, struct journal_entry *out) {
    if (out == 0) return 0;
    if (seq >= g_next_seq) return 0;                       /* not written yet */
    if (g_next_seq > JOURNAL_CAPACITY && seq < g_next_seq - JOURNAL_CAPACITY) {
        return 0;                                          /* scrolled out */
    }
    *out = g_ring[seq % JOURNAL_CAPACITY];
    return 1;
}

/* Append a decimal number; returns new position. */
static size_t put_dec(char *b, size_t pos, size_t cap, uint64_t v) {
    char tmp[24]; int n = 0;
    if (v == 0) { if (pos + 1 < cap) b[pos++] = '0'; return pos; }
    while (v && n < 24) { tmp[n++] = (char)('0' + (v % 10u)); v /= 10u; }
    while (n && pos + 1 < cap) b[pos++] = tmp[--n];
    return pos;
}
static size_t put_str(char *b, size_t pos, size_t cap, const char *s) {
    while (s && *s && pos + 1 < cap) b[pos++] = *s++;
    return pos;
}
static size_t put_hex(char *b, size_t pos, size_t cap, uint64_t v) {
    const char *hex = "0123456789abcdef";
    char tmp[16];
    int n = 0;
    if (pos + 2 < cap) {
        b[pos++] = '0';
        b[pos++] = 'x';
    }
    if (v == 0) {
        if (pos + 1 < cap) b[pos++] = '0';
        return pos;
    }
    while (v && n < 16) {
        tmp[n++] = hex[v & 0xfu];
        v >>= 4;
    }
    while (n && pos + 1 < cap) b[pos++] = tmp[--n];
    return pos;
}

size_t journal_format_all(char *buf, size_t cap) {
    static const char *tags[] = {"INFO ", "WARN ", "ERROR", "FAULT", "APP  "};
    uint64_t start = (g_next_seq > JOURNAL_CAPACITY) ? g_next_seq - JOURNAL_CAPACITY : 0;
    uint64_t s;
    size_t pos = 0;

    pos = put_str(buf, pos, cap, "# VibeOS journal boot=");
    pos = put_hex(buf, pos, cap, g_boot_id);
    pos = put_str(buf, pos, cap, " hz=");
    pos = put_dec(buf, pos, cap, timer_frequency_hz());
    pos = put_str(buf, pos, cap, " total=");
    pos = put_dec(buf, pos, cap, g_next_seq);
    pos = put_str(buf, pos, cap, "\n");

    for (s = start; s < g_next_seq && pos + 1 < cap; ++s) {
        struct journal_entry *e = &g_ring[s % JOURNAL_CAPACITY];
        uint32_t hz = e->hz ? e->hz : timer_frequency_hz();
        pos = put_str(buf, pos, cap, "[boot=");
        pos = put_hex(buf, pos, cap, e->boot_id);
        pos = put_str(buf, pos, cap, " seq=");
        pos = put_dec(buf, pos, cap, e->seq);
        pos = put_str(buf, pos, cap, " tick=");
        pos = put_dec(buf, pos, cap, e->tick);
        pos = put_str(buf, pos, cap, " t=");
        pos = put_dec(buf, pos, cap, hz ? e->tick / hz : e->tick);
        pos = put_str(buf, pos, cap, ".");
        {
            uint64_t ms = hz ? ((e->tick % hz) * 1000u) / hz : 0;
            if (ms < 100) pos = put_str(buf, pos, cap, "0");
            if (ms < 10) pos = put_str(buf, pos, cap, "0");
            pos = put_dec(buf, pos, cap, ms);
        }
        pos = put_str(buf, pos, cap, "s] ");
        pos = put_str(buf, pos, cap, e->level < 5 ? tags[e->level] : "?????");
        pos = put_str(buf, pos, cap, " pid=");
        pos = put_dec(buf, pos, cap, e->pid);
        pos = put_str(buf, pos, cap, "  ");
        pos = put_str(buf, pos, cap, e->msg);
        pos = put_str(buf, pos, cap, "\n");
    }
    buf[pos < cap ? pos : cap - 1] = '\0';
    return pos;
}
