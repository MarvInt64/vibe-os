/* taskmgr - VibeOS process manager built as a standalone VexUI app. */
#include "vexui.h"

#define PROCESS_STATE_EMPTY 0
#define PROCESS_STATE_READY 1
#define PROCESS_STATE_RUNNING 2
#define PROCESS_STATE_SLEEPING 3
#define PROCESS_STATE_WAITING 4
#define PROCESS_STATE_EXITED 5
#define PROCESS_STATE_WAITING_IO 6

static vui_widget *g_total_label;
static vui_widget *g_ready_label;
static vui_widget *g_status_label;
static vui_widget *g_rows[VUI_PROCESS_MAX];
static vui_widget *g_states[VUI_PROCESS_MAX];
static vui_widget *g_metrics[VUI_PROCESS_MAX];
static vui_widget *g_kill[VUI_PROCESS_MAX];
static vui_widget *g_refresh_button;
static unsigned int g_row_pid[VUI_PROCESS_MAX];
static int g_tick;

static int append(char *out, int cap, int pos, const char *s) {
    while (s && *s && pos + 1 < cap) {
        out[pos++] = *s++;
    }
    out[pos] = 0;
    return pos;
}

static int append_uint(char *out, int cap, int pos, unsigned long value) {
    char tmp[24];
    int n = 0;
    if (value == 0) {
        return append(out, cap, pos, "0");
    }
    while (value && n < (int)sizeof(tmp)) {
        tmp[n++] = (char)('0' + (value % 10u));
        value /= 10u;
    }
    while (n && pos + 1 < cap) {
        out[pos++] = tmp[--n];
    }
    out[pos] = 0;
    return pos;
}

static int append_int(char *out, int cap, int pos, int value) {
    if (value < 0) {
        pos = append(out, cap, pos, "-");
        value = -value;
    }
    return append_uint(out, cap, pos, (unsigned long)value);
}

static const char *state_name(unsigned char state) {
    switch (state) {
        case PROCESS_STATE_READY: return "READY";
        case PROCESS_STATE_RUNNING: return "RUNNING";
        case PROCESS_STATE_SLEEPING: return "SLEEP";
        case PROCESS_STATE_WAITING: return "WAIT";
        case PROCESS_STATE_WAITING_IO: return "IOWAIT";
        case PROCESS_STATE_EXITED: return "EXIT";
        default: return "EMPTY";
    }
}

static vui_u32 state_color(unsigned char state) {
    switch (state) {
        case PROCESS_STATE_RUNNING: return VUI_ACCENT;
        case PROCESS_STATE_READY: return VUI_OK;
        case PROCESS_STATE_SLEEPING: return VUI_WARN;
        case PROCESS_STATE_WAITING:
        case PROCESS_STATE_WAITING_IO: return 0x006bcdf0u;
        case PROCESS_STATE_EXITED: return VUI_DANGER;
        default: return VUI_TEXT_DIM;
    }
}

static void set_status(const char *text, vui_u32 color) {
    vui_set_text(g_status_label, text);
    vui_set_color(g_status_label, color);
}

static void refresh_processes(void) {
    unsigned int slot;
    unsigned int loaded = 0;
    unsigned int ready = 0;
    unsigned int sleeping = 0;
    char text[96];
    int pos;

    for (slot = 0; slot < VUI_PROCESS_MAX; ++slot) {
        vui_process_info p;
        char row[96];
        char metric[80];

        g_row_pid[slot] = 0;
        if (vui_process_snapshot(slot, &p) <= 0 || !p.loaded || p.state == PROCESS_STATE_EMPTY || p.state == PROCESS_STATE_EXITED) {
            vui_set_visible(g_rows[slot], 0);
            vui_set_visible(g_states[slot], 0);
            vui_set_visible(g_metrics[slot], 0);
            vui_set_visible(g_kill[slot], 0);
            continue;
        }

        ++loaded;
        if (p.state == PROCESS_STATE_READY) ++ready;
        if (p.state == PROCESS_STATE_SLEEPING) ++sleeping;
        g_row_pid[slot] = p.pid;

        pos = append(row, sizeof(row), 0, "PID ");
        pos = append_uint(row, sizeof(row), pos, p.pid);
        pos = append(row, sizeof(row), pos, "  ");
        (void)append(row, sizeof(row), pos, p.name[0] ? p.name : "process");

        pos = append(metric, sizeof(metric), 0, "ticks ");
        pos = append_uint(metric, sizeof(metric), pos, p.runtime_ticks);
        pos = append(metric, sizeof(metric), pos, "   switches ");
        pos = append_uint(metric, sizeof(metric), pos, p.switch_count);
        pos = append(metric, sizeof(metric), pos, "   parent ");
        (void)append_uint(metric, sizeof(metric), pos, p.parent_pid);

        vui_set_text(g_rows[slot], row);
        vui_set_color(g_rows[slot], p.state == PROCESS_STATE_RUNNING ? VUI_TEXT : VUI_TEXT_DIM);
        vui_set_visible(g_rows[slot], 1);

        vui_set_text(g_states[slot], state_name(p.state));
        vui_set_color(g_states[slot], state_color(p.state));
        vui_set_visible(g_states[slot], 1);

        vui_set_text(g_metrics[slot], metric);
        vui_set_color(g_metrics[slot], VUI_TEXT_DIM);
        vui_set_visible(g_metrics[slot], 1);

        vui_set_visible(g_kill[slot], p.state != PROCESS_STATE_RUNNING);
    }

    text[0] = 0;
    pos = append(text, sizeof(text), 0, "PROCESSES ");
    pos = append_uint(text, sizeof(text), pos, loaded);
    pos = append(text, sizeof(text), pos, " / ");
    (void)append_uint(text, sizeof(text), pos, VUI_PROCESS_MAX);
    vui_set_text(g_total_label, text);

    text[0] = 0;
    pos = append(text, sizeof(text), 0, "READY ");
    pos = append_uint(text, sizeof(text), pos, ready);
    pos = append(text, sizeof(text), pos, "   SLEEP ");
    (void)append_uint(text, sizeof(text), pos, sleeping);
    vui_set_text(g_ready_label, text);
}

