#include "app.h"
#include "process.h"
#include "render.h"
#include "timer.h"
#include "ui.h"
#include "window.h"

static const char *task_state_text(uint8_t state) {
    switch (state) {
        case PROCESS_STATE_READY:
            return "READY";
        case PROCESS_STATE_RUNNING:
            return "RUNNING";
        case PROCESS_STATE_SLEEPING:
            return "SLEEP";
        case PROCESS_STATE_WAITING:
            return "WAIT";
        case PROCESS_STATE_WAITING_IO:
            return "IOWAIT";
        case PROCESS_STATE_EXITED:
            return "EXIT";
        default:
            return "EMPTY";
    }
}

static uint32_t task_state_fill(uint8_t state) {
    switch (state) {
        case PROCESS_STATE_READY:
            return 0x0027442fu;
        case PROCESS_STATE_RUNNING:
            return 0x00243d52u;
        case PROCESS_STATE_SLEEPING:
            return 0x00443720u;
        case PROCESS_STATE_WAITING:
            return 0x0032324au;
        case PROCESS_STATE_WAITING_IO:
            return 0x0029333fu;
        case PROCESS_STATE_EXITED:
            return 0x00473834u;
        default:
            return 0x00212731u;
    }
}

static uint32_t task_state_border(uint8_t state) {
    switch (state) {
        case PROCESS_STATE_READY:
            return 0x0064d298u;
        case PROCESS_STATE_RUNNING:
            return 0x006bcdf0u;
        case PROCESS_STATE_SLEEPING:
            return 0x00f0b86eu;
        case PROCESS_STATE_WAITING:
            return 0x0096a9f8u;
        case PROCESS_STATE_WAITING_IO:
            return 0x0094b4c9u;
        case PROCESS_STATE_EXITED:
            return 0x00d28c78u;
        default:
            return 0x00596c7au;
    }
}

static int task_manager_handle_keyboard(struct app_instance *app, const struct keyboard_state *keyboard) {
    (void)app;
    (void)keyboard;
    return 0;
}

static int task_manager_needs_redraw(struct app_instance *app) {
    struct task_manager_app_state *state = (struct task_manager_app_state *)app->state;
    uint64_t tick_bucket = timer_tick_count() / 20u;

    if (state->seen_tick_bucket == tick_bucket) {
        return 0;
    }

    state->seen_tick_bucket = tick_bucket;
    return 1;
}

static int task_manager_consume_damage(struct app_instance *app, const struct app_draw_context *ctx, struct rect *damage_rect) {
    (void)app;
    damage_rect->x = 0;
    damage_rect->y = 0;
    damage_rect->width = ctx->window->width;
    damage_rect->height = ctx->window->height;
    return 1;
}

static void draw_summary_card(const struct app_draw_context *ctx, const struct ui_metrics *ui, const struct rect *rect, const char *title, const char *value, const char *caption, uint32_t accent) {
    int title_y = rect->y + ui->inset + 2;
    int value_y = title_y + (ctx->text_scale * 11);
    int caption_y = value_y + ((ctx->text_scale + 1) * 10);

    ui_draw_card(ctx->fb, rect, 0x00111924u, 0x002b3b50u, accent);
    draw_text(ctx->fb, rect->x + ui->inset, title_y, title, 0x008eb5c9u, ctx->text_scale);
    draw_text(ctx->fb, rect->x + ui->inset, value_y, value, 0x00f2fbffu, ctx->text_scale + 1);
    draw_text(ctx->fb, rect->x + ui->inset, caption_y, caption, 0x007b98aau, ctx->text_scale);
}

static void build_screen_value(char *buffer, size_t capacity, const struct desktop_state *desktop) {
    size_t offset = ui_text_append_uint(buffer, capacity, 0, desktop->screen_width);
    offset = ui_text_append(buffer, capacity, offset, " X ");
    (void)ui_text_append_uint(buffer, capacity, offset, desktop->screen_height);
}

static void build_ticks_value(char *buffer, size_t capacity) {
    size_t offset = ui_text_append_uint(buffer, capacity, 0, (uint32_t)timer_tick_count());
    offset = ui_text_append(buffer, capacity, offset, " @ ");
    offset = ui_text_append_uint(buffer, capacity, offset, timer_frequency_hz());
    (void)ui_text_append(buffer, capacity, offset, " HZ");
}

static void build_pointer_value(char *buffer, size_t capacity, const struct desktop_state *desktop) {
    size_t offset = ui_text_append_uint(buffer, capacity, 0, (uint32_t)desktop->mouse_x);
    offset = ui_text_append(buffer, capacity, offset, ", ");
    (void)ui_text_append_uint(buffer, capacity, offset, (uint32_t)desktop->mouse_y);
}

