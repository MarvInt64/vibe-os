#include "vexui.h"
#include <sys/syscall.h>
#include <vibeos.h>

struct DockEntry {
    const char *label;
    int icon;            /* line-art icon id: 1=web 2=activity 3=grid 4=code 5=folder */
    uint32_t color;
    const char *path;
};

static const DockEntry kEntries[] = {
    {"Browser", 1, 0x006eb6ffu, "/bin/browser"},
    {"Tasks", 2, 0x0076e0b5u, "/bin/taskmgr"},
    {"Demo", 3, 0x00f4c36bu, "/bin/uidemo"},
    {"C++", 4, 0x008f7bf0u, "/bin/cpptest"},
};

static void launch_app(vui_widget *self) {
    const char *path = (const char *)vui_get_user(self);
    if (path) vos_spawn(path);
}

int main() {
    uint32_t mode = (uint32_t)__sc2(SYS_DISPLAY_MODE, 0, 0);
    int screen_w = (int)((mode >> 16) & 0xffffu);
    int screen_h = (int)(mode & 0xffffu);
    int width = 344;
    int height = 84;
    int x;
    int y;

    if (screen_w <= 0) screen_w = 1024;
    if (screen_h <= 0) screen_h = 768;
    if (width > screen_w - 24) width = screen_w - 24;
    x = (screen_w - width) / 2;
    y = screen_h - height - 22;
    if (y < 0) y = 0;

    vui_window *win = vui_window_open_ex(
        "Dock", width, height,
        VUI_WINDOW_FRAMELESS | VUI_WINDOW_NO_DOCK |
            VUI_WINDOW_POSITIONED | VUI_WINDOW_ALWAYS_ON_TOP,
        x, y);
    vui_set_clear_color(win, VUI_COLOR_TRANSPARENT);

    vui_widget *surface = vui_card(win, 0, 0, width, height, "");
    vui_set_color(surface, 0x00122232u);   /* spec dock glass background */

    vui_widget *row = vui_hbox(win, 20, 14, width - 40, height - 28);
    vui_set_gap(row, 22);
    vui_set_padding(row, 2);

    for (unsigned i = 0; i < sizeof(kEntries) / sizeof(kEntries[0]); ++i) {
        vui_widget *button = vui_tile_button(win, 0, 0, "");
        vui_set_color(button, kEntries[i].color);
        vui_set_value(button, kEntries[i].icon);
        vui_set_user(button, (void *)kEntries[i].path);
        vui_on_click(button, launch_app);
        vui_box_add(row, button);
    }

    vui_run(win);
}
