#ifndef VIBEOS_WINDOW_H
#define VIBEOS_WINDOW_H

#include "app.h"
#include "framebuffer.h"
#include "input.h"
#include "winsys.h"

/* Number of concurrent userspace app windows supported. Slot backing buffers
 * are now heap-allocated at each window's actual size (see user_app_slot), so
 * this count costs only small fixed per-slot state, not full-screen buffers. */
#define MAX_USER_APPS 16

enum window_id {
    WINDOW_INFO = 0,
    WINDOW_FILES = 1,
    WINDOW_TERMINAL = 2,
    WINDOW_TASK_MANAGER = 3,
    /* First slot for a userspace app window. Slots 4..4+MAX_USER_APPS-1 are
     * all userspace app windows. Only the four kernel windows above get
     * desktop icons. */
    WINDOW_APP_FIRST = 4,
    WINDOW_APP = 4, /* backward-compat alias */
    WINDOW_COUNT = 4 + MAX_USER_APPS
};

#define DESKTOP_ICON_COUNT 4

/* Max size of a userspace app window's content area. (Kept modest so the
 * kernel-side backing buffers stay within BSS budget; the per-process user
 * memory region also limits how big an app's own pixel buffer can be.) */
#define WINDOW_APP_CONTENT_MAX_WIDTH WINSYS_MAX_WIDTH
#define WINDOW_APP_CONTENT_MAX_HEIGHT WINSYS_MAX_HEIGHT
#define WINDOW_APP_SURFACE_MAX_WIDTH (WINSYS_MAX_WIDTH + 64u)
#define WINDOW_APP_SURFACE_MAX_HEIGHT (WINSYS_MAX_HEIGHT + 96u)
#define WINSYS_EVENT_QUEUE 32
#define DESKTOP_LAUNCHER_MAX 12

#define DESKTOP_MAX_WIDTH 1920u
#define DESKTOP_MAX_HEIGHT 1080u
#define WINDOW_INFO_MAX_WIDTH 620u
#define WINDOW_INFO_MAX_HEIGHT 320u
#define WINDOW_FILES_MAX_WIDTH 420u
#define WINDOW_FILES_MAX_HEIGHT 320u
#define WINDOW_TERMINAL_MAX_WIDTH 1180u
#define WINDOW_TERMINAL_MAX_HEIGHT 700u
#define WINDOW_TASKS_MAX_WIDTH 640u
#define WINDOW_TASKS_MAX_HEIGHT 560u

struct window_state {
    const char *title;
    int x;
    int y;
    int width;
    int height;
    uint32_t body;
    uint32_t titlebar;
    uint32_t accent;
    uint32_t flags;
    uint8_t  app_slot;
    uint8_t  visible;
    uint8_t  maximized;
    uint8_t  shadow_inset_top; /* pixels to skip shadow from window top */
    int restore_x;
    int restore_y;
    int restore_width;
    int restore_height;
};

/* Per-slot state for a userspace app window.  Each of the MAX_USER_APPS
 * concurrent app windows gets its own copy of these fields instead of the
 * single flat set that used to live directly in desktop_state. */
struct user_app_slot {
    /* Window backing stores, allocated on the kernel heap at the window's
     * actual size (not a fixed worst-case array), grown on resize and freed on
     * close — so memory scales with what is really on screen, like a real OS.
     * The content buffer's row stride is content_width; the surface buffer's
     * stride is the framed window width. *_cap_px track the allocated pixel
     * capacity so interactive resize grows the buffer without thrashing. */
    uint32_t *surface_storage;
    uint32_t *content_storage;
    int surface_cap_px;
    int content_cap_px;
    int content_width;
    int content_height;
    /* Row stride (in px) of the pixels CURRENTLY stored in content_storage —
     * i.e. the width of the app's last present. It lags content_width during an
     * interactive resize (content_width is the new target; the stored pixels are
     * still laid out at the old width until the app presents again). The
     * compositor must read at data_width to avoid shearing mid-resize. 0 = no
     * content presented yet. */
    int data_width;
    uint8_t created;
    uint32_t pid;
    struct winsys_event events[WINSYS_EVENT_QUEUE];
    int event_head;
    int event_tail;
    char title[64];
    int menu_count;
    struct winsys_menu_item menu[WINSYS_MAX_MENU_ITEMS];
    int menubar_count;
    struct winsys_menubar_item menubar[WINSYS_MAX_MENUBAR_ITEMS];
};

