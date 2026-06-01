/* libvexui — retained-mode GUI toolkit for VibeOS userspace apps.
 *
 * Widgets are persistent objects you create once and attach callbacks to.
 * The framework owns the event loop (vui_run), dispatches events to widgets,
 * and only repaints when something actually changed. All kernel syscalls are
 * hidden inside the library; app code never touches them.
 *
 *     #include "vexui.h"
 *     static vui_widget *counter;
 *     static int n;
 *     static void on_click(vui_widget *self) { vui_set_int(counter, ++n); }
 *
 *     void _start(void) {
 *         vui_window *win = vui_window_open("Demo", 360, 220);
 *         vui_panel(win, 16, 16, 328, 70, "Counter");
 *         vui_widget *b = vui_button(win, 28, 44, "Increment");
 *         vui_on_click(b, on_click);
 *         counter = vui_label(win, 200, 50, "0");
 *         vui_run(win);          // runs until the window is closed
 *     }
 */
#ifndef VEXUI_H
#define VEXUI_H

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned int vui_u32;

/* ---- Shared theme (consistent look across all apps) ---- */
#define VUI_BG          0x00161d2bu
#define VUI_PANEL       0x001f2942u
#define VUI_ACCENT      0x008f7bf0u
#define VUI_ACCENT_DIM  0x005a4f93u
#define VUI_TEXT        0x00e6edf7u
#define VUI_TEXT_DIM    0x009aa7bau
#define VUI_BORDER      0x00344259u
#define VUI_OK          0x0050c98au
#define VUI_WARN        0x00f0b86eu
#define VUI_DANGER      0x00ef7f7fu
#define VUI_COLOR_TRANSPARENT 0x00ff00ffu
#define VUI_PROCESS_MAX 8   /* must match kernel PROCESS_MAX_COUNT */

#define VUI_WINDOW_FRAMELESS  0x00000001u
#define VUI_WINDOW_NO_DOCK    0x00000002u
#define VUI_WINDOW_POSITIONED 0x00000004u
#define VUI_WINDOW_ALWAYS_ON_TOP 0x00000008u

typedef struct vui_window vui_window;
typedef struct vui_widget vui_widget;
typedef void (*vui_callback)(vui_widget *self);
typedef void (*vui_tick_callback)(vui_window *window);
typedef void (*vui_resize_callback)(vui_window *window, int width, int height);
typedef void (*vui_context_callback)(vui_window *window, int x, int y);
typedef void (*vui_menu_callback)(vui_window *window);

typedef struct vui_theme {
    vui_u32 bg;
    vui_u32 surface;
    vui_u32 surface_hi;
    vui_u32 border;
    vui_u32 border_hi;
    vui_u32 text;
    vui_u32 text_dim;
    vui_u32 accent;
    vui_u32 ok;
    vui_u32 warn;
    vui_u32 danger;
    vui_u32 menu_bg;
    vui_u32 menu_item;
    vui_u32 menu_muted;
    unsigned char radius;
    unsigned char padding;
} vui_theme;

const vui_theme *vui_theme_default(void);
void vui_set_theme(const vui_theme *theme);
int  vui_load_theme(const char *path);

typedef struct vui_process_info {
    unsigned int pid;
    unsigned int parent_pid;
    unsigned long runtime_ticks;
    unsigned long switch_count;
    unsigned long preempt_count;
    unsigned long wake_tick;
    unsigned char state;
    unsigned char loaded;
    char name[32];
    /* Must match struct process_snapshot in kernel/include/process.h. */
    unsigned long mem_bytes;     /* physical RAM footprint (bytes)        */
    unsigned int  thread_count;  /* threads sharing the address space (>=1) */
} vui_process_info;

/* Open a window with a content area of width x height pixels.
 * Always returns a valid window: if no window server is running, the toolkit
 * prints a hint and terminates the process, so callers need not null-check. */
vui_window *vui_window_open(const char *title, int width, int height);
vui_window *vui_window_open_ex(const char *title, int width, int height,
                               vui_u32 flags, int x, int y);

/* ---- Create persistent widgets (retained) ---- */
vui_widget *vui_panel(vui_window *w, int x, int y, int width, int height, const char *title);
vui_widget *vui_card(vui_window *w, int x, int y, int width, int height, const char *title);
vui_widget *vui_label(vui_window *w, int x, int y, const char *text);
vui_widget *vui_button(vui_window *w, int x, int y, const char *text);
vui_widget *vui_tile_button(vui_window *w, int x, int y, const char *text);
vui_widget *vui_input(vui_window *w, int x, int y, int width, const char *placeholder);
vui_widget *vui_badge(vui_window *w, int x, int y, const char *text);
vui_widget *vui_tabs(vui_window *w, int x, int y, int width, const char *labels, int active);
vui_widget *vui_bar(vui_window *w, int x, int y, int width, int height, int max);
/* Decorative mini bar-graph (sparkline) for metric cards. */
vui_widget *vui_sparkline(vui_window *w, int x, int y, int width, int height);

