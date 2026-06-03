/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Marvin Kicha <https://github.com/MarvInt64/> */

#include "ui.h"
#include "render.h"

size_t ui_text_copy(char *dest, size_t capacity, const char *src) {
    size_t count = 0;

    if (capacity == 0) {
        return 0;
    }

    while (src[count] != '\0' && count + 1 < capacity) {
        dest[count] = src[count];
        ++count;
    }

    dest[count] = '\0';
    return count;
}

size_t ui_text_append(char *dest, size_t capacity, size_t offset, const char *src) {
    if (offset >= capacity) {
        return offset;
    }

    return offset + ui_text_copy(dest + offset, capacity - offset, src);
}

size_t ui_text_append_uint(char *dest, size_t capacity, size_t offset, uint32_t value) {
    char digits[10];
    size_t digit_count = 0;
    size_t i;

    if (offset >= capacity) {
        return offset;
    }

    if (value == 0) {
        if (offset + 1 < capacity) {
            dest[offset++] = '0';
            dest[offset] = '\0';
        }
        return offset;
    }

    while (value > 0 && digit_count < sizeof(digits)) {
        digits[digit_count++] = (char)('0' + (value % 10u));
        value /= 10u;
    }

    for (i = digit_count; i > 0; --i) {
        if (offset + 1 >= capacity) {
            break;
        }
        dest[offset++] = digits[i - 1];
    }

    dest[offset] = '\0';
    return offset;
}

int ui_text_width(const char *text, int scale) {
    int count = 0;

    if (text == 0 || scale <= 0) {
        return 0;
    }

    while (*text != '\0') {
        ++count;
        ++text;
    }

    return count * text_char_advance(scale);
}

void ui_metrics_from_draw_context(const struct app_draw_context *ctx, struct ui_metrics *metrics) {
    metrics->inset = ctx->large_ui ? 12 : 8;
    metrics->gap = ctx->large_ui ? 10 : 6;
    metrics->row_height = ctx->line_step;
    metrics->title_scale = ctx->text_scale + 1;
}

void ui_draw_card(struct framebuffer *fb, const struct rect *rect, uint32_t fill, uint32_t border, uint32_t accent) {
    draw_soft_shadow(fb, rect->x, rect->y, rect->width, rect->height, 12, 5, 0x00071018u);
    draw_rounded_panel(fb, rect->x, rect->y, rect->width, rect->height, 12, fill, 0x000b1118u, border, 0x00ffffffu);
    draw_rounded_panel(fb, rect->x + 10, rect->y + 10, rect->width - 20, 6, 3, accent, accent, accent, 0x00ffffffu);
}

void ui_draw_header(struct framebuffer *fb, const struct rect *rect, const char *title, const char *subtitle, const struct ui_metrics *metrics) {
    int text_x = rect->x + metrics->inset;
    int title_y = rect->y + metrics->inset + 2;
    int subtitle_y = title_y + (metrics->title_scale * 10);

    ui_draw_card(fb, rect, 0x00141d28u, 0x00324658u, 0x0076d3c6u);
    draw_text(fb, text_x, title_y, title, 0x00eef8ffu, metrics->title_scale);
    draw_text(fb, text_x, subtitle_y, subtitle, 0x009fc1d7u, metrics->title_scale > 1 ? metrics->title_scale - 1 : 1);
}

void ui_draw_key_value(struct framebuffer *fb, const struct rect *rect, const char *label, const char *value, const struct ui_metrics *metrics, uint32_t label_color, uint32_t value_color) {
    int text_y = rect->y + metrics->inset + 4;
    int value_x = rect->x + rect->width - metrics->inset - ui_text_width(value, 1);

    ui_draw_card(fb, rect, 0x00101822u, 0x00293a4eu, 0x0047b5c9u);
    draw_text(fb, rect->x + metrics->inset, text_y, label, label_color, 1);
    draw_text(fb, value_x, text_y, value, value_color, 1);
}

void ui_draw_badge(struct framebuffer *fb, int x, int y, const char *text, uint32_t fill, uint32_t border, uint32_t text_color, int scale) {
    int width = ui_text_width(text, scale) + (scale * 6);
    int height = (scale * 9) + (scale * 2);

    draw_rounded_panel(fb, x, y, width, height, height / 2, fill, 0x000e151du, border, 0x00ffffffu);
    draw_text(fb, x + (scale * 3), y + scale, text, text_color, scale);
}
