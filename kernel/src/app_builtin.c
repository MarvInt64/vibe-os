/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Marvin Kicha <https://github.com/MarvInt64/> */

#include "app.h"
#include "render.h"

static int text_app_handle_keyboard(struct app_instance *app, const struct keyboard_state *keyboard) {
    (void)app;
    (void)keyboard;
    return 0;
}

static int text_app_needs_redraw(struct app_instance *app) {
    (void)app;
    return 0;
}

static int text_app_consume_damage(struct app_instance *app, const struct app_draw_context *ctx, struct rect *damage_rect) {
    (void)app;
    (void)ctx;
    (void)damage_rect;
    return 0;
}

static void text_app_draw(const struct app_instance *app, const struct app_draw_context *ctx) {
    const struct text_app_state *state = (const struct text_app_state *)app->state;
    int y = ctx->content_y;
    size_t i;

    for (i = 0; i < state->line_count; ++i) {
        uint32_t color = i == 0 ? 0x00eef3fbu : 0x009fb0c6u;
        draw_text(ctx->fb, ctx->content_x, y, state->lines[i], color, ctx->text_scale);
        y += ctx->line_step;
    }
}

static const struct app_vtable TEXT_APP_VTABLE = {
    .activate = 0,
    .handle_keyboard = text_app_handle_keyboard,
    .needs_redraw = text_app_needs_redraw,
    .consume_damage = text_app_consume_damage,
    .draw = text_app_draw
};

void app_init_text(struct app_instance *app, struct text_app_state *state, const char *const *lines, size_t line_count) {
    state->lines = lines;
    state->line_count = line_count;
    app->vtable = &TEXT_APP_VTABLE;
    app->state = state;
}

void app_activate(struct app_instance *app) {
    if (app != 0 && app->vtable != 0 && app->vtable->activate != 0) {
        app->vtable->activate(app);
    }
}

int app_handle_keyboard(struct app_instance *app, const struct keyboard_state *keyboard) {
    if (app == 0 || app->vtable == 0 || app->vtable->handle_keyboard == 0) {
        return 0;
    }

    return app->vtable->handle_keyboard(app, keyboard);
}

int app_needs_redraw(struct app_instance *app) {
    if (app == 0 || app->vtable == 0 || app->vtable->needs_redraw == 0) {
        return 0;
    }

    return app->vtable->needs_redraw(app);
}

int app_consume_damage(struct app_instance *app, const struct app_draw_context *ctx, struct rect *damage_rect) {
    if (app == 0 || app->vtable == 0 || app->vtable->consume_damage == 0) {
        return 0;
    }

    return app->vtable->consume_damage(app, ctx, damage_rect);
}

void app_draw(const struct app_instance *app, const struct app_draw_context *ctx) {
    if (app == 0 || app->vtable == 0 || app->vtable->draw == 0) {
        return;
    }

    app->vtable->draw(app, ctx);
}

uint32_t app_window_owner_pid(struct app_instance *app) {
    if (app == 0 || app->vtable == 0 || app->vtable->window_owner_pid == 0) {
        return 0;
    }

    return app->vtable->window_owner_pid(app);
}

void app_window_closed(struct app_instance *app) {
    if (app != 0 && app->vtable != 0 && app->vtable->window_closed != 0) {
        app->vtable->window_closed(app);
    }
}

int app_menu_items(struct app_instance *app, struct winsys_menu_item *out, int max) {
    if (app == 0 || app->vtable == 0 || app->vtable->menu_items == 0 || out == 0 || max <= 0) {
        return 0;
    }

    return app->vtable->menu_items(app, out, max);
}

void app_menu_action(struct app_instance *app, uint32_t action_id) {
    if (app != 0 && app->vtable != 0 && app->vtable->menu_action != 0) {
        app->vtable->menu_action(app, action_id);
    }
}