static void build_process_mix_value(char *buffer, size_t capacity) {
    size_t offset = ui_text_append_uint(buffer, capacity, 0, process_loaded_count());
    offset = ui_text_append(buffer, capacity, offset, " TOTAL ");
    offset = ui_text_append_uint(buffer, capacity, offset, process_ready_count());
    offset = ui_text_append(buffer, capacity, offset, " READY ");
    offset = ui_text_append_uint(buffer, capacity, offset, process_running_count());
    offset = ui_text_append(buffer, capacity, offset, " RUN ");
    offset = ui_text_append_uint(buffer, capacity, offset, process_sleeping_count());
    (void)ui_text_append(buffer, capacity, offset, " SLP");
}

static void draw_window_panel(const struct app_draw_context *ctx, const struct ui_metrics *ui, const struct rect *rect, const struct desktop_state *desktop) {
    size_t i;
    int y = rect->y + ui->inset + (ctx->text_scale * 10);

    ui_draw_card(ctx->fb, rect, 0x00101720u, 0x002a3950u, 0x00854df2u);
    draw_text(ctx->fb, rect->x + ui->inset, rect->y + ui->inset, "WINDOWS", 0x00eef8ffu, ctx->text_scale);

    for (i = 0; i < WINDOW_COUNT; ++i) {
        const struct window_state *window = &desktop->windows[i];
        int badge_x;

        if (y + ui->row_height > rect->y + rect->height - ui->inset) {
            break;
        }

        draw_text(ctx->fb, rect->x + ui->inset, y, window->title, 0x00dfeef9u, ctx->text_scale);
        badge_x = rect->x + rect->width - ui->inset - ui_text_width(window->visible ? "VISIBLE" : "HIDDEN", ctx->text_scale) - (ctx->text_scale * 6);
        ui_draw_badge(
            ctx->fb,
            badge_x,
            y - 1,
            window->visible ? "VISIBLE" : "HIDDEN",
            window->visible ? 0x0027442fu : 0x00473834u,
            window->visible ? 0x0064d298u : 0x00d28c78u,
            0x00eef8ffu,
            ctx->text_scale
        );

        if ((int)i == desktop->focused_window) {
            ui_draw_badge(ctx->fb, rect->x + rect->width - ui->inset - 132, y - 1, "FOCUS", 0x00243d52u, 0x006bcdf0u, 0x00eef8ffu, ctx->text_scale);
        }

        y += ui->row_height + 4;
    }
}

static void draw_process_panel(const struct app_draw_context *ctx, const struct ui_metrics *ui, const struct rect *rect) {
    uint32_t i;
    int y = rect->y + ui->inset + (ctx->text_scale * 10);

    ui_draw_card(ctx->fb, rect, 0x00101720u, 0x002a3950u, 0x0076d3c6u);
    draw_text(ctx->fb, rect->x + ui->inset, rect->y + ui->inset, "TASKS", 0x00eef8ffu, ctx->text_scale);

    for (i = 0; i < process_snapshot_count(); ++i) {
        struct process_snapshot snapshot;
        char left[48];
        char right[80];
        size_t offset;

        if (!process_get_snapshot(i, &snapshot) || !snapshot.loaded) {
            continue;
        }

        if (y + ui->row_height > rect->y + rect->height - ui->inset) {
            break;
        }

        left[0] = '\0';
        right[0] = '\0';
        offset = ui_text_append(left, sizeof(left), 0, "PID ");
        offset = ui_text_append_uint(left, sizeof(left), offset, snapshot.pid);
        offset = ui_text_append(left, sizeof(left), offset, "  T");
        offset = ui_text_append_uint(left, sizeof(left), offset, (uint32_t)snapshot.runtime_ticks);

        offset = ui_text_append(right, sizeof(right), 0, "SW ");
        offset = ui_text_append_uint(right, sizeof(right), offset, (uint32_t)snapshot.switch_count);
        offset = ui_text_append(right, sizeof(right), offset, "  PR ");
        offset = ui_text_append_uint(right, sizeof(right), offset, (uint32_t)snapshot.preempt_count);
        if (snapshot.state == PROCESS_STATE_SLEEPING) {
            offset = ui_text_append(right, sizeof(right), offset, "  WAKE ");
            (void)ui_text_append_uint(right, sizeof(right), offset, (uint32_t)snapshot.wake_tick);
        }

        draw_text(ctx->fb, rect->x + ui->inset, y, left, 0x00dfeef9u, ctx->text_scale);
        ui_draw_badge(
            ctx->fb,
            rect->x + rect->width - ui->inset - ui_text_width(task_state_text(snapshot.state), ctx->text_scale) - (ctx->text_scale * 6),
            y - 1,
            task_state_text(snapshot.state),
            task_state_fill(snapshot.state),
            task_state_border(snapshot.state),
            0x00eef8ffu,
            ctx->text_scale
        );
        draw_text(ctx->fb, rect->x + ui->inset, y + ui->row_height - 2, right, 0x009fc1d7u, ctx->text_scale);
        y += (ui->row_height * 2);
    }
}

