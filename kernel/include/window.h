#ifndef VIBEOS_WINDOW_H
#define VIBEOS_WINDOW_H

#include "app.h"
#include "framebuffer.h"
#include "input.h"
#include "winsys.h"

enum window_id {
    WINDOW_INFO = 0,
    WINDOW_FILES = 1,
    WINDOW_TERMINAL = 2,
    WINDOW_TASK_MANAGER = 3,
    /* Slot for a userspace app window (content presented via the window
     * server syscalls). Only the four windows above get desktop icons. */
    WINDOW_APP = 4,
    WINDOW_COUNT = 5
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
    uint8_t app_slot;
    uint8_t visible;
    uint8_t maximized;
    int restore_x;
    int restore_y;
    int restore_width;
    int restore_height;
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
    struct rect dirty_rect;
    struct rect window_dirty_rects[WINDOW_COUNT];
    uint32_t background_storage[DESKTOP_MAX_WIDTH * DESKTOP_MAX_HEIGHT];
    uint32_t info_surface_storage[WINDOW_INFO_MAX_WIDTH * WINDOW_INFO_MAX_HEIGHT];
    uint32_t files_surface_storage[WINDOW_FILES_MAX_WIDTH * WINDOW_FILES_MAX_HEIGHT];
    uint32_t terminal_surface_storage[WINDOW_TERMINAL_MAX_WIDTH * WINDOW_TERMINAL_MAX_HEIGHT];
    uint32_t tasks_surface_storage[WINDOW_TASKS_MAX_WIDTH * WINDOW_TASKS_MAX_HEIGHT];

    /* Userspace app window (WINDOW_APP): frame buffer + last-presented content
     * + an input event queue the app drains via SYS_EVENT_POLL. */
    uint32_t app_surface_storage[WINDOW_APP_SURFACE_MAX_WIDTH * WINDOW_APP_SURFACE_MAX_HEIGHT];
    uint32_t app_content_storage[WINDOW_APP_CONTENT_MAX_WIDTH * WINDOW_APP_CONTENT_MAX_HEIGHT];
    int app_content_width;
    int app_content_height;
    uint8_t app_created;
    uint32_t app_pid;
    char app_title[64];
    struct winsys_event app_events[WINSYS_EVENT_QUEUE];
    int app_event_head;
    int app_event_tail;
    uint8_t context_menu_open;
    int context_menu_x;
    int context_menu_y;
    int context_menu_window;

    /* Context-menu entries declared by the userspace app in WINDOW_APP (set via
     * SYS_WINDOW_SET_MENU). Kernel apps declare theirs through their vtable. */
    struct winsys_menu_item app_menu_items[WINSYS_MAX_MENU_ITEMS];
    int app_menu_count;

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
int desktop_take_dirty_rect(struct desktop_state *desktop, struct rect *rect);

/* ---- Window server: called by the syscall layer for userspace GUI apps ---- */

/* Create (or reset) the app window with the given title and content size.
 * Returns a window id (>=0) or negative on error. */
int desktop_app_create(struct desktop_state *desktop, uint32_t pid, const char *title, int width, int height);

/* Copy a presented XRGB8888 pixel buffer (src, src_w x src_h) into the app
 * window content and mark it dirty. Called while the app's address space is
 * active so src is directly readable. Returns 0 on success. */
int desktop_app_present(struct desktop_state *desktop, uint32_t pid, int win_id, const uint32_t *src, int src_w, int src_h);

/* Dequeue one input event for the app window into *out. Returns 1 if an event
 * was returned, 0 if the queue is empty. */
int desktop_app_poll_event(struct desktop_state *desktop, uint32_t pid, int win_id, struct winsys_event *out);
void desktop_app_close_for_pid(struct desktop_state *desktop, uint32_t pid);

/* Replace the WINDOW_APP context-menu entries with `count` items read from the
 * calling app's address space. Returns 0 on success, negative on error. */
int desktop_app_set_menu(struct desktop_state *desktop, uint32_t pid, int win_id, const struct winsys_menu_item *items, int count);

#endif
