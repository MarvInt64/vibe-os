#ifndef VIBEOS_APP_H
#define VIBEOS_APP_H

#include "framebuffer.h"
#include "input.h"
#include "syscall.h"
#include "tty.h"
#include "types.h"
#include "winsys.h"

struct framebuffer;
struct desktop_state;
struct window_state;

struct app_draw_context {
    struct framebuffer *fb;
    const struct window_state *window;
    int window_width;
    int window_height;
    int focused;
    int content_x;
    int content_y;
    int content_width;
    int content_height;
    int text_scale;
    int line_step;
    int large_ui;
};

struct app_instance;

struct app_vtable {
    void (*activate)(struct app_instance *app);
    int (*handle_keyboard)(struct app_instance *app, const struct keyboard_state *keyboard);
    int (*needs_redraw)(struct app_instance *app);
    int (*consume_damage)(struct app_instance *app, const struct app_draw_context *ctx, struct rect *damage_rect);
    void (*draw)(const struct app_instance *app, const struct app_draw_context *ctx);

    /* ---- Optional hooks (may be NULL) ----
     * window_owner_pid: returns the pid whose death should close this app's
     *   window; 0 means "no owning process" (the window stays open).
     * window_closed: called after the compositor hides the window so the app
     *   can drop its per-session state.
     * menu_items: fills up to `max` app-declared context-menu entries and
     *   returns how many were written.
     * menu_action: the user picked one of those entries (by action_id). The
     *   app does its own work here; the compositor handles show/focus. */
    uint32_t (*window_owner_pid)(struct app_instance *app);
    void (*window_closed)(struct app_instance *app);
    int (*menu_items)(struct app_instance *app, struct winsys_menu_item *out, int max);
    void (*menu_action)(struct app_instance *app, uint32_t action_id);
};

struct text_app_state {
    const char *const *lines;
    size_t line_count;
};

struct terminal_app_state {
    struct tty tty;
    struct fd_table fd_table;
    struct syscall_context syscalls;
    uint32_t seen_tty_revision;
    size_t last_line_count;
    size_t last_partial_length;
    size_t last_input_length;
    uint8_t damage_full;
    uint8_t damage_input;
    uint32_t shell_pid;     /* pid of the hosted /bin/sh; 0 when none */
    uint8_t shell_running;
};

struct task_manager_app_state {
    const struct desktop_state *desktop;
    uint64_t seen_tick_bucket;
};

union app_state_storage {
    struct text_app_state text;
    struct terminal_app_state terminal;
    struct task_manager_app_state task_manager;
};

struct app_instance {
    const struct app_vtable *vtable;
    void *state;
};

void app_init_text(struct app_instance *app, struct text_app_state *state, const char *const *lines, size_t line_count);
void app_init_terminal(struct app_instance *app, struct terminal_app_state *state);
void app_init_task_manager(struct app_instance *app, struct task_manager_app_state *state, const struct desktop_state *desktop);
void app_activate(struct app_instance *app);
int app_handle_keyboard(struct app_instance *app, const struct keyboard_state *keyboard);
int app_needs_redraw(struct app_instance *app);
int app_consume_damage(struct app_instance *app, const struct app_draw_context *ctx, struct rect *damage_rect);
void app_draw(const struct app_instance *app, const struct app_draw_context *ctx);
uint32_t app_window_owner_pid(struct app_instance *app);
void app_window_closed(struct app_instance *app);
int app_menu_items(struct app_instance *app, struct winsys_menu_item *out, int max);
void app_menu_action(struct app_instance *app, uint32_t action_id);

#endif
