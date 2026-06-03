#ifndef VIBEOS_WINSYS_H
#define VIBEOS_WINSYS_H

#include "types.h"

/* Shared ABI between the kernel window server and userspace GUI apps.
 * Userspace apps create a window, render into a pixel buffer (XRGB8888) and
 * present it; the compositor draws the frame and blits the content. Apps get
 * input through an event queue. This header is the *only* contract an external
 * app needs — no kernel internals required. */

/* Max app window size. WIDTH spans the full screen so the top-bar app can be a
 * single full-width window (the per-slot content/surface buffers scale with
 * this). */
#define WINSYS_MAX_WIDTH  1920
#define WINSYS_MAX_HEIGHT 640

#define WINSYS_WINDOW_FRAMELESS     0x00000001u
#define WINSYS_WINDOW_NO_DOCK       0x00000002u
#define WINSYS_WINDOW_POSITIONED    0x00000004u
#define WINSYS_WINDOW_ALWAYS_ON_TOP 0x00000008u
#define WINSYS_WINDOW_TRANSLUCENT   0x00000010u
/* Do not draw a drop shadow beneath this window (useful for shell panels). */
#define WINSYS_WINDOW_NO_SHADOW     0x00000020u

struct winsys_window_options {
    const char *title;
    int32_t width;
    int32_t height;
    uint32_t flags;
    int32_t x;
    int32_t y;
    /* Pixels to inset the drop shadow from the top of the window.  Use this
     * when the window has a transparent header zone (e.g. tooltip headroom)
     * so the shadow only appears below the visible content, not the empty
     * upper zone.  0 = shadow starts at the window top (default). */
    int32_t shadow_inset_top;
};

/* Event types delivered to the focused window. */
enum winsys_event_type {
    WINSYS_EVENT_NONE = 0,
    WINSYS_EVENT_MOUSE_MOVE = 1,
    WINSYS_EVENT_MOUSE_DOWN = 2,
    WINSYS_EVENT_MOUSE_UP = 3,
    WINSYS_EVENT_KEY = 4,
    WINSYS_EVENT_CLOSE = 5,
    WINSYS_EVENT_CONTEXT_MENU = 6,
    /* The user picked one of the app-declared dock context-menu entries. The
     * chosen entry's action_id is delivered in the event's `key` field. */
    WINSYS_EVENT_MENU_ACTION = 7,
    /* Scroll-wheel/touchpad: signed notch delta delivered in the `y` field
     * (positive = wheel forward/away from the user). */
    WINSYS_EVENT_SCROLL = 8,
    /* Window content area changed size. New width/height are in x/y. */
    WINSYS_EVENT_RESIZE = 9
};

/* One input event. Mouse coordinates are relative to the window content area. */
struct winsys_event {
    uint32_t type;
    int32_t x;
    int32_t y;
    uint32_t buttons; /* bit0 = left, bit1 = right */
    uint32_t key;     /* ASCII for WINSYS_EVENT_KEY; action_id for MENU_ACTION */
};

/* ---- App-declared context-menu entries ----
 * An app describes the extra entries it wants in its dock icon's context menu.
 * The window server shows them below the standard Show/Hide rows and reports
 * the picked entry's action_id back to the app (kernel apps via a vtable hook,
 * userspace apps via a WINSYS_EVENT_MENU_ACTION event). This is the entire
 * contract — apps never touch the compositor's menu drawing code. */
#define WINSYS_MENU_LABEL_MAX 24
#define WINSYS_MAX_MENU_ITEMS 6

struct winsys_menu_item {
    char label[WINSYS_MENU_LABEL_MAX];
    uint32_t action_id;
};

/* ---- App-declared top-bar menu bar ----
 * The focused window supplies the global menu bar shown in the top bar. The
 * bar is a flat list: an item with WINSYS_MB_TITLE starts a new top-level menu
 * (Page, Edit, …); the items after it (until the next title) are that menu's
 * entries. Picking an entry sends a WINSYS_EVENT_MENU_ACTION with its
 * action_id back to the app. The compositor owns all drawing/interaction. */
#define WINSYS_MENUBAR_LABEL_MAX    28
#define WINSYS_MENUBAR_SHORTCUT_MAX 16
#define WINSYS_MAX_MENUBAR_ITEMS    64

enum {
    WINSYS_MB_TITLE   = 1u,   /* starts a new top-level menu */
    WINSYS_MB_DIVIDER = 2u,   /* horizontal separator (label ignored) */
    WINSYS_MB_DANGER  = 4u,   /* destructive item (subtle red) */
    WINSYS_MB_CHECK   = 8u,   /* checkbox item */
    WINSYS_MB_CHECKED = 16u,  /* checkbox is currently on */
    WINSYS_MB_ARROW   = 32u   /* opens a submenu (arrow shown) */
};

struct winsys_menubar_item {
    char label[WINSYS_MENUBAR_LABEL_MAX];
    char shortcut[WINSYS_MENUBAR_SHORTCUT_MAX];
    uint32_t flags;
    uint32_t action_id;
};

/* ---- Desktop status for the userspace top-bar app ----
 * Snapshot the top bar needs each frame: system load, uptime, the focused
 * window's title and its declared menu bar. Filled by SYS_DESKTOP_STATUS. */
#define WINSYS_APP_LABEL_MAX 20

struct winsys_desktop_status {
    uint32_t uptime_seconds;
    uint32_t cpu_pct;
    uint32_t ui_pct;
    uint32_t mem_pct;
    uint32_t net_up;                 /* 1 if the network has an IP */
    char     app_label[WINSYS_APP_LABEL_MAX];   /* focused window title */
    uint32_t menu_count;             /* valid entries in menu[] */
    struct winsys_menubar_item menu[WINSYS_MAX_MENUBAR_ITEMS];
};

#endif
