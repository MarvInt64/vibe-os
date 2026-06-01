#ifndef VIBEOS_WINSYS_H
#define VIBEOS_WINSYS_H

#include "types.h"

/* Shared ABI between the kernel window server and userspace GUI apps.
 * Userspace apps create a window, render into a pixel buffer (XRGB8888) and
 * present it; the compositor draws the frame and blits the content. Apps get
 * input through an event queue. This header is the *only* contract an external
 * app needs — no kernel internals required. */

#define WINSYS_MAX_WIDTH  900
#define WINSYS_MAX_HEIGHT 640

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

#endif