/* ---- Configure widgets ---- */
void vui_on_click(vui_widget *b, vui_callback cb);
void vui_set_text(vui_widget *wgt, const char *text);
void vui_set_int(vui_widget *wgt, int value);          /* label: show a number */
int  vui_get_int(vui_widget *wgt);
void vui_set_value(vui_widget *wgt, int value);        /* bar: progress value */
void vui_set_color(vui_widget *wgt, vui_u32 color);
void vui_set_user(vui_widget *wgt, void *user);        /* attach app data */
void *vui_get_user(vui_widget *wgt);
void vui_set_visible(vui_widget *wgt, int visible);
void vui_set_bounds(vui_widget *wgt, int x, int y, int width, int height);
/* Set only width/height; leave position unchanged (pass 0 to skip an axis). */
void vui_set_size(vui_widget *wgt, int width, int height);
void vui_set_button_width(vui_widget *wgt, int width);
#define VUI_ANCHOR_LEFT   1
#define VUI_ANCHOR_TOP    2
#define VUI_ANCHOR_RIGHT  4
#define VUI_ANCHOR_BOTTOM 8
/* Keep a widget attached to window edges. LEFT|RIGHT grows horizontally;
 * RIGHT alone follows the right edge; TOP|BOTTOM grows vertically. */
void vui_set_anchor(vui_widget *wgt, int anchors);
void vui_set_clear_color(vui_window *w, vui_u32 color);
int vui_window_width(vui_window *w);
int vui_window_height(vui_window *w);
void vui_on_tick(vui_window *w, vui_tick_callback cb);
void vui_on_resize(vui_window *w, vui_resize_callback cb);
void vui_on_context_menu(vui_window *w, vui_context_callback cb);
/* Declare an entry for this window's dock-icon context menu. The window server
 * shows it (below the standard Show/Hide) and invokes `cb` when picked. Up to
 * 6 entries; extra calls are ignored. */
/* Add an entry to the window's dock/taskbar context menu (right-click on the
 * app's icon in the dock).  Up to 6 entries; extra calls are ignored.
 * Note: this is distinct from the in-window menu bar (vui_menubar). */
void vui_add_dock_item(vui_window *w, const char *label, vui_menu_callback cb);
/* Deprecated alias kept for source compatibility. */
static inline void vui_add_menu_item(vui_window *w, const char *label,
                                     vui_menu_callback cb) {
    vui_add_dock_item(w, label, cb);
}
void vui_request_repaint(vui_window *w);

/* ---- Layout containers -------------------------------------------------- */

/* Create a vertical-stack container (children arranged top-to-bottom).
 * The container itself is invisible; it only controls child geometry. */
vui_widget *vui_vbox(vui_window *w, int x, int y, int width, int height);

/* Create a horizontal-stack container (children arranged left-to-right). */
vui_widget *vui_hbox(vui_window *w, int x, int y, int width, int height);

/* Assign child to container.  Must be called after both widgets are created
 * in the same window.  A widget may only have one parent box at a time. */
void vui_box_add(vui_widget *container, vui_widget *child);

/* Gap between children on the main axis (pixels, default 4). */
void vui_set_gap(vui_widget *container, int gap);

/* Uniform inner padding on all four sides (pixels, default 0). */
void vui_set_padding(vui_widget *container, int padding);

/* Child flag: stretch the child to fill the container's full cross-axis.
 * VBox children with fill get width = container_width − 2 × padding.
 * HBox children with fill get height = container_height − 2 × padding. */
void vui_set_fill(vui_widget *child);

/* Child flag: grow to consume any leftover space on the main axis.
 * Multiple expand children share the free space evenly. */
void vui_set_expand(vui_widget *child);

/* ---- In-window menu bar ------------------------------------------------- */

/* Height reserved for the menu bar at the top of the window.
 * When a menu bar is present, place other content at y >= VUI_MENUBAR_HEIGHT. */
#define VUI_MENUBAR_HEIGHT 22

/* Create the menu bar widget.  It is always anchored to the top and stretches
 * to the full window width.  Call this before creating other widgets so that
 * panels and boxes placed below it start at y = VUI_MENUBAR_HEIGHT. */
vui_widget *vui_menubar(vui_window *w);

/* Add a dropdown menu to a menu bar (e.g. "File", "Edit", "View").
 * Returns a menu widget; attach items to it with vui_menuitem(). */
vui_widget *vui_menu(vui_window *w, vui_widget *menubar, const char *title);

/* Add a clickable item to a dropdown menu.
 * Register an action callback with vui_on_click(item, cb). */
vui_widget *vui_menuitem(vui_window *w, vui_widget *menu, const char *label);

/* Add a horizontal separator line inside a menu (not clickable). */
vui_widget *vui_menu_separator(vui_window *w, vui_widget *menu);

/* ---- Small system helpers for VexUI apps ---- */
int vui_process_snapshot(unsigned int slot, vui_process_info *out);
int vui_process_kill(unsigned int pid);

/* ---- Run the framework event loop until the window closes ---- */
void vui_run(vui_window *w) __attribute__((noreturn));
void vui_quit(vui_window *w);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
