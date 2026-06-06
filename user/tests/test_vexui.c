/*
 * test_vexui.c — TCC-compiled VexUI program (minimal window).
 *
 * Compile inside VibeOS:
 *   tcc /tests/test_vexui.c /lib/vexui.o /lib/svg.o /lib/libc.a -o /tmp/tv
 *   /tmp/tv
 *
 * Opens a small window with a button. Clicking the button changes its label.
 * The window can be closed normally (titlebar X or dock).
 */
#include <vexui.h>

static int clicks = 0;

static void on_click(vui_widget *btn) {
    clicks++;
    if (clicks == 1)
        vui_set_text(btn, "Clicked 1x");
    else {
        char buf[32];
        /* snprintf from libc via #include <stdio.h> */
        extern int snprintf(char *buf, unsigned long size, const char *fmt, ...);
        snprintf(buf, sizeof(buf), "Clicked %dx", clicks);
        vui_set_text(btn, buf);
    }
}

int main(void) {
    /* Open a 320×200 window. Titlebar lets user close it. */
    vui_window *win = vui_window_open("TCC VexUI Test", 320, 200);

    /* Static label at top */
    vui_label(win, 20, 30, "Compiled with TCC inside VibeOS!");

    /* Button in the middle */
    vui_widget *btn = vui_button(win, 80, 80, "Click Me");
    vui_on_click(btn, on_click);

    /* Small note at bottom */
    vui_label(win, 20, 150, "Close this window to exit.");

    vui_run(win);
}
