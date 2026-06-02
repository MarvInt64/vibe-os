#include "vexui.h"
#include <vibeos.h>

static vui_widget *g_version;
static vui_widget *g_build;
static vui_widget *g_uptime;
static vui_widget *g_memory;
static vui_widget *g_processes;
static vui_widget *g_windows;

static char *append_text(char *out, char *end, const char *text) {
    while (text && *text && out + 1 < end) {
        *out++ = *text++;
    }
    *out = 0;
    return out;
}

static char *append_uint(char *out, char *end, unsigned long long value) {
    char digits[24];
    int n = 0;
    if (value == 0) {
        return append_text(out, end, "0");
    }
    while (value > 0 && n < (int)sizeof(digits)) {
        digits[n++] = (char)('0' + (value % 10));
        value /= 10;
    }
    while (n > 0 && out + 1 < end) {
        *out++ = digits[--n];
    }
    *out = 0;
    return out;
}

static char *append_mb(char *out, char *end, unsigned long long bytes) {
    out = append_uint(out, end, bytes / (1024ull * 1024ull));
    return append_text(out, end, " MB");
}

static void set_pair(vui_widget *label, const char *prefix, const char *value) {
    char text[96];
    char *p = text;
    char *end = text + sizeof(text);
    p = append_text(p, end, prefix);
    p = append_text(p, end, value);
    vui_set_text(label, text);
}

static void refresh(vui_window *) {
    struct vos_system_info info{};
    char value[96];
    char *p;
    char *end = value + sizeof(value);
    unsigned long long seconds;
    unsigned long long hours;
    unsigned long long minutes;

    if (vos_system_info(&info) < 0) {
        vui_set_text(g_version, "Kernel info unavailable");
        return;
    }

    set_pair(g_version, "VibeOS ", info.version);
    set_pair(g_build, "Build ", info.build);

    seconds = info.timer_hz ? info.uptime_ticks / info.timer_hz : 0;
    hours = seconds / 3600;
    minutes = (seconds / 60) % 60;
    seconds %= 60;
    p = value;
    p = append_uint(p, end, hours);
    p = append_text(p, end, "h ");
    p = append_uint(p, end, minutes);
    p = append_text(p, end, "m ");
    p = append_uint(p, end, seconds);
    p = append_text(p, end, "s");
    set_pair(g_uptime, "Uptime ", value);

    p = value;
    p = append_mb(p, end, info.heap_used_bytes);
    p = append_text(p, end, " / ");
    p = append_mb(p, end, info.heap_total_bytes);
    set_pair(g_memory, "RAM ", value);

    p = value;
    p = append_uint(p, end, info.process_count);
    p = append_text(p, end, " / ");
    p = append_uint(p, end, info.process_max);
    set_pair(g_processes, "Processes ", value);

    p = value;
    p = append_uint(p, end, info.app_window_max);
    set_pair(g_windows, "App windows ", value);
}

int main() {
    vui_window *win = vui_window_open("System Info", 420, 300);

    vui_set_anchor(vui_card(win, 16, 16, 388, 72, "SYSTEM"), VUI_ANCHOR_LEFT | VUI_ANCHOR_TOP | VUI_ANCHOR_RIGHT);
    g_version = vui_label(win, 32, 42, "VibeOS");
    g_build = vui_label(win, 32, 62, "Build");

    vui_set_anchor(vui_card(win, 16, 102, 388, 168, "RESOURCES"), VUI_ANCHOR_LEFT | VUI_ANCHOR_TOP | VUI_ANCHOR_RIGHT | VUI_ANCHOR_BOTTOM);
    g_uptime = vui_label(win, 32, 130, "Uptime");
    g_memory = vui_label(win, 32, 156, "RAM");
    g_processes = vui_label(win, 32, 182, "Processes");
    g_windows = vui_label(win, 32, 208, "App windows");

    vui_set_color(g_version, VUI_ACCENT);
    vui_set_color(g_memory, VUI_WARN);
    vui_set_color(g_processes, VUI_OK);

    refresh(win);
    vui_on_tick(win, refresh);
    vui_run(win);
}
