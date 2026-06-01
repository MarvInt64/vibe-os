/* uidemo — example VibeOS GUI app (retained mode). No syscalls, no kernel
 * headers: just the libvexui object API with callbacks. */
#include "vexui.h"

static vui_widget *g_counter;
static vui_widget *g_bar;
static int g_clicks;

static void on_increment(vui_widget *self) {
    (void)self;
    g_clicks++;
    vui_set_int(g_counter, g_clicks);
    vui_set_value(g_bar, g_clicks % 11);
}

static void on_reset(vui_widget *self) {
    (void)self;
    g_clicks = 0;
    vui_set_int(g_counter, 0);
    vui_set_value(g_bar, 0);
}

/* Dock context-menu entry, declared by the app itself via vui_add_menu_item. */
static void on_menu_bump(vui_window *w) {
    g_clicks += 5;
    vui_set_int(g_counter, g_clicks);
    vui_set_value(g_bar, g_clicks % 11);
    vui_request_repaint(w);
}

void __attribute__((noreturn)) _start(void) {
    /* If no window server is running, vui_window_open() prints a hint and
     * exits the process for us -- the app never sees a NULL window. */
    vui_window *win = vui_window_open("UI Demo", 360, 230);
    vui_widget *b1;
    vui_widget *b2;

    vui_label(win, 16, 10, "VEXUI TOOLKIT (RETAINED MODE)");

    vui_set_anchor(vui_panel(win, 16, 36, 328, 96, "Buttons"), VUI_ANCHOR_LEFT | VUI_ANCHOR_TOP | VUI_ANCHOR_RIGHT);
    b1 = vui_button(win, 28, 66, "Increment");
    vui_on_click(b1, on_increment);
    b2 = vui_button(win, 170, 66, "Reset");
    vui_on_click(b2, on_reset);

    vui_label(win, 28, 104, "Clicks:");
    g_counter = vui_label(win, 110, 104, "0");
    vui_set_color(g_counter, VUI_OK);

    vui_set_anchor(vui_panel(win, 16, 142, 328, 72, "Progress"), VUI_ANCHOR_LEFT | VUI_ANCHOR_TOP | VUI_ANCHOR_RIGHT | VUI_ANCHOR_BOTTOM);
    g_bar = vui_bar(win, 28, 172, 300, 18, 10);
    vui_set_anchor(g_bar, VUI_ANCHOR_LEFT | VUI_ANCHOR_TOP | VUI_ANCHOR_RIGHT);

    /* App-declared dock context-menu entry (right-click the dock icon). */
    vui_add_menu_item(win, "Bump +5", on_menu_bump);

    vui_run(win);   /* framework loop: dispatches events, repaints on change */
}
