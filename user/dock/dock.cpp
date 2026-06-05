#include "vexui.h"
#include <sys/syscall.h>
#include <vibeos.h>
#include <string.h>

#define PROCESS_STATE_EMPTY 0
#define PROCESS_STATE_EXITED 5

struct DockEntry {
    const char *label;
    int icon;            /* line-art icon id: 1=web 2=activity 3=grid 4=code 5=folder */
    uint32_t color;
    const char *path;
    const char *icon_path;
};

static const DockEntry kEntries[] = {
    {"Terminal", 4, 0x0064f2ccu, "/bin/terminal", "/icons/dock/terminal.svg"},
    {"DOOM",    3, 0x00e03030u, "/bin/doom",     0},
    {"Browser", 1, 0x006eb6ffu, "/bin/browser", "/icons/dock/browser.svg"},
    {"Files", 5, 0x007ec8ffu, "/bin/filebrowser", "/icons/dock/filebrowser.svg"},
    {"Tasks", 2, 0x0076e0b5u, "/bin/taskmgr", "/icons/dock/taskmgr.svg"},
    {"Player", 3, 0x004aa8ffu, "/bin/audioplayer", "/icons/dock/player.svg"},
    {"C++", 4, 0x008f7bf0u, "/bin/cpptest", "/icons/dock/terminal.svg"},
    {"Info", 2, 0x00a8c7ffu, "/bin/sysinfo", 0},
};

static vui_widget *sButtons[8];

static void dock_on_tick(vui_window *win) {
    (void)win;
    for (unsigned i = 0; i < sizeof(kEntries) / sizeof(kEntries[0]); ++i) {
        bool running = false;
        const char *path = kEntries[i].path;
        const char *name = path;
        for (const char *p = path; *p; ++p) {
            if (*p == '/') {
                name = p + 1;
            }
        }
        for (unsigned int slot = 0; slot < VUI_PROCESS_MAX; ++slot) {
            vui_process_info p;
            if (vui_process_snapshot(slot, &p) > 0 && p.loaded && p.state != PROCESS_STATE_EMPTY && p.state != PROCESS_STATE_EXITED) {
                if (strcmp(p.name, name) == 0 || strcmp(p.name, path) == 0) {
                    running = true;
                    break;
                }
            }
        }
        vui_set_running(sButtons[i], running ? 1 : 0);
    }
}

static void append_str(char *buf, int *pos, int cap, const char *s) {
    while (s && *s && *pos + 1 < cap) {
        buf[(*pos)++] = *s++;
    }
    buf[*pos] = '\0';
}

static void append_int(char *buf, int *pos, int cap, int value) {
    char tmp[16];
    unsigned int n;
    int len = 0;
    if (value < 0) {
        append_str(buf, pos, cap, "-");
        n = (unsigned int)(-value);
    } else {
        n = (unsigned int)value;
    }
    if (n == 0) {
        append_str(buf, pos, cap, "0");
        return;
    }
    while (n && len < (int)sizeof(tmp)) {
        tmp[len++] = (char)('0' + (n % 10u));
        n /= 10u;
    }
    while (len > 0 && *pos + 1 < cap) {
        buf[(*pos)++] = tmp[--len];
    }
    buf[*pos] = '\0';
}

static void launch_app(vui_widget *self) {
    const char *path = (const char *)vui_get_user(self);
    if (path) {
        char msg[96];
        int pos = 0;
        int pid;
        append_str(msg, &pos, sizeof(msg), "dock launch ");
        append_str(msg, &pos, sizeof(msg), path);
        vos_log(VOS_LOG_APP, msg);

        pid = vos_spawn(path);
        pos = 0;
        append_str(msg, &pos, sizeof(msg), "dock launch result ");
        append_str(msg, &pos, sizeof(msg), path);
        append_str(msg, &pos, sizeof(msg), " pid=");
        append_int(msg, &pos, sizeof(msg), pid);
        vos_log(VOS_LOG_APP, msg);
    }
}

int main() {
    uint32_t mode = vos_display_mode_get();
    int screen_w = (int)((mode >> 16) & 0xffffu);
    int screen_h = (int)(mode & 0xffffu);
    int width      = 560;
    int dock_h     = 76;   /* height of the visible pill */
    int tooltip_h  = 32;   /* transparent headroom above the pill for tooltips */
    int height     = dock_h + tooltip_h;
    int x;
    int y;

    if (screen_w <= 0) screen_w = 1024;
    if (screen_h <= 0) screen_h = 768;
    if (width > screen_w - 24) width = screen_w - 24;
    x = (screen_w - width) / 2;
    y = screen_h - height - 24;
    if (y < 0) y = 0;

    vos_log(VOS_LOG_APP, "dock ready");

    /* shadow_inset_top = tooltip_h: the upper transparent zone (tooltip
     * headroom) casts no shadow, but the visible pill below still does. */
    vui_window *win = vui_window_open_inset(
        "Dock", width, height,
        VUI_WINDOW_FRAMELESS | VUI_WINDOW_NO_DOCK |
            VUI_WINDOW_POSITIONED | VUI_WINDOW_ALWAYS_ON_TOP |
            VUI_WINDOW_TRANSLUCENT,
        x, y, tooltip_h);
    vui_set_clear_color(win, VUI_COLOR_TRANSPARENT);

    /* The pill sits in the lower dock_h pixels; the upper tooltip_h pixels are
     * fully transparent so the tooltip bubble has room to render above the icons. */
    vui_widget *surface = vui_pill(win, 0, tooltip_h, width, dock_h);
    /* Glassmorphism dock: deep navy-blue like macOS, with a layered
     * gradient (lighter top, darker bottom), soft border, and top highlight
     * — matching the reference dock.png aesthetic. */
    vui_set_color(surface, 0x00142c4au);

    vui_widget *row = vui_hbox(win, 32, tooltip_h + 8, width - 64, dock_h - 16);
    vui_set_gap(row, 18);
    vui_set_padding(row, 0);

    for (unsigned i = 0; i < sizeof(kEntries) / sizeof(kEntries[0]); ++i) {
        vui_widget *button = vui_tile_button(win, 0, 0, "");
        sButtons[i] = button;
        vui_set_color(button, kEntries[i].color);
        vui_set_value(button, kEntries[i].icon);
        if (kEntries[i].icon_path) {
            vui_set_icon_svg_path(button, kEntries[i].icon_path);
        }
        vui_set_user(button, (void *)kEntries[i].path);
        vui_set_tooltip(button, kEntries[i].label);
        vui_on_click(button, launch_app);
        vui_box_add(row, button);
    }

    vui_on_tick(win, dock_on_tick);

    vui_run(win);
}
