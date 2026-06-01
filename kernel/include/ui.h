#ifndef VIBEOS_UI_H
#define VIBEOS_UI_H

#include "app.h"
#include "framebuffer.h"
#include "types.h"

struct ui_metrics {
    int inset;
    int gap;
    int row_height;
    int title_scale;
};

size_t ui_text_copy(char *dest, size_t capacity, const char *src);
size_t ui_text_append(char *dest, size_t capacity, size_t offset, const char *src);
size_t ui_text_append_uint(char *dest, size_t capacity, size_t offset, uint32_t value);
int ui_text_width(const char *text, int scale);
void ui_metrics_from_draw_context(const struct app_draw_context *ctx, struct ui_metrics *metrics);
void ui_draw_card(struct framebuffer *fb, const struct rect *rect, uint32_t fill, uint32_t border, uint32_t accent);
void ui_draw_header(struct framebuffer *fb, const struct rect *rect, const char *title, const char *subtitle, const struct ui_metrics *metrics);
void ui_draw_key_value(struct framebuffer *fb, const struct rect *rect, const char *label, const char *value, const struct ui_metrics *metrics, uint32_t label_color, uint32_t value_color);
void ui_draw_badge(struct framebuffer *fb, int x, int y, const char *text, uint32_t fill, uint32_t border, uint32_t text_color, int scale);

#endif