static void on_kill(vui_widget *self) {
    int row = (int)(unsigned long)vui_get_user(self);
    unsigned int pid;
    int result;

    if (row < 0 || row >= VUI_PROCESS_MAX) {
        return;
    }

    pid = g_row_pid[row];
    if (pid == 0) {
        return;
    }

    result = vui_process_kill(pid);
    if (result == 0) {
        set_status("KILL SENT", VUI_WARN);
    } else {
        char status[32];
        int pos = append(status, sizeof(status), 0, "KILL ");
        (void)append_int(status, sizeof(status), pos, result);
        set_status(status, VUI_DANGER);
    }
    refresh_processes();
}

static void on_refresh(vui_widget *self) {
    (void)self;
    set_status("REFRESHED", VUI_ACCENT);
    refresh_processes();
}

static void on_tick(vui_window *window) {
    (void)window;
    ++g_tick;
    if ((g_tick % 18) == 0) {
        refresh_processes();
    }
}

void __attribute__((noreturn)) _start(void) {
    vui_window *win = vui_window_open("Task Manager", 560, 340);
    unsigned int i;

    vui_label(win, 18, 12, "TASK MANAGER");
    vui_widget *subtitle = vui_label(win, 152, 12, "LIVE PROCESS CONTROL");
    vui_set_color(subtitle, VUI_TEXT_DIM);
    g_refresh_button = vui_button(win, 458, 7, "Refresh");
    vui_set_button_width(g_refresh_button, 76);
    vui_set_anchor(g_refresh_button, VUI_ANCHOR_TOP | VUI_ANCHOR_RIGHT);
    vui_on_click(g_refresh_button, on_refresh);

    vui_set_anchor(vui_panel(win, 16, 40, 528, 58, "System"), VUI_ANCHOR_LEFT | VUI_ANCHOR_TOP | VUI_ANCHOR_RIGHT);
    g_total_label = vui_label(win, 30, 70, "PROCESSES");
    vui_set_color(g_total_label, VUI_ACCENT);
    g_ready_label = vui_label(win, 214, 70, "READY");
    vui_set_color(g_ready_label, VUI_OK);
    g_status_label = vui_label(win, 394, 70, "READY");
    vui_set_anchor(g_status_label, VUI_ANCHOR_TOP | VUI_ANCHOR_RIGHT);
    vui_set_color(g_status_label, VUI_TEXT_DIM);

    vui_set_anchor(vui_panel(win, 16, 112, 528, 210, "Processes"), VUI_ANCHOR_LEFT | VUI_ANCHOR_TOP | VUI_ANCHOR_RIGHT | VUI_ANCHOR_BOTTOM);
    vui_label(win, 30, 142, "PROCESS");
    vui_label(win, 296, 142, "STATE");
    vui_set_anchor(vui_label(win, 486, 142, "ACTION"), VUI_ANCHOR_TOP | VUI_ANCHOR_RIGHT);

    for (i = 0; i < VUI_PROCESS_MAX; ++i) {
        int y = 166 + ((int)i * 38);
        g_rows[i] = vui_label(win, 30, y, "");
        g_states[i] = vui_label(win, 296, y, "");
        g_metrics[i] = vui_label(win, 30, y + 17, "");
        g_kill[i] = vui_button(win, 482, y - 4, "KILL");
        vui_set_button_width(g_kill[i], 48);
        vui_set_anchor(g_kill[i], VUI_ANCHOR_TOP | VUI_ANCHOR_RIGHT);
        vui_set_color(g_kill[i], VUI_DANGER);
        vui_set_user(g_kill[i], (void *)(unsigned long)i);
        vui_on_click(g_kill[i], on_kill);
    }

    refresh_processes();
    vui_on_tick(win, on_tick);
    vui_run(win);
}
