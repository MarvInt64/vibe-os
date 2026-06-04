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
#define VUI_PROCESS_MAX 48  /* must match kernel PROCESS_MAX_COUNT */

#define VUI_WINDOW_FRAMELESS  0x00000001u
#define VUI_WINDOW_NO_DOCK    0x00000002u
#define VUI_WINDOW_POSITIONED 0x00000004u
#define VUI_WINDOW_ALWAYS_ON_TOP 0x00000008u
#define VUI_WINDOW_TRANSLUCENT   0x00000010u
#define VUI_WINDOW_NO_SHADOW     0x00000020u   /* suppress drop shadow (shell panels) */
#define VUI_WINDOW_ASPECT_RATIO  0x00000040u   /* lock window resizing to its initial aspect ratio */

typedef struct vui_window vui_window;
typedef struct vui_widget vui_widget;
typedef void (*vui_callback)(vui_widget *self);
typedef void (*vui_tick_callback)(vui_window *window);
typedef void (*vui_resize_callback)(vui_window *window, int width, int height);
typedef void (*vui_context_callback)(vui_window *window, int x, int y);
/* Raw keystroke delivered to the app when no input widget is focused. */
typedef void (*vui_key_callback)(vui_window *window, unsigned int key);
/* Mouse-wheel scroll; delta is the signed notch count (positive = away/up). */
typedef void (*vui_scroll_callback)(vui_window *window, int delta);
typedef void (*vui_menu_callback)(vui_window *window);
typedef void (*vui_mouse_callback)(vui_window *window, int x, int y);

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
/* Like vui_window_open_ex, but with shadow_inset_top: the drop shadow is
 * drawn starting shadow_inset_top pixels below the window top, so a
 * transparent header zone (e.g. tooltip headroom) does not cast a shadow. */
vui_window *vui_window_open_inset(const char *title, int width, int height,
                                  vui_u32 flags, int x, int y,
                                  int shadow_inset_top);

/* ---- Create persistent widgets (retained) ---- */
vui_widget *vui_panel(vui_window *w, int x, int y, int width, int height, const char *title);
vui_widget *vui_card(vui_window *w, int x, int y, int width, int height, const char *title);
vui_widget *vui_label(vui_window *w, int x, int y, const char *text);
vui_widget *vui_button(vui_window *w, int x, int y, const char *text);
/* Fully-rounded pill button (tags / quick actions). */
vui_widget *vui_pill_button(vui_window *w, int x, int y, const char *text);
vui_widget *vui_tile_button(vui_window *w, int x, int y, const char *text);
/* Bare SVG icon/logo (no chrome). Set the artwork via vui_set_icon_svg[_path]. */
vui_widget *vui_image(vui_window *w, int x, int y, int size);
vui_widget *vui_input(vui_window *w, int x, int y, int width, const char *placeholder);
/* Sidebar/list row: left-aligned SVG icon + label. Hover highlights; mark the
 * active row with vui_set_running(item, 1) for a tinted surface + accent bar.
 * Assign the icon via vui_set_icon_svg and a click handler via vui_on_click. */
vui_widget *vui_listitem(vui_window *w, int x, int y, int width, int height, const char *label);
vui_widget *vui_badge(vui_window *w, int x, int y, const char *text);
/* Low-level pixel buffer widget for custom rendering (games, browser).
 * The pixels buffer must be at least stride * height * 4 bytes. */
vui_widget *vui_canvas(vui_window *w, int x, int y, int width, int height, vui_u32 *pixels);
vui_widget *vui_canvas_ex(vui_window *w, int x, int y, int width, int height, vui_u32 *pixels, int stride);
vui_widget *vui_tabs(vui_window *w, int x, int y, int width, const char *labels, int active);
vui_widget *vui_bar(vui_window *w, int x, int y, int width, int height, int max);
/* Interactive horizontal slider (track + draggable handle). value 0..max;
 * the on_click callback fires whenever a drag/click changes the value. */
