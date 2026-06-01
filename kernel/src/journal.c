/* Kernel event journal — ring buffer + serial mirror. See journal.h. */
#include "journal.h"
#include "serial.h"
#include "timer.h"

static struct journal_entry g_ring[JOURNAL_CAPACITY];
static uint64_t g_next_seq;     /* seq of the next record to write */
static int g_persist_pending;   /* a WARN/ERROR/FAULT needs flushing to disk */

void journal_init(void) {
    g_next_seq = 0;
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
    e->level = (uint32_t)level;
    e->pid = pid;
    copy_msg(e->msg, msg);
    g_next_seq++;

    /* Mark the journal for persistence on important events. */
    if (level == JOURNAL_WARN || level == JOURNAL_ERROR || level == JOURNAL_FAULT) {
        g_persist_pending = 1;
    }

    /* Mirror to serial for host-side debugging. */
    serial_write("[");
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

size_t journal_format_all(char *buf, size_t cap) {
    static const char *tags[] = {"INFO ", "WARN ", "ERROR", "FAULT", "APP  "};
    uint64_t start = (g_next_seq > JOURNAL_CAPACITY) ? g_next_seq - JOURNAL_CAPACITY : 0;
    uint64_t s;
    size_t pos = 0;

    for (s = start; s < g_next_seq && pos + 1 < cap; ++s) {
        struct journal_entry *e = &g_ring[s % JOURNAL_CAPACITY];
        pos = put_str(buf, pos, cap, "[");
        pos = put_dec(buf, pos, cap, e->tick);
        pos = put_str(buf, pos, cap, "] ");
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