struct desktop_state {
    uint32_t screen_width;
    uint32_t screen_height;
    struct window_state windows[WINDOW_COUNT];
    struct app_instance apps[WINDOW_COUNT];
    union app_state_storage app_storage[WINDOW_COUNT];
    struct framebuffer background_fb;
    struct framebuffer window_fbs[WINDOW_COUNT];
    int z_order[WINDOW_COUNT];
    int focused_window;
    /* Last focused "real" app window (not a shell panel like the dock or top
     * bar). The top-bar app shows this window's menu and is the target of menu
     * actions, so clicking the bar itself doesn't hijack the menu. */
    int menu_target_window;
    int dragging_window;
    int drag_offset_x;
    int drag_offset_y;
    int resizing_window;
    int resize_edges;
    int resize_start_mouse_x;
    int resize_start_mouse_y;
    int resize_start_x;
    int resize_start_y;
    int resize_start_width;
    int resize_start_height;
    int mouse_x;
    int mouse_y;
    uint8_t mouse_buttons;
    uint8_t background_dirty;
    uint8_t window_dirty[WINDOW_COUNT];
    uint8_t dirty;
    
    /* Tile-based dirty tracking for efficient rendering */
    #define MAX_TILE_BIT_ARRAY 1024
    uint8_t dirty_tiles[MAX_TILE_BIT_ARRAY];
    int tile_size;
    int tiles_x;
    int tiles_y;

    struct rect dirty_rect;
    struct rect window_dirty_rects[WINDOW_COUNT];
    /* Optional desktop wallpaper. wallpaper_active=0 => procedural gradient.
     * Stored in a static buffer inside desktop_state (like background_storage)
     * so it is mapped in every page-table context the compositor may run under
     * — a kmalloc heap buffer is not guaranteed to be. */
    int       wallpaper_active;
    uint32_t background_storage[DESKTOP_MAX_WIDTH * DESKTOP_MAX_HEIGHT];
    uint32_t wallpaper_storage[DESKTOP_MAX_WIDTH * DESKTOP_MAX_HEIGHT];
    uint32_t info_surface_storage[WINDOW_INFO_MAX_WIDTH * WINDOW_INFO_MAX_HEIGHT];
    uint32_t files_surface_storage[WINDOW_FILES_MAX_WIDTH * WINDOW_FILES_MAX_HEIGHT];
    uint32_t terminal_surface_storage[WINDOW_TERMINAL_MAX_WIDTH * WINDOW_TERMINAL_MAX_HEIGHT];
    uint32_t tasks_surface_storage[WINDOW_TASKS_MAX_WIDTH * WINDOW_TASKS_MAX_HEIGHT];

    /* Userspace app windows: up to MAX_USER_APPS concurrent apps, each with
     * its own pixel buffers, event queue, PID and title. */
    struct user_app_slot user_apps[MAX_USER_APPS];

    uint8_t context_menu_open;
    int context_menu_x;
    int context_menu_y;
    int context_menu_window;
    int context_menu_hover;

    uint8_t modal_open;
    char modal_title[48];
    char modal_message[160];