vui_widget *vui_slider(vui_window *w, int x, int y, int width, int height, int max);
/* Decorative mini line-graph (sparkline) for metric cards. */
vui_widget *vui_sparkline(vui_window *w, int x, int y, int width, int height);
/* Set an integer text scale on a label (1 = normal, 2 = double size, …). */
void vui_set_text_scale(vui_widget *wd, int scale);
/* Large rounded pill surface (e.g. the dock bar). */
vui_widget *vui_pill(vui_window *w, int x, int y, int width, int height);
/* Self-contained metric card (title + big value + sub-label + chart/bar).
 * mode: 0 = area chart, 1 = progress bar (set percent via vui_set_value). */
vui_widget *vui_metric(vui_window *w, int x, int y, int width, int height,
                       const char *title, int mode);
/* Set a metric card's big value and sub-label (e.g. "12%", "2.1 GHz"). */
void vui_set_metric(vui_widget *wd, const char *value, const char *sub);
/* Append a live sample (0..100) to a metric card's history chart. */
void vui_metric_push(vui_widget *wd, int sample);

/* ---- Configure widgets ---- */
void vui_on_click(vui_widget *b, vui_callback cb);
/* W_INPUT only: callback fired when Enter/Return is pressed in the input. Use
 * this for "submit on Enter" (e.g. a URL bar); on_click fires on every keystroke. */
void vui_on_submit(vui_widget *b, vui_callback cb);
void vui_set_text(vui_widget *wgt, const char *text);
void vui_set_int(vui_widget *wgt, int value);          /* label: show a number */
int  vui_get_int(vui_widget *wgt);
void vui_set_value(vui_widget *wgt, int value);        /* bar: progress value */
void vui_set_color(vui_widget *wgt, vui_u32 color);
/* Assign a compact stroked SVG icon to a tile-style widget. The renderer
 * supports the icon subset VexUI uses: rect, circle and path strokes. */
void vui_set_icon_svg(vui_widget *wgt, const char *svg);
int  vui_set_icon_svg_path(vui_widget *wgt, const char *path);
void vui_set_user(vui_widget *wgt, void *user);        /* attach app data */
void *vui_get_user(vui_widget *wgt);
/* Current text typed into an input field (empty string if none). */
const char *vui_input_text(vui_widget *wgt);
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
/* Register a raw-key handler; fired for keystrokes when no input is focused. */
void vui_on_key(vui_window *w, vui_key_callback cb);
/* Set a tooltip text shown as a bubble above the widget after a short hover.
 * Pass NULL or "" to remove. Works on buttons, tile-buttons, and pill-buttons. */
void vui_set_tooltip(vui_widget *wgt, const char *tip);
void vui_set_running(vui_widget *wd, int running);
/* Register a mouse-wheel handler for this window. */
void vui_on_scroll(vui_window *w, vui_scroll_callback cb);
/* Register mouse move, click, and release handlers for this window. */
void vui_on_mouse_move(vui_window *w, vui_mouse_callback cb);
void vui_on_mouse_click(vui_window *w, vui_mouse_callback cb);
void vui_on_mouse_release(vui_window *w, vui_mouse_callback cb);
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
/* Present only the given canvas widget's region (no full-window repaint). Use
 * for high-frame-rate animation of one area while the chrome stays static. */
void vui_canvas_flush(vui_window *w, vui_widget *canvas);

/* ---- Standard Dialogs --------------------------------------------------- */

/**
 * Open a standard file selection dialog (blocks the caller).
 * @param title The dialog title (e.g. "Open File").
 * @param initial_path Starting directory (e.g. "/home/user").
 * @param out_path Buffer to receive the selected path.
 * @param out_cap Capacity of the output buffer.
 * @param save_mode 0 = Open mode, 1 = Save mode (allows filename entry).
 * @return 1 if a file was selected, 0 if cancelled or error.
 */
int vui_file_dialog(const char *title, const char *initial_path, char *out_path, int out_cap, int save_mode);

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
/* Push the window's W_MENU entries to the window server so the top bar shows
 * the focused app's menus.  Call once after adding all menus. */
void vui_sync_menubar(vui_window *w);

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
/* The window-server id of a VexUI window (e.g. for vos_window_set_menubar). */
int  vui_window_id(vui_window *w);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
