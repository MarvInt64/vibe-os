#include "vexui.h"
#include <vibeos.h>
#include <string.h>

#define PROCESS_STATE_EMPTY  0
#define PROCESS_STATE_EXITED 5

struct DockEntry {
    const char *label;
    int         icon;
    uint32_t    color;
    const char *path;
    const char *icon_path;
};

static DockEntry g_entries[] = {
    {"Terminal", 4, 0x002ea8ffu, "/bin/terminal", "/icons/dock/terminal.svg"},
    {"Files",    5, 0x002ea8ffu, "/bin/filebrowser", "/icons/dock/filebrowser.svg"},
    {"Browser",  1, 0x002ea8ffu, "/bin/browser", "/icons/dock/browser.svg"},
    {"Editor",   4, 0x002ea8ffu, "/bin/texteditor", "/icons/dock/terminal.svg"},
    {"Tasks",    2, 0x002ea8ffu, "/bin/taskmgr", "/icons/dock/taskmgr.svg"},
    {"Player",   3, 0x002ea8ffu, "/bin/audioplayer", "/icons/dock/player.svg"},
};
#define DOCK_COUNT (int)(sizeof(g_entries) / sizeof(g_entries[0]))

static vui_widget *sButtons[6];

static void apply_entry(int idx) {
    vui_widget *b = sButtons[idx];
    DockEntry *e = &g_entries[idx];
    vui_set_color(b, e->color);
    vui_set_value(b, e->icon);
    if (e->icon_path) vui_set_icon_svg_path(b, e->icon_path);
    vui_set_user(b, (void *)e->path);
    vui_set_tooltip(b, e->label);
}

static void swap_entries(int a, int b) {
    if (a == b || a < 0 || b < 0 || a >= DOCK_COUNT || b >= DOCK_COUNT) return;
    DockEntry tmp = g_entries[a];
    g_entries[a] = g_entries[b];
    g_entries[b] = tmp;
    apply_entry(a);
    apply_entry(b);
}

static void dock_on_tick(vui_window *win) {
    (void)win;
    for (int i = 0; i < DOCK_COUNT; i++) {
        bool running = false;
        const char *path = g_entries[i].path;
        const char *name = path;
        for (const char *p = path; *p; p++)
            if (*p == '/') name = p + 1;
        for (unsigned s = 0; s < VUI_PROCESS_MAX; s++) {
            vui_process_info p;
            if (vui_process_snapshot(s, &p) > 0 && p.loaded
                && p.state != PROCESS_STATE_EMPTY && p.state != PROCESS_STATE_EXITED) {
                if (strcmp(p.name, name) == 0 || strcmp(p.name, path) == 0) {
                    running = true; break;
                }
            }
        }
        vui_set_running(sButtons[i], running ? 1 : 0);
    }
}

static void launch_app(vui_widget *self) {
    const char *path = (const char *)vui_get_user(self);
    if (path) vos_spawn(path);
}

/* ---- Drag support (window-level callbacks) ----------------------------- */
static int  g_held_idx   = -1;
static int  g_held_ticks = 0;
static int  g_drag_on    = 0;

static int icon_at(int mx, int my) {
    if (my < 38 || my > 38 + 58) return -1;
    for (int i = 0; i < DOCK_COUNT; i++) {
        int ix = 50 + i * (58 + 38);
        if (mx >= ix && mx <= ix + 58) return i;
    }
    return -1;
}

static void on_mouse_dn(vui_window *win, int x, int y, vui_u32 btns) {
    (void)win; (void)btns;
    vos_log(VOS_LOG_APP, "dock: mouse-down");
    g_held_idx   = icon_at(x, y);
    g_held_ticks = 0;
    g_drag_on    = 0;
}

static void on_mouse_up(vui_window *win, int x, int y, vui_u32 btns) {
    (void)win; (void)btns;
    if (g_drag_on && g_held_idx >= 0) {
        int dst = icon_at(x, y);
        if (dst >= 0 && dst != g_held_idx)
            swap_entries(g_held_idx, dst);
    } else if (g_held_idx >= 0 && !g_drag_on) {
        /* Short click — launch the app on release. */
        const char *path = (const char *)vui_get_user(sButtons[g_held_idx]);
        if (path) vos_spawn(path);
    }
    g_held_idx   = -1;
    g_held_ticks = 0;
    g_drag_on    = 0;
}

static void on_mouse_mv(vui_window *win, int x, int y, vui_u32 btns) {
    (void)win; (void)btns;
    if (!g_drag_on || g_held_idx < 0) return;
    int dst = icon_at(x, y);
    if (dst >= 0 && dst != g_held_idx) {
        swap_entries(g_held_idx, dst);
        g_held_idx = dst;
    }
}

static void dock_tick2(vui_window *win) {
    dock_on_tick(win);
    if (g_held_idx >= 0 && !g_drag_on) {
        if (++g_held_ticks > 25) {
            g_drag_on = 1;
            vos_log(VOS_LOG_APP, "dock: drag started");
        }
    }
}

int main() {
    uint32_t mode = vos_display_mode_get();
    int screen_w = (int)((mode >> 16) & 0xffffu);
    int screen_h = (int)(mode & 0xffffu);
    int width, dock_h = 78, tooltip_h = 28, height = dock_h + tooltip_h, x, y;

    if (screen_w <= 0) screen_w = 1024;
    if (screen_h <= 0) screen_h = 768;
    width = 680;
    if (screen_w < 900) width = screen_w - 40;
    if (width > screen_w - 48) width = screen_w - 48;
    x = (screen_w - width) / 2;
    y = screen_h - height - 24;
    if (y < 0) y = 0;

    vos_log(VOS_LOG_APP, "dock ready");

    vui_window *win = vui_window_open_inset(
        "Dock", width, height,
        VUI_WINDOW_FRAMELESS | VUI_WINDOW_NO_DOCK |
            VUI_WINDOW_POSITIONED | VUI_WINDOW_ALWAYS_ON_TOP |
            VUI_WINDOW_TRANSLUCENT | VUI_WINDOW_SINGLE_INSTANCE,
        x, y, tooltip_h);
    vui_set_clear_color(win, VUI_COLOR_TRANSPARENT);

    vui_widget *surface = vui_pill(win, 0, tooltip_h, width, dock_h);
    vui_set_anchor(surface, VUI_ANCHOR_LEFT | VUI_ANCHOR_TOP | VUI_ANCHOR_RIGHT);
    vui_set_color(surface, 0x000b2342u);

    vui_widget *row = vui_hbox(win, 50, tooltip_h + 10, width - 100, dock_h - 18);
    vui_set_anchor(row, VUI_ANCHOR_LEFT | VUI_ANCHOR_TOP | VUI_ANCHOR_RIGHT);
    vui_set_gap(row, 38);
    vui_set_padding(row, 0);

    for (int i = 0; i < DOCK_COUNT; i++) {
        vui_widget *button = vui_tile_button(win, 0, 0, "");
        sButtons[i] = button;
        vui_set_size(button, 58, 58);
        apply_entry(i);
        vui_box_add(row, button);
    }

    vui_on_tick(win, dock_tick2);
    vui_on_mouse_click(win, on_mouse_dn);
    vui_on_mouse_release(win, on_mouse_up);
    vui_on_mouse_move(win, on_mouse_mv);

    vui_run(win);
}