static void task_manager_draw(const struct app_instance *app, const struct app_draw_context *ctx) {
    const struct task_manager_app_state *state = (const struct task_manager_app_state *)app->state;
    const struct desktop_state *desktop = state->desktop;
    struct ui_metrics ui;
    struct rect header_rect;
    struct rect summary_rects[4];
    struct rect windows_rect;
    struct rect processes_rect;
    char buffer[96];
    int content_bottom;
    int summary_width;
    int header_height;
    int summary_y;
    int summary_height;
    int lower_y;
    int lower_height;

    ui_metrics_from_draw_context(ctx, &ui);
    fb_fill_rect(ctx->fb, ctx->content_x, ctx->content_y, ctx->content_width, ctx->content_height, 0x000d131au);

    header_height = (ctx->text_scale * 24) + (ui.inset * 2);
    header_rect.x = ctx->content_x + ui.gap;
    header_rect.y = ctx->content_y + ui.gap;
    header_rect.width = ctx->content_width - (ui.gap * 2);
    header_rect.height = header_height;
    ui_draw_header(ctx->fb, &header_rect, "TASK MANAGER", "LIVE SCHEDULER, WINDOWS AND INPUT STATE", &ui);

    summary_y = header_rect.y + header_rect.height + ui.gap;
    summary_height = (ctx->text_scale * 32) + (ui.inset * 2);
    summary_width = (ctx->content_width - (ui.gap * 5)) / 4;
    for (content_bottom = 0; content_bottom < 4; ++content_bottom) {
        summary_rects[content_bottom].x = ctx->content_x + ui.gap + (content_bottom * (summary_width + ui.gap));
        summary_rects[content_bottom].y = summary_y;
        summary_rects[content_bottom].width = summary_width;
        summary_rects[content_bottom].height = summary_height;
    }

    build_screen_value(buffer, sizeof(buffer), desktop);
    draw_summary_card(ctx, &ui, &summary_rects[0], "DISPLAY", buffer, "ACTIVE FRAMEBUFFER", 0x006bcdf0u);

    build_ticks_value(buffer, sizeof(buffer));
    draw_summary_card(ctx, &ui, &summary_rects[1], "TIMER", buffer, "PREEMPTIVE TICK SOURCE", 0x0076d3c6u);

    build_pointer_value(buffer, sizeof(buffer), desktop);
    draw_summary_card(ctx, &ui, &summary_rects[2], "POINTER", buffer, "CURRENT CURSOR POSITION", 0x00f0b86eu);

    build_process_mix_value(buffer, sizeof(buffer));
    draw_summary_card(ctx, &ui, &summary_rects[3], "PROCESSES", buffer, "READY, RUNNING AND SLEEPING", 0x0064d298u);

    lower_y = summary_y + summary_height + ui.gap;
    content_bottom = ctx->content_y + ctx->content_height - ui.gap;
    lower_height = content_bottom - lower_y;

    windows_rect.x = ctx->content_x + ui.gap;
    windows_rect.y = lower_y;
    windows_rect.width = (ctx->content_width - (ui.gap * 3)) / 3;
    windows_rect.height = lower_height;

    processes_rect.x = windows_rect.x + windows_rect.width + ui.gap;
    processes_rect.y = lower_y;
    processes_rect.width = ctx->content_x + ctx->content_width - ui.gap - processes_rect.x;
    processes_rect.height = lower_height;

    draw_window_panel(ctx, &ui, &windows_rect, desktop);
    draw_process_panel(ctx, &ui, &processes_rect);

    {
        struct rect footer_rect;

        footer_rect.x = ctx->content_x + ui.gap;
        footer_rect.y = ctx->content_y + ctx->content_height - ((ctx->text_scale * 12) + ui.gap + 8);
        footer_rect.width = ctx->content_width - (ui.gap * 2);
        footer_rect.height = (ctx->text_scale * 10) + 10;
        ui_draw_key_value(ctx->fb, &footer_rect, "FOCUSED WINDOW", desktop->focused_window >= 0 ? desktop->windows[desktop->focused_window].title : "NONE", &ui, 0x008eb5c9u, 0x00eef8ffu);
    }
}

static const struct app_vtable TASK_MANAGER_APP_VTABLE = {
    .activate = 0,
    .handle_keyboard = task_manager_handle_keyboard,
    .needs_redraw = task_manager_needs_redraw,
    .consume_damage = task_manager_consume_damage,
    .draw = task_manager_draw
};

void app_init_task_manager(struct app_instance *app, struct task_manager_app_state *state, const struct desktop_state *desktop) {
    state->desktop = desktop;
    state->seen_tick_bucket = 0;
    app->vtable = &TASK_MANAGER_APP_VTABLE;
    app->state = state;
}
