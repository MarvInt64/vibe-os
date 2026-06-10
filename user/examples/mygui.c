/*
 * mygui.c — small VexUI app meant to be compiled inside VibeOS with TCC.
 *
 * Inside VibeOS:
 *   tcc /examples/mygui.c /lib/vexui.o /lib/svg.o /lib/libc.a -o /tmp/mygui
 *   chmod 755 /tmp/mygui
 *   /tmp/mygui
 */
#include <stdio.h>
#include <vexui.h>

static vui_widget *g_status;
static vui_widget *g_count_label;
static vui_widget *g_progress;
static vui_widget *g_slider;
static vui_widget *g_name_input;
static int g_count;

static void update_status(const char *prefix) {
    char buf[96];
    const char *name = vui_input_text(g_name_input);
    int level = vui_get_value(g_slider);

    if (name && name[0])
        snprintf(buf, sizeof(buf), "%s %s - count %d - level %d%%",
                 prefix, name, g_count, level);
    else
        snprintf(buf, sizeof(buf), "%s count %d - level %d%%",
                 prefix, g_count, level);

    vui_set_text(g_status, buf);
}

static void refresh_count(void) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", g_count);
    vui_set_text(g_count_label, buf);
}

static void on_increment(vui_widget *button) {
    (void)button;
    g_count++;
    refresh_count();
    update_status("Clicked.");
}

static void on_reset(vui_widget *button) {
    (void)button;
    g_count = 0;
    refresh_count();
    vui_set_value(g_slider, 50);
    vui_set_value(g_progress, 50);
    update_status("Reset.");
}

static void on_slider(vui_widget *slider) {
    vui_set_value(g_progress, vui_get_value(slider));
    update_status("Adjusted.");
}

static void on_name(vui_widget *input) {
    (void)input;
    update_status("Hello");
}

int main(void) {
    vui_window *win = vui_window_open("TCC-built VexUI App", 420, 250);

    vui_label(win, 24, 24, "Compiled inside VibeOS");
    vui_widget *badge = vui_badge(win, 250, 22, "TCC + VexUI");
    vui_set_color(badge, VUI_OK);

    vui_label(win, 24, 62, "Counter");
    g_count_label = vui_label(win, 112, 58, "0");
    vui_set_text_scale(g_count_label, 2);

    vui_widget *inc = vui_button(win, 190, 62, "Increment");
    vui_set_button_width(inc, 110);
    vui_on_click(inc, on_increment);

    vui_widget *reset = vui_button(win, 310, 62, "Reset");
    vui_set_button_width(reset, 78);
    vui_on_click(reset, on_reset);

    vui_label(win, 24, 108, "Name");
    g_name_input = vui_input(win, 90, 102, 210, "type and press Enter");
    vui_on_submit(g_name_input, on_name);

    vui_label(win, 24, 152, "Level");
    g_slider = vui_slider(win, 90, 154, 210, 24, 100);
    vui_set_value(g_slider, 50);
    vui_on_click(g_slider, on_slider);

    g_progress = vui_bar(win, 310, 157, 78, 12, 100);
    vui_set_value(g_progress, 50);

    g_status = vui_label(win, 24, 204, "Ready. Built by TCC inside VibeOS.");

    vui_run(win);
    return 0;
}