    /* User desktop launchers loaded from .desktop files in /home/user/Desktop.
     * Each launcher is a small text file with Name= and Exec= keys. */
    char launcher_names[DESKTOP_LAUNCHER_MAX][32];
    char launcher_execs[DESKTOP_LAUNCHER_MAX][64];
    char launcher_paths[DESKTOP_LAUNCHER_MAX][96];
    uint32_t launcher_colors[DESKTOP_LAUNCHER_MAX];
    int launcher_x[DESKTOP_LAUNCHER_MAX];
    int launcher_y[DESKTOP_LAUNCHER_MAX];
    int launcher_count;
    int dragging_launcher;
    int launcher_drag_offset_x;
    int launcher_drag_offset_y;
    int launcher_drag_start_x;
    int launcher_drag_start_y;
    uint8_t launcher_drag_moved;
};

void desktop_init(struct desktop_state *desktop, uint32_t screen_width, uint32_t screen_height);
void desktop_handle_input(struct desktop_state *desktop, const struct mouse_state *mouse, const struct keyboard_state *keyboard);
void desktop_poll_apps(struct desktop_state *desktop);
void desktop_render(struct desktop_state *desktop, struct framebuffer *fb);
void desktop_render_rect(struct desktop_state *desktop, struct framebuffer *fb, const struct rect *rect);
void desktop_draw_cursor_overlay(const struct desktop_state *desktop, struct framebuffer *fb);
void desktop_cursor_rect_at(const struct desktop_state *desktop, int x, int y, struct rect *rect);
int desktop_get_dirty_region(struct desktop_state *desktop, struct rect *rect);
void desktop_show_error(struct desktop_state *desktop, const char *title, const char *message);
uint64_t desktop_activity_units(void);

/* ---- Window server: called by the syscall layer for userspace GUI apps ---- */

/* Create (or reset) the app window with the given title and content size.
 * Returns a window id (>=0) or negative on error. */
int desktop_app_create(struct desktop_state *desktop, uint32_t pid, const char *title, int width, int height);
int desktop_app_create_ex(struct desktop_state *desktop, uint32_t pid, const struct winsys_window_options *options);

/* Copy a presented XRGB8888 pixel buffer (src, src_w x src_h) into the app
 * window content and mark it dirty. Called while the app's address space is
 * active so src is directly readable. Returns 0 on success. */
int desktop_app_present(struct desktop_state *desktop, uint32_t pid, int win_id, const uint32_t *src, int src_w, int src_h);
/* Present only a sub-rectangle (in content space) of the app's canvas. */
int desktop_app_present_rect(struct desktop_state *desktop, uint32_t pid, int win_id,
                             const uint32_t *src, int src_w, int src_h,
                             int dx, int dy, int dw, int dh);

/* Replace the desktop backdrop with a wallpaper image. src is an XRGB pixel
 * buffer (0x00RRGGBB) of src_w x src_h, scaled to fill the screen. Returns 0 on
 * success, <0 on error. */
int desktop_set_wallpaper(struct desktop_state *desktop, const uint32_t *src, int src_w, int src_h);

/* Dequeue one input event for the app window into *out. Returns 1 if an event
 * was returned, 0 if the queue is empty. */
int desktop_app_poll_event(struct desktop_state *desktop, uint32_t pid, int win_id, struct winsys_event *out);
void desktop_app_close_for_pid(struct desktop_state *desktop, uint32_t pid);
int desktop_shell_dock_active(const struct desktop_state *desktop);

/* Replace the WINDOW_APP context-menu entries with `count` items read from the
 * calling app's address space. Returns 0 on success, negative on error. */
int desktop_app_set_menu(struct desktop_state *desktop, uint32_t pid, int win_id, const struct winsys_menu_item *items, int count);
int desktop_app_set_menubar(struct desktop_state *desktop, uint32_t pid, int win_id, const struct winsys_menubar_item *items, int count);

/* Top-bar app support: snapshot status (load, uptime, focused menu bar) and
 * deliver a picked menu action to the focused window. */
void desktop_fill_status(struct desktop_state *desktop, struct winsys_desktop_status *out);
void desktop_dispatch_menu_action(struct desktop_state *desktop, uint32_t action_id);

#endif
