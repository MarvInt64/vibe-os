#include "window.h"
#include "process.h"
#include "render.h"
#include "serial.h"
#include "string.h"
#include "types.h"
#include "vfs.h"

struct desktop_icon {
    int x;
    int y;
    uint32_t color;
    const char *label;
    int window_index;
    int launcher_index;
};

#define WINDOW_SHADOW_SPREAD 3

static struct desktop_icon launcher_icon_at(const struct desktop_state *desktop, int index);

/* Pixels left at this colour in a window surface are treated as transparent
 * when the surface is composited, so the rounded corners show the desktop
 * behind them instead of an ugly black block. */
#define WINDOW_TRANSPARENT_KEY 0x00ff00ffu

static const char *const INFO_LINES[] = {
    "MOUSE READY",
    "KEYBOARD READY",
    "CLICK TASKS ICON",
    "DRAG WINDOWS"
};

static const char *const FILES_LINES[] = {
    "BOOT",
    "KERNEL",
    "APPS",
    "TERM"
};

static int clamp_value(int value, int min, int max) {
    if (value < min) {
        return min;
    }
    if (value > max) {
        return max;
    }
    return value;
}

static uint32_t mix_color(uint32_t a, uint32_t b, uint32_t step, uint32_t total) {
    uint32_t ar = (a >> 16) & 0xffu;
    uint32_t ag = (a >> 8) & 0xffu;
    uint32_t ab = a & 0xffu;
    uint32_t br = (b >> 16) & 0xffu;
    uint32_t bg = (b >> 8) & 0xffu;
    uint32_t bb = b & 0xffu;

    return ((((ar * (total - step)) + (br * step)) / total) << 16) |
           ((((ag * (total - step)) + (bg * step)) / total) << 8) |
           (((ab * (total - step)) + (bb * step)) / total);
}

static int large_ui(const struct desktop_state *desktop) {
	return desktop->screen_width >= 1920u || desktop->screen_height >= 1080u;
}

static int ui_titlebar_height(const struct desktop_state *desktop) {
    return large_ui(desktop) ? 38 : 30;
}

/* Vertical space reserved at the bottom for the macOS-style dock. */
static int ui_taskbar_height(const struct desktop_state *desktop) {
    return large_ui(desktop) ? 104 : 80;
}

/* Small left margin (the dock is now at the bottom, not on the left). */
static int ui_dock_width(const struct desktop_state *desktop) {
    (void)desktop;
    return 12;
}

static int ui_icon_box_width(const struct desktop_state *desktop) {
    return large_ui(desktop) ? 50 : 38;
}

static int ui_icon_box_height(const struct desktop_state *desktop) {
    return large_ui(desktop) ? 38 : 30;
}

static int ui_icon_label_scale(const struct desktop_state *desktop) {
    /* 8x16 font: scale 1 == 16px, a normal crisp UI size (was a huge 3x). */
    return large_ui(desktop) ? 1 : 1;
}

static int ui_window_text_scale(const struct desktop_state *desktop) {
    return large_ui(desktop) ? 1 : 1;
}

static int ui_terminal_text_scale(const struct desktop_state *desktop) {
    return large_ui(desktop) ? 1 : 1;
}

static int ui_terminal_line_step(const struct desktop_state *desktop) {
    return text_line_height(ui_terminal_text_scale(desktop)) + (large_ui(desktop) ? 2 : 1);
}

static void copy_text(char *dst, size_t cap, const char *src) {
    size_t i;
    if (dst == 0 || cap == 0) return;
    for (i = 0; i + 1 < cap && src && src[i]; ++i) dst[i] = src[i];
    dst[i] = '\0';
}

static void append_text(char *dst, size_t cap, const char *src) {
    size_t len = string_length(dst);
    size_t i = 0;
    if (len >= cap) return;
    while (len + 1 < cap && src && src[i]) dst[len++] = src[i++];
    dst[len] = '\0';
}

static int text_ends_with(const char *s, const char *suffix) {
    size_t sl = string_length(s);
    size_t xl = string_length(suffix);
    if (xl > sl) return 0;
    return strcmp(s + sl - xl, suffix) == 0;
}

static int line_value(const char *text, const char *key, char *out, size_t out_cap) {
    size_t key_len = string_length(key);
    size_t i = 0;
    if (out_cap > 0) out[0] = '\0';
    while (text && text[i]) {
        size_t line = i;
        size_t k;
        int match = 1;
        while (text[i] && text[i] != '\n' && text[i] != '\r') ++i;
        for (k = 0; k < key_len; ++k) {
            if (line + k >= i || text[line + k] != key[k]) { match = 0; break; }
        }
        if (match && line + key_len < i && text[line + key_len] == '=') {
            size_t p = line + key_len + 1;
            size_t o = 0;
            while (p < i && o + 1 < out_cap) out[o++] = text[p++];
            if (out_cap > 0) out[o] = '\0';
            return o > 0;
        }
        while (text[i] == '\n' || text[i] == '\r') ++i;
    }
    return 0;
}

static uint32_t parse_hex_color(const char *s, uint32_t fallback) {
    uint32_t v = 0;
    int i = 0;
    if (s == 0 || s[0] == '\0') return fallback;
    if (s[0] == '#') ++s;
    while (s[i] && i < 6) {
        char c = s[i];
        uint32_t n;
        if (c >= '0' && c <= '9') n = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') n = (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') n = (uint32_t)(c - 'A' + 10);
        else return fallback;
        v = (v << 4) | n;
        ++i;
    }
    return i == 6 ? v : fallback;
}

static int parse_decimal(const char *s, int fallback) {
    int sign = 1;
    int v = 0;
    int any = 0;
    if (s == 0) return fallback;
    if (*s == '-') { sign = -1; ++s; }
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        any = 1;
        ++s;
    }
    return any ? v * sign : fallback;
}

static void persist_launcher(struct desktop_state *desktop, int index) {
    char data[256];
    char tmp[16];
    int pos = 0;
    int v;
    char digits[12];
    int n;

#define ADD_STR(s) do { size_t _i=0; while ((s)[_i] && pos + 1 < (int)sizeof(data)) data[pos++] = (s)[_i++]; data[pos] = '\0'; } while (0)
#define ADD_INT(num) do { v=(num); n=0; if(v<0){ if(pos+1<(int)sizeof(data)) data[pos++]='-'; v=-v; } if(v==0) digits[n++]='0'; while(v && n<(int)sizeof(digits)){ digits[n++]=(char)('0'+(v%10)); v/=10; } while(n && pos+1<(int)sizeof(data)) data[pos++]=digits[--n]; data[pos]='\0'; } while (0)

    if (index < 0 || index >= desktop->launcher_count) return;
    data[0] = '\0';
    ADD_STR("Name="); ADD_STR(desktop->launcher_names[index]); ADD_STR("\n");
    ADD_STR("Exec="); ADD_STR(desktop->launcher_execs[index]); ADD_STR("\n");
    ADD_STR("IconColor=#");
    {
        static const char hex[] = "0123456789abcdef";
        uint32_t c = desktop->launcher_colors[index];
        int shift;
        for (shift = 20; shift >= 0 && pos + 1 < (int)sizeof(data); shift -= 4) {
            data[pos++] = hex[(c >> shift) & 0x0fu];
        }
        data[pos] = '\0';
    }
    ADD_STR("\nDesktopX="); ADD_INT(desktop->launcher_x[index]);
    ADD_STR("\nDesktopY="); ADD_INT(desktop->launcher_y[index]); ADD_STR("\n");
    (void)tmp;
    (void)v;
    (void)digits;
    (void)n;
    (void)vfs_write_all(desktop->launcher_paths[index], data, string_length(data));

#undef ADD_INT
#undef ADD_STR
}

static void desktop_refresh_launchers(struct desktop_state *desktop) {
    struct vfs_stat st;
    uint32_t index = 0;
    desktop->launcher_count = 0;

    if (!vfs_stat_path("/home/user", &st)) (void)vfs_mkdir("/home/user");
    if (!vfs_stat_path("/home/user/Desktop", &st)) (void)vfs_mkdir("/home/user/Desktop");
    if (!vfs_stat_path("/home/user/Desktop/Browser.desktop", &st)) {
        static const char browser_desktop[] =
            "Name=Browser\n"
            "Exec=/bin/browser\n"
            "IconColor=#64f2cc\n";
        (void)vfs_write_all("/home/user/Desktop/Browser.desktop", browser_desktop, sizeof(browser_desktop) - 1u);
    }

    while (desktop->launcher_count < DESKTOP_LAUNCHER_MAX) {
        struct vfs_dir_entry entry;
        char path[96];
        char data[512];
        ssize_t n;
        char color[16];
        int li;

        if (!vfs_readdir("/home/user/Desktop", index++, &entry)) break;
        if (entry.kind != VFS_NODE_FILE || !text_ends_with(entry.name, ".desktop")) continue;

        copy_text(path, sizeof(path), "/home/user/Desktop/");
        append_text(path, sizeof(path), entry.name);
        n = vfs_read(path, 0, data, sizeof(data) - 1u);
        if (n <= 0) continue;
        data[n] = '\0';

        li = desktop->launcher_count;
        if (!line_value(data, "Name", desktop->launcher_names[li], sizeof(desktop->launcher_names[li]))) {
            copy_text(desktop->launcher_names[li], sizeof(desktop->launcher_names[li]), entry.name);
        }
        if (!line_value(data, "Exec", desktop->launcher_execs[li], sizeof(desktop->launcher_execs[li]))) {
            continue;
        }
        copy_text(desktop->launcher_paths[li], sizeof(desktop->launcher_paths[li]), path);
        color[0] = '\0';
        (void)line_value(data, "IconColor", color, sizeof(color));
        desktop->launcher_colors[li] = parse_hex_color(color, 0x008f7bf0u);
        {
            char xy[16];
            struct desktop_icon def = launcher_icon_at(desktop, li);
            xy[0] = '\0';
            desktop->launcher_x[li] = line_value(data, "DesktopX", xy, sizeof(xy)) ? parse_decimal(xy, def.x) : def.x;
            xy[0] = '\0';
            desktop->launcher_y[li] = line_value(data, "DesktopY", xy, sizeof(xy)) ? parse_decimal(xy, def.y) : def.y;
        }
        desktop->launcher_count++;
    }
}

static struct desktop_icon desktop_icon_at_size(int index, int screen_width, int screen_height) {
    struct desktop_icon icon;
    int dense = screen_width >= 1200 || screen_height >= 900;
    int base_x = dense ? 52 : 36;
    int base_y = dense ? 148 : 126;
    int step_y = dense ? 120 : 102;

    (void)screen_width;
    (void)screen_height;

    icon.x = base_x;
    icon.y = base_y + (index * step_y);
    icon.launcher_index = -1;

    if (index == 0) {
        icon.window_index = WINDOW_FILES;
        icon.color = 0x0036b8ffu;
        icon.label = "FILES";
    } else if (index == 1) {
        icon.window_index = WINDOW_TERMINAL;
        icon.color = 0x0063e6beu;
        icon.label = "TERM";
    } else if (index == 2) {
        icon.window_index = WINDOW_TASK_MANAGER;
        icon.color = 0x00ef7f7fu;
        icon.label = "TASKS";
    } else {
        icon.window_index = WINDOW_INFO;
        icon.color = 0x00f5ad4eu;
        icon.label = "INFO";
    }

    return icon;
}

/* macOS-style dock geometry: a centred, rounded bar along the bottom. */
static int ui_dock_icon_size(const struct desktop_state *desktop) { return large_ui(desktop) ? 52 : 40; }
static int ui_dock_icon_gap(const struct desktop_state *desktop) { return large_ui(desktop) ? 18 : 14; }
static int ui_dock_pad(const struct desktop_state *desktop) { return large_ui(desktop) ? 14 : 10; }

static int dock_icon_count(const struct desktop_state *desktop) {
    int i;
    int n = DESKTOP_ICON_COUNT;
    for (i = 0; i < MAX_USER_APPS; i++)
        if (desktop->user_apps[i].created) n++;
    return n;
}

static void ui_dock_metrics(const struct desktop_state *desktop, int *dx, int *dy, int *dw, int *dh) {
    int isz = ui_dock_icon_size(desktop);
    int gap = ui_dock_icon_gap(desktop);
    int pad = ui_dock_pad(desktop);
    int count = dock_icon_count(desktop);
    int inner = count * isz + (count - 1) * gap;
    int w = inner + 2 * pad;
    int h = isz + 2 * pad;
    *dw = w;
    *dh = h;
    *dx = ((int)desktop->screen_width - w) / 2;
    *dy = (int)desktop->screen_height - h - (large_ui(desktop) ? 16 : 12);
}

static struct desktop_icon desktop_icon_at(const struct desktop_state *desktop, int index) {
    struct desktop_icon icon;
    int dx, dy, dw, dh;
    int isz = ui_dock_icon_size(desktop);
    int gap = ui_dock_icon_gap(desktop);
    int pad = ui_dock_pad(desktop);

    ui_dock_metrics(desktop, &dx, &dy, &dw, &dh);
    icon.x = dx + pad + index * (isz + gap);
    icon.y = dy + pad;
    icon.launcher_index = -1;

    if (index == 0) {
        icon.window_index = WINDOW_FILES;
        icon.color = 0x0036b8ffu;
        icon.label = "FILES";
    } else if (index == 1) {
        icon.window_index = WINDOW_TERMINAL;
        icon.color = 0x0063e6beu;
        icon.label = "TERM";
    } else if (index == 2) {
        icon.window_index = WINDOW_TASK_MANAGER;
        icon.color = 0x00ef7f7fu;
        icon.label = "TASKS";
    } else {
        icon.window_index = WINDOW_INFO;
        icon.color = 0x00f5ad4eu;
        icon.label = "INFO";
    }

    return icon;
}

static struct desktop_icon dock_icon_at(const struct desktop_state *desktop, int index) {
    struct desktop_icon icon;
    int dx, dy, dw, dh;
    int isz = ui_dock_icon_size(desktop);
    int gap = ui_dock_icon_gap(desktop);
    int pad = ui_dock_pad(desktop);

    if (index < DESKTOP_ICON_COUNT) {
        return desktop_icon_at(desktop, index);
    }

    /* Map dock position to a created user-app slot (in slot order). */
    {
        int app_pos = index - DESKTOP_ICON_COUNT;
        int i;
        int found = -1;
        int count = 0;
        for (i = 0; i < MAX_USER_APPS; i++) {
            if (desktop->user_apps[i].created) {
                if (count == app_pos) { found = i; break; }
                count++;
            }
        }
        if (found < 0) found = 0; /* fallback */

        ui_dock_metrics(desktop, &dx, &dy, &dw, &dh);
        (void)dw;
        (void)dh;
        icon.x = dx + pad + index * (isz + gap);
        icon.y = dy + pad;
        icon.window_index = WINDOW_APP_FIRST + found;
        icon.color = 0x008f7bf0u;
        icon.label = desktop->user_apps[found].title[0] ? desktop->user_apps[found].title : "APP";
        icon.launcher_index = -1;
        return icon;
    }
}

static struct desktop_icon launcher_icon_at(const struct desktop_state *desktop, int index) {
    struct desktop_icon icon;
    int sz = ui_dock_icon_size(desktop);
    int col = index % 2;
    int row = index / 2;
    icon.x = (large_ui(desktop) ? 48 : 32) + col * (sz + (large_ui(desktop) ? 86 : 70));
    icon.y = (large_ui(desktop) ? 122 : 96) + row * (sz + (large_ui(desktop) ? 64 : 54));
    if (index >= 0 && index < desktop->launcher_count) {
        icon.x = desktop->launcher_x[index];
        icon.y = desktop->launcher_y[index];
    }
    icon.color = desktop->launcher_colors[index];
    icon.label = desktop->launcher_names[index];
    icon.window_index = -1;
    icon.launcher_index = index;
    return icon;
}

static int point_in_rect(int px, int py, int x, int y, int width, int height) {
    return px >= x && py >= y && px < x + width && py < y + height;
}

static int rects_intersect(const struct rect *a, const struct rect *b) {
    return a->x < b->x + b->width &&
           a->x + a->width > b->x &&
           a->y < b->y + b->height &&
           a->y + a->height > b->y;
}

static int rect_is_empty(const struct rect *rect) {
    return rect->width <= 0 || rect->height <= 0;
}

static struct rect rect_from_bounds(int x, int y, int width, int height) {
    struct rect rect;
    rect.x = x;
    rect.y = y;
    rect.width = width;
    rect.height = height;
    return rect;
}

static void clip_rect_to_screen(const struct desktop_state *desktop, struct rect *rect) {
    int x0 = rect->x;
    int y0 = rect->y;
    int x1 = rect->x + rect->width;
    int y1 = rect->y + rect->height;

    if (x0 < 0) {
        x0 = 0;
    }
    if (y0 < 0) {
        y0 = 0;
    }
    if (x1 > (int)desktop->screen_width) {
        x1 = (int)desktop->screen_width;
    }
    if (y1 > (int)desktop->screen_height) {
        y1 = (int)desktop->screen_height;
    }

    rect->x = x0;
    rect->y = y0;
    rect->width = x1 - x0;
    rect->height = y1 - y0;
}

static void mark_dirty_rect(struct desktop_state *desktop, struct rect rect) {
    int x0;
    int y0;
    int x1;
    int y1;

    clip_rect_to_screen(desktop, &rect);
    if (rect_is_empty(&rect)) {
        return;
    }

    if (!desktop->dirty) {
        desktop->dirty = 1;
        desktop->dirty_rect = rect;
        return;
    }

    x0 = rect.x < desktop->dirty_rect.x ? rect.x : desktop->dirty_rect.x;
    y0 = rect.y < desktop->dirty_rect.y ? rect.y : desktop->dirty_rect.y;
    x1 = rect.x + rect.width > desktop->dirty_rect.x + desktop->dirty_rect.width ? rect.x + rect.width : desktop->dirty_rect.x + desktop->dirty_rect.width;
    y1 = rect.y + rect.height > desktop->dirty_rect.y + desktop->dirty_rect.height ? rect.y + rect.height : desktop->dirty_rect.y + desktop->dirty_rect.height;

    desktop->dirty_rect.x = x0;
    desktop->dirty_rect.y = y0;
    desktop->dirty_rect.width = x1 - x0;
    desktop->dirty_rect.height = y1 - y0;
}

static void mark_background_dirty(struct desktop_state *desktop) {
    desktop->background_dirty = 1;
}

static struct rect dock_dirty_rect(const struct desktop_state *desktop) {
    int dx;
    int dy;
    int dw;
    int dh;

    ui_dock_metrics(desktop, &dx, &dy, &dw, &dh);
    return rect_from_bounds(dx - 10, dy - 10, dw + 20, dh + 24);
}

static void mark_dock_dirty(struct desktop_state *desktop) {
    mark_background_dirty(desktop);
    mark_dirty_rect(desktop, dock_dirty_rect(desktop));
}

static void mark_window_dirty(struct desktop_state *desktop, int index) {
    struct window_state *window;

    if (index < 0 || index >= WINDOW_COUNT) {
        return;
    }

    window = &desktop->windows[index];
    desktop->window_dirty[index] = 1;
    desktop->window_dirty_rects[index] = rect_from_bounds(0, 0, window->width, window->height);
}

static void mark_window_dirty_region(struct desktop_state *desktop, int index, struct rect rect) {
    struct window_state *window;
    struct rect *current;
    int x0;
    int y0;
    int x1;
    int y1;

    if (index < 0 || index >= WINDOW_COUNT) {
        return;
    }

    window = &desktop->windows[index];
    if (rect.x < 0) {
        rect.width += rect.x;
        rect.x = 0;
    }
    if (rect.y < 0) {
        rect.height += rect.y;
        rect.y = 0;
    }
    if (rect.x + rect.width > window->width) {
        rect.width = window->width - rect.x;
    }
    if (rect.y + rect.height > window->height) {
        rect.height = window->height - rect.y;
    }
    if (rect.width <= 0 || rect.height <= 0) {
        return;
    }

    if (!desktop->window_dirty[index]) {
        desktop->window_dirty[index] = 1;
        desktop->window_dirty_rects[index] = rect;
        return;
    }

    current = &desktop->window_dirty_rects[index];
    x0 = rect.x < current->x ? rect.x : current->x;
    y0 = rect.y < current->y ? rect.y : current->y;
    x1 = rect.x + rect.width > current->x + current->width ? rect.x + rect.width : current->x + current->width;
    y1 = rect.y + rect.height > current->y + current->height ? rect.y + rect.height : current->y + current->height;
    current->x = x0;
    current->y = y0;
    current->width = x1 - x0;
    current->height = y1 - y0;
}

static void build_app_draw_context(struct desktop_state *desktop, const struct window_state *window, int focused, struct app_draw_context *app_ctx) {
    app_ctx->fb = 0;
    app_ctx->window = window;
    app_ctx->window_width = window->width;
    app_ctx->window_height = window->height;
    app_ctx->focused = focused;
    app_ctx->content_x = large_ui(desktop) ? 22 : 16;
    app_ctx->content_y = large_ui(desktop) ? 58 : 48;
    app_ctx->content_width = window->width - (large_ui(desktop) ? 44 : 32);
    app_ctx->content_height = window->height - (large_ui(desktop) ? 82 : 66);
    app_ctx->text_scale = window->app_slot == WINDOW_TERMINAL ? ui_terminal_text_scale(desktop) : ui_window_text_scale(desktop);
    app_ctx->line_step = window->app_slot == WINDOW_TERMINAL ? ui_terminal_line_step(desktop) : text_line_height(app_ctx->text_scale) + (large_ui(desktop) ? 10 : 7);
    app_ctx->large_ui = large_ui(desktop);
}

static struct rect window_rect(const struct window_state *window) {
    return rect_from_bounds(window->x - WINDOW_SHADOW_SPREAD, window->y - WINDOW_SHADOW_SPREAD, window->width + (WINDOW_SHADOW_SPREAD * 2), window->height + (WINDOW_SHADOW_SPREAD * 2));
}

static struct rect surface_rect_to_screen(const struct window_state *window, const struct rect *rect) {
    return rect_from_bounds(window->x + rect->x, window->y + rect->y, rect->width, rect->height);
}

static struct rect cursor_rect(const struct desktop_state *desktop, int x, int y) {
    int size = large_ui(desktop) ? 24 : 18;
    return rect_from_bounds(x, y, 12, size);
}

static struct rect taskbar_rect(const struct desktop_state *desktop) {
    int taskbar_height = ui_taskbar_height(desktop);
    return rect_from_bounds(0, (int)desktop->screen_height - taskbar_height, (int)desktop->screen_width, taskbar_height);
}

static int point_in_icon(int px, int py, const struct desktop_icon *icon, int size) {
    return point_in_rect(px, py, icon->x, icon->y, size, size);
}

enum window_button {
    WIN_BTN_NONE = 0,
    WIN_BTN_CLOSE = 1,
    WIN_BTN_MAX = 2,
    WIN_BTN_MIN = 3
};

enum resize_edge {
    RESIZE_LEFT = 1,
    RESIZE_RIGHT = 2,
    RESIZE_TOP = 4,
    RESIZE_BOTTOM = 8
};

static void app_enqueue_event_slot(struct desktop_state *desktop, int slot, uint32_t type, int x, int y, uint32_t buttons, uint32_t key);

/* ---- Multi-app slot helpers ---- */
static int is_user_app_slot(int idx) {
    return idx >= WINDOW_APP_FIRST && idx < WINDOW_APP_FIRST + MAX_USER_APPS;
}
static int slot_index(int window_idx) {
    return window_idx - WINDOW_APP_FIRST;
}
static int find_slot_by_pid(struct desktop_state *d, uint32_t pid) {
    int i;
    for (i = 0; i < MAX_USER_APPS; i++)
        if (d->user_apps[i].created && d->user_apps[i].pid == pid) return i;
    return -1;
}
static int find_free_slot(struct desktop_state *d) {
    int i;
    for (i = 0; i < MAX_USER_APPS; i++)
        if (!d->user_apps[i].created) return i;
    return 0; /* reuse slot 0 if all full */
}


/* Which traffic-light control (if any) is under the cursor. Geometry mirrors
 * the buttons drawn in draw_window_frame so the hit-testing actually lines up. */
static int window_button_hit(const struct desktop_state *desktop, const struct window_state *window, int px, int py) {
    int large = large_ui(desktop);
    int size = large ? 16 : 12;
    int gap = large ? 24 : 18;
    int by = window->y + (large ? 12 : 10);
    int xr = window->x + window->width;
    int x_close = xr - gap - (large ? 4 : 0);
    int x_max = xr - (gap * 2) - (large ? 6 : 0);
    int x_min = xr - (gap * 3) - (large ? 8 : 0);
    int pad = 4;

    if (point_in_rect(px, py, x_close - pad, by - pad, size + 2 * pad, size + 2 * pad)) {
        return WIN_BTN_CLOSE;
    }
    if (point_in_rect(px, py, x_max - pad, by - pad, size + 2 * pad, size + 2 * pad)) {
        return WIN_BTN_MAX;
    }
    if (point_in_rect(px, py, x_min - pad, by - pad, size + 2 * pad, size + 2 * pad)) {
        return WIN_BTN_MIN;
    }
    return WIN_BTN_NONE;
}

static int titlebar_hit(const struct desktop_state *desktop, const struct window_state *window, int px, int py) {
    return point_in_rect(px, py, window->x, window->y, window->width, ui_titlebar_height(desktop));
}

static int resize_hit(const struct desktop_state *desktop, const struct window_state *window, int px, int py) {
    int edge_grip = large_ui(desktop) ? 12 : 9;
    int corner_grip = large_ui(desktop) ? 26 : 20;
    int edges = 0;

    if (!point_in_rect(px, py, window->x, window->y, window->width, window->height)) {
        return 0;
    }
    if (px < window->x + edge_grip) edges |= RESIZE_LEFT;
    if (px >= window->x + window->width - edge_grip) edges |= RESIZE_RIGHT;
    if (py < window->y + edge_grip) edges |= RESIZE_TOP;
    if (py >= window->y + window->height - edge_grip) edges |= RESIZE_BOTTOM;
    if (px >= window->x + window->width - corner_grip && py >= window->y + window->height - corner_grip) {
        edges |= RESIZE_RIGHT | RESIZE_BOTTOM;
    }
    if (px < window->x + corner_grip && py >= window->y + window->height - corner_grip) {
        edges |= RESIZE_LEFT | RESIZE_BOTTOM;
    }
    if (px >= window->x + window->width - corner_grip && py < window->y + corner_grip) {
        edges |= RESIZE_RIGHT | RESIZE_TOP;
    }
    if (px < window->x + corner_grip && py < window->y + corner_grip) {
        edges |= RESIZE_LEFT | RESIZE_TOP;
    }
    if ((edges & (RESIZE_LEFT | RESIZE_RIGHT)) && py < window->y + ui_titlebar_height(desktop)) {
        edges &= ~(RESIZE_LEFT | RESIZE_RIGHT);
    }
    return edges;
}

static void app_content_size_for_window(struct desktop_state *desktop, const struct window_state *window, int *cw, int *ch) {
    int ix = large_ui(desktop) ? 44 : 32;
    int iy = large_ui(desktop) ? 82 : 66;
    *cw = window->width - ix;
    *ch = window->height - iy;
    if (*cw < 1) *cw = 1;
    if (*ch < 1) *ch = 1;
}

static void app_window_max_size(struct desktop_state *desktop, int *max_w, int *max_h) {
    *max_w = (int)WINDOW_APP_CONTENT_MAX_WIDTH + (large_ui(desktop) ? 44 : 32);
    *max_h = (int)WINDOW_APP_CONTENT_MAX_HEIGHT + (large_ui(desktop) ? 82 : 66);
    if (*max_w > (int)WINDOW_APP_SURFACE_MAX_WIDTH) *max_w = (int)WINDOW_APP_SURFACE_MAX_WIDTH;
    if (*max_h > (int)WINDOW_APP_SURFACE_MAX_HEIGHT) *max_h = (int)WINDOW_APP_SURFACE_MAX_HEIGHT;
}

static void update_app_content_size_slot(struct desktop_state *desktop, int slot) {
    int cw;
    int ch;
    int win_idx = WINDOW_APP_FIRST + slot;
    app_content_size_for_window(desktop, &desktop->windows[win_idx], &cw, &ch);
    if (cw > (int)WINDOW_APP_CONTENT_MAX_WIDTH) cw = (int)WINDOW_APP_CONTENT_MAX_WIDTH;
    if (ch > (int)WINDOW_APP_CONTENT_MAX_HEIGHT) ch = (int)WINDOW_APP_CONTENT_MAX_HEIGHT;
    if (desktop->user_apps[slot].content_width != cw || desktop->user_apps[slot].content_height != ch) {
        desktop->user_apps[slot].content_width = cw;
        desktop->user_apps[slot].content_height = ch;
        app_enqueue_event_slot(desktop, slot, WINSYS_EVENT_RESIZE, cw, ch, 0, 0);
    }
}

static void clamp_window_to_screen(struct desktop_state *desktop, struct window_state *window) {
    /* Allow windows to be dragged right up to the screen edge (they may cover
     * the icon dock on purpose). Only the initial placement avoids the dock. */
    if (window->x < 6) {
        window->x = 6;
    }
    if (window->y < 8) {
        window->y = 8;
    }
    if (window->x + window->width > (int)desktop->screen_width - 16) {
        window->x = (int)desktop->screen_width - window->width - 16;
    }
    if (window->y + window->height > (int)desktop->screen_height - ui_taskbar_height(desktop)) {
        window->y = (int)desktop->screen_height - window->height - ui_taskbar_height(desktop);
    }
}

static void focus_window(struct desktop_state *desktop, int index) {
    int order;
    int position = -1;
    int previous_focus = desktop->focused_window;

    if (index < 0 || index >= WINDOW_COUNT) {
        desktop->focused_window = -1;
        return;
    }

    desktop->focused_window = index;
    for (order = 0; order < WINDOW_COUNT; ++order) {
        if (desktop->z_order[order] == index) {
            position = order;
            break;
        }
    }

    if (position < 0) {
        return;
    }

    for (order = position; order + 1 < WINDOW_COUNT; ++order) {
        desktop->z_order[order] = desktop->z_order[order + 1];
    }
    desktop->z_order[WINDOW_COUNT - 1] = index;

    if (previous_focus >= 0 && previous_focus < WINDOW_COUNT && previous_focus != index) {
        mark_window_dirty(desktop, previous_focus);
        mark_dirty_rect(desktop, window_rect(&desktop->windows[previous_focus]));
    }
    if (previous_focus != index) {
        mark_window_dirty(desktop, index);
        mark_dirty_rect(desktop, window_rect(&desktop->windows[index]));
    }
}

static int topmost_window_at(struct desktop_state *desktop, int px, int py) {
    int order;

    for (order = WINDOW_COUNT - 1; order >= 0; --order) {
        int index = desktop->z_order[order];
        struct window_state *window = &desktop->windows[index];

        if (window->visible && point_in_rect(px, py, window->x, window->y, window->width, window->height)) {
            return index;
        }
    }

    return -1;
}

static void draw_window_frame(struct desktop_state *desktop, struct framebuffer *fb, const struct window_state *window, int focused) {
    int titlebar_height = ui_titlebar_height(desktop);
    int button_y = window->y + (large_ui(desktop) ? 12 : 10);
    int button_size = large_ui(desktop) ? 16 : 12;
    int button_gap = large_ui(desktop) ? 24 : 18;
    uint32_t border = focused ? 0x004c5f7cu : 0x002b384eu;
    uint32_t panel_top = mix_color(window->body, 0x00ffffffu, 1u, 20u);
    uint32_t panel_bottom = mix_color(window->body, 0x00000000u, 1u, 16u);
    uint32_t title_top = mix_color(window->titlebar, 0x00ffffffu, 1u, 16u);
    uint32_t title_bottom = window->titlebar;
    int radius = large_ui(desktop) ? 18 : 12;

    draw_rounded_panel(fb, window->x, window->y, window->width, window->height, radius, panel_top, panel_bottom, border, 0x00ffffffu);
    draw_rounded_panel(fb, window->x + 2, window->y + 2, window->width - 4, titlebar_height, radius > 2 ? radius - 2 : radius, title_top, title_bottom, mix_color(window->titlebar, 0x00ffffffu, 1u, 12u), 0x00ffffffu);
    /* thin, subtle accent line separating the title bar from the body */
    fb_fill_rect(fb, window->x + (large_ui(desktop) ? 16 : 12), window->y + titlebar_height, window->width - (large_ui(desktop) ? 32 : 24), 2, window->accent);

    /* traffic-light controls (positions kept stable for hit-testing) */
    draw_rounded_panel(fb, window->x + window->width - (button_gap * 3) - (large_ui(desktop) ? 8 : 0), button_y, button_size, button_size, button_size / 2, 0x00ffd166u, 0x00d99a2bu, 0x00ffe4a3u, 0x00ffffffu);
    draw_rounded_panel(fb, window->x + window->width - (button_gap * 2) - (large_ui(desktop) ? 6 : 0), button_y, button_size, button_size, button_size / 2, 0x00ff9e64u, 0x00d06a30u, 0x00ffc9a3u, 0x00ffffffu);
    draw_rounded_panel(fb, window->x + window->width - button_gap - (large_ui(desktop) ? 4 : 0), button_y, button_size, button_size, button_size / 2, 0x00ff6b81u, 0x00d23b52u, 0x00ffb3c0u, 0x00ffffffu);

    draw_text(fb, window->x + (large_ui(desktop) ? 18 : 14), window->y + (titlebar_height - 16) / 2 + 1, window->title, focused ? 0x00f3f7fdu : 0x00b9c4d4u, 1);
    if (!window->maximized && window->width > 80 && window->height > 80) {
        int gx = window->x + window->width - (large_ui(desktop) ? 20 : 16);
        int gy = window->y + window->height - (large_ui(desktop) ? 20 : 16);
        uint32_t grip = focused ? mix_color(window->accent, 0x00ffffffu, 1u, 4u) : 0x00506476u;
        fb_fill_rect(fb, gx + 10, gy + 4, 2, 10, grip);
        fb_fill_rect(fb, gx + 6, gy + 8, 2, 6, grip);
        fb_fill_rect(fb, gx + 2, gy + 12, 2, 2, grip);
    }
}

static void draw_icon(struct desktop_state *desktop, struct framebuffer *fb, const struct desktop_icon *icon) {
    int sz = ui_dock_icon_size(desktop);
    int radius = large_ui(desktop) ? 13 : 10;
    uint32_t icon_top = mix_color(icon->color, 0x00ffffffu, 3u, 10u);
    uint32_t icon_bottom = mix_color(icon->color, 0x00000000u, 1u, 6u);
    int running = icon->window_index >= 0 && icon->window_index < WINDOW_COUNT && desktop->windows[icon->window_index].visible;
    int gx, gy;

    /* rounded app tile with a subtle glossy top */
    draw_rounded_panel(fb, icon->x, icon->y, sz, sz, radius, icon_top, icon_bottom,
                       mix_color(icon->color, 0x00ffffffu, 2u, 10u), 0x00ffffffu);

    /* a simple glyph: first letter of the label, centred */
    {
        int scale = large_ui(desktop) ? 3 : 2;
        int gw = 7 * scale;
        char letter[2];
        letter[0] = icon->label[0];
        letter[1] = 0;
        gx = icon->x + (sz - gw) / 2;
        gy = icon->y + (sz - 16 * scale / 2) / 2 - 2;
        draw_text(fb, gx, gy, letter, 0x00ffffffu, scale);
    }

    /* running indicator: a bright dot centred below the tile (macOS style) */
    if (running) {
        int dot = large_ui(desktop) ? 5 : 4;
        fb_fill_rect(fb, icon->x + sz / 2 - dot / 2, icon->y + sz + (large_ui(desktop) ? 5 : 4), dot, dot, 0x00cfe6ffu);
    }
}

static void draw_desktop_launcher_icon(struct desktop_state *desktop, struct framebuffer *fb, const struct desktop_icon *icon) {
    int sz = ui_dock_icon_size(desktop);
    int len = (int)string_length(icon->label);
    int max_chars = large_ui(desktop) ? 14 : 12;
    char label[16];
    int i;
    int label_w;
    int label_x;
    int label_y;

    draw_icon(desktop, fb, icon);

    if (len > max_chars) len = max_chars;
    for (i = 0; i < len && i + 1 < (int)sizeof(label); ++i) {
        label[i] = icon->label[i];
    }
    label[i] = '\0';

    label_w = i * text_char_advance(1);
    label_x = icon->x + (sz - label_w) / 2;
    label_y = icon->y + sz + (large_ui(desktop) ? 10 : 8);

    /* Small dark backing keeps labels readable on the gradient desktop. */
    fb_fill_rect(fb, label_x - 4, label_y - 2, label_w + 8, 18, 0x66000000u);
    draw_text(fb, label_x, label_y, label, 0x00eef6ffu, 1);
}

/* macOS-style dock: a centred, rounded, translucent bar holding the app
 * tiles, with running-app indicator dots. Replaces the old full-width
 * taskbar and left icon column. */
static void draw_dock(struct desktop_state *desktop, struct framebuffer *fb) {
    int dx, dy, dw, dh;
    int i;

    ui_dock_metrics(desktop, &dx, &dy, &dw, &dh);

    draw_soft_shadow(fb, dx, dy, dw, dh, 22, 5, 0x000a0f18u);
    draw_rounded_panel(fb, dx, dy, dw, dh, 22, 0x00263247u, 0x00161f30u, 0x003a4a66u, 0x00ffffffu);
    /* glossy top highlight line */
    fb_fill_rect(fb, dx + 14, dy + 2, dw - 28, 1, 0x00415574u);

    for (i = 0; i < dock_icon_count(desktop); ++i) {
        struct desktop_icon icon = dock_icon_at(desktop, i);
        draw_icon(desktop, fb, &icon);
    }
}

static void draw_cursor(struct desktop_state *desktop, struct framebuffer *fb, int x, int y) {
    int size = large_ui(desktop) ? 24 : 18;

    fb_fill_rect(fb, x, y, 3, size, 0x00ffffffu);
    fb_fill_rect(fb, x + 3, y + 3, 3, size - 5, 0x00ffffffu);
    fb_fill_rect(fb, x + 6, y + 6, 3, size - 11, 0x00ffffffu);
    fb_fill_rect(fb, x + 9, y + 9, 3, size - 17, 0x00ffffffu);
    fb_fill_rect(fb, x + 1, y + 1, 1, size - 1, 0x00000000u);
}

static uint32_t *surface_storage_for_window(struct desktop_state *desktop, int index) {
    switch (index) {
        case WINDOW_INFO:
            return desktop->info_surface_storage;
        case WINDOW_FILES:
            return desktop->files_surface_storage;
        case WINDOW_TERMINAL:
            return desktop->terminal_surface_storage;
        case WINDOW_TASK_MANAGER:
            return desktop->tasks_surface_storage;
        default:
            if (is_user_app_slot(index)) {
                return desktop->user_apps[slot_index(index)].surface_storage;
            }
            return 0;
    }
}

/* Maximum surface (and therefore window) size for a slot, bounded by the
 * statically-allocated backing buffer in struct desktop_state. */
static void window_max_surface(int index, int *out_w, int *out_h) {
    switch (index) {
        case WINDOW_INFO: *out_w = WINDOW_INFO_MAX_WIDTH; *out_h = WINDOW_INFO_MAX_HEIGHT; break;
        case WINDOW_FILES: *out_w = WINDOW_FILES_MAX_WIDTH; *out_h = WINDOW_FILES_MAX_HEIGHT; break;
        case WINDOW_TERMINAL: *out_w = WINDOW_TERMINAL_MAX_WIDTH; *out_h = WINDOW_TERMINAL_MAX_HEIGHT; break;
        case WINDOW_TASK_MANAGER: *out_w = WINDOW_TASKS_MAX_WIDTH; *out_h = WINDOW_TASKS_MAX_HEIGHT; break;
        default:
            if (is_user_app_slot(index)) {
                *out_w = WINDOW_APP_SURFACE_MAX_WIDTH; *out_h = WINDOW_APP_SURFACE_MAX_HEIGHT;
            } else {
                *out_w = WINDOW_TASKS_MAX_WIDTH; *out_h = WINDOW_TASKS_MAX_HEIGHT;
            }
            break;
    }
}

/* Toggle a window between its normal geometry and the largest size its backing
 * surface allows (clamped to the free desktop area). Re-inits the window
 * framebuffer so the new pitch matches the new width. */
static void window_toggle_maximize(struct desktop_state *desktop, int index) {
    struct window_state *w = &desktop->windows[index];

    if (!w->maximized) {
        int maxw;
        int maxh;
        int availw = (int)desktop->screen_width - ui_dock_width(desktop) - 24;
        int availh = (int)desktop->screen_height - ui_taskbar_height(desktop) - 24;

        if (is_user_app_slot(index)) {
            app_window_max_size(desktop, &maxw, &maxh);
        } else {
            window_max_surface(index, &maxw, &maxh);
        }
        w->restore_x = w->x;
        w->restore_y = w->y;
        w->restore_width = w->width;
        w->restore_height = w->height;
        w->width = maxw < availw ? maxw : availw;
        w->height = maxh < availh ? maxh : availh;
        w->x = ui_dock_width(desktop) + 12;
        w->y = 12;
        w->maximized = 1;
    } else {
        w->x = w->restore_x;
        w->y = w->restore_y;
        w->width = w->restore_width;
        w->height = w->restore_height;
        w->maximized = 0;
    }

    clamp_window_to_screen(desktop, w);
    fb_init(&desktop->window_fbs[index], (uintptr_t)surface_storage_for_window(desktop, index), w->width, w->height, w->width * 4u, 32u);
    if (is_user_app_slot(index) && desktop->user_apps[slot_index(index)].created) {
        update_app_content_size_slot(desktop, slot_index(index));
    }
}

static void draw_shadow_block(struct framebuffer *fb, int x, int y, int width, int height);

static void render_background_surface(struct desktop_state *desktop) {
    struct framebuffer *fb = &desktop->background_fb;
    int order;

    fb_reset_clip(fb);
    draw_gradient_background(fb, 0x000b1120u, 0x00162338u);
    draw_text(fb, large_ui(desktop) ? 42 : 34, large_ui(desktop) ? 30 : 28, "VIBEOS DESKTOP", 0x00dfe8f5u, 2);

    for (order = 0; order < desktop->launcher_count; ++order) {
        struct desktop_icon icon = launcher_icon_at(desktop, order);
        draw_desktop_launcher_icon(desktop, fb, &icon);
    }

    draw_dock(desktop, fb);

    /* Window shadows are NOT baked in here anymore; they are composited live
     * in compose_scene_rect so the background can stay static during drags. */
    (void)order;
    desktop->background_dirty = 0;
}

static void render_window_surface(struct desktop_state *desktop, int index) {
    struct window_state *window = &desktop->windows[index];
    struct window_state local_window;
    struct framebuffer *fb = &desktop->window_fbs[index];
    struct app_draw_context app_ctx;
    int focused = desktop->focused_window == index;

    if (!window->visible) {
        desktop->window_dirty[index] = 0;
        return;
    }

    local_window = *window;
    local_window.x = 0;
    local_window.y = 0;
    build_app_draw_context(desktop, &local_window, focused, &app_ctx);
    app_ctx.fb = fb;
    fb_reset_clip(fb);
    fb_fill_rect(fb, 0, 0, window->width, window->height, WINDOW_TRANSPARENT_KEY);
    draw_window_frame(desktop, fb, &local_window, focused);

    if (is_user_app_slot(index)) {
        /* Content comes from the userspace app's last-presented pixel buffer. */
        int s = slot_index(index);
        int cx = app_ctx.content_x;
        int cy = app_ctx.content_y;
        int cw = desktop->user_apps[s].content_width;
        int ch = desktop->user_apps[s].content_height;
        int row;
        if (cw > app_ctx.content_width) cw = app_ctx.content_width;
        if (ch > app_ctx.content_height) ch = app_ctx.content_height;
        for (row = 0; row < ch; ++row) {
            const uint32_t *src = &desktop->user_apps[s].content_storage[(size_t)row * (size_t)WINDOW_APP_CONTENT_MAX_WIDTH];
            int col;
            for (col = 0; col < cw; ++col) {
                fb_put_pixel(fb, cx + col, cy + row, src[col]);
            }
        }
    } else {
        app_draw(&desktop->apps[window->app_slot], &app_ctx);
    }
    desktop->window_dirty[index] = 0;
    desktop->window_dirty_rects[index] = rect_from_bounds(0, 0, 0, 0);
}

static void blit_window_surface_region(struct framebuffer *dest, const struct framebuffer *src, int dest_x, int dest_y, const struct rect *clip_rect) {
    int x0 = dest_x > clip_rect->x ? dest_x : clip_rect->x;
    int y0 = dest_y > clip_rect->y ? dest_y : clip_rect->y;
    int x1 = dest_x + (int)src->width < clip_rect->x + clip_rect->width ? dest_x + (int)src->width : clip_rect->x + clip_rect->width;
    int y1 = dest_y + (int)src->height < clip_rect->y + clip_rect->height ? dest_y + (int)src->height : clip_rect->y + clip_rect->height;
    int y;

    if (x0 >= x1 || y0 >= y1) {
        return;
    }

    for (y = y0; y < y1; ++y) {
        uint32_t *dest_row = (uint32_t *)((uint8_t *)dest->base + ((uint32_t)y * dest->pitch));
        const uint32_t *src_row = (const uint32_t *)((const uint8_t *)src->base + ((uint32_t)(y - dest_y) * src->pitch));
        int x;
        for (x = x0; x < x1; ++x) {
            uint32_t px = src_row[x - dest_x];
            /* Skip transparent-key pixels so rounded corners reveal the desktop. */
            if (px != WINDOW_TRANSPARENT_KEY) {
                dest_row[x] = px;
            }
        }
    }
}

static void draw_shadow_block(struct framebuffer *fb, int x, int y, int width, int height) {
    draw_soft_shadow(fb, x, y, width, height, 12, WINDOW_SHADOW_SPREAD, 0x0009131eu);
}

/* Context-menu entries are built dynamically: the standard Show/Hide (plus
 * Quit App for userspace windows) followed by whatever the app itself declares.
 * Kernel apps declare entries via their vtable; the userspace WINDOW_APP via
 * SYS_WINDOW_SET_MENU. This keeps the menu contents owned by the app, not
 * hard-coded into the compositor. */
enum ctx_entry_kind {
    CTX_SHOW = 0,
    CTX_HIDE = 1,
    CTX_QUIT_APP = 2,
    CTX_APP_KERNEL = 3,
    CTX_APP_USER = 4
};

#define CTX_MAX_ENTRIES (3 + WINSYS_MAX_MENU_ITEMS)

struct ctx_entry {
    char label[WINSYS_MENU_LABEL_MAX];
    int kind;
    uint32_t action;
};

static void ctx_set_label(struct ctx_entry *e, const char *s) {
    int i;
    for (i = 0; i + 1 < WINSYS_MENU_LABEL_MAX && s && s[i]; ++i) {
        e->label[i] = s[i];
    }
    e->label[i] = '\0';
}

static int build_context_entries(struct desktop_state *desktop, int win, struct ctx_entry *out) {
    int n = 0;

    if (win < 0 || win >= WINDOW_COUNT) {
        return 0;
    }

    ctx_set_label(&out[n], "Show"); out[n].kind = CTX_SHOW; out[n].action = 0; ++n;
    ctx_set_label(&out[n], "Hide"); out[n].kind = CTX_HIDE; out[n].action = 0; ++n;

    if (is_user_app_slot(win)) {
        int s = slot_index(win);
        int i;
        ctx_set_label(&out[n], "Quit App"); out[n].kind = CTX_QUIT_APP; out[n].action = 0; ++n;
        for (i = 0; i < desktop->user_apps[s].menu_count && n < CTX_MAX_ENTRIES; ++i) {
            ctx_set_label(&out[n], desktop->user_apps[s].menu[i].label);
            out[n].kind = CTX_APP_USER;
            out[n].action = desktop->user_apps[s].menu[i].action_id;
            ++n;
        }
    } else {
        struct winsys_menu_item items[WINSYS_MAX_MENU_ITEMS];
        int count = app_menu_items(&desktop->apps[desktop->windows[win].app_slot], items, WINSYS_MAX_MENU_ITEMS);
        int i;
        for (i = 0; i < count && n < CTX_MAX_ENTRIES; ++i) {
            ctx_set_label(&out[n], items[i].label);
            out[n].kind = CTX_APP_KERNEL;
            out[n].action = items[i].action_id;
            ++n;
        }
    }

    return n;
}

static struct rect context_menu_rect(struct desktop_state *desktop) {
    struct ctx_entry entries[CTX_MAX_ENTRIES];
    int count = build_context_entries(desktop, desktop->context_menu_window, entries);
    int longest = 0;
    int width;
    int height;
    int x = desktop->context_menu_x;
    int y = desktop->context_menu_y;
    int i;

    if (count < 1) {
        count = 1;
    }
    for (i = 0; i < count; ++i) {
        int len = 0;
        while (entries[i].label[len]) ++len;
        if (len > longest) longest = len;
    }

    width = longest * text_char_advance(1) + 28;
    if (width < 112) width = 112;
    height = count * 22 + 18;

    if (x + width > (int)desktop->screen_width - 8) x = (int)desktop->screen_width - width - 8;
    if (y + height > (int)desktop->screen_height - 8) y = (int)desktop->screen_height - height - 8;
    if (x < 8) x = 8;
    if (y < 8) y = 8;
    return rect_from_bounds(x, y, width, height);
}

static void draw_context_menu(struct desktop_state *desktop, struct framebuffer *fb) {
    struct ctx_entry entries[CTX_MAX_ENTRIES];
    struct rect r;
    int count;
    int y;
    int i;

    if (!desktop->context_menu_open) {
        return;
    }

    count = build_context_entries(desktop, desktop->context_menu_window, entries);
    r = context_menu_rect(desktop);
    draw_soft_shadow(fb, r.x, r.y, r.width, r.height, 12, 5, 0x00070b12u);
    draw_rounded_panel(fb, r.x, r.y, r.width, r.height, 10, 0x0029364du, 0x00182335u, 0x00425370u, 0x00ffffffu);
    y = r.y + 12;
    for (i = 0; i < count; ++i) {
        uint32_t color = 0x00c7d3e2u;
        if (entries[i].kind == CTX_SHOW) color = 0x00edf6ffu;
        else if (entries[i].kind == CTX_QUIT_APP) color = 0x00ff9aa8u;
        else if (entries[i].kind == CTX_APP_KERNEL || entries[i].kind == CTX_APP_USER) color = 0x008fe0c8u;
        draw_text(fb, r.x + 14, y, entries[i].label, color, 1);
        y += 22;
    }
}

static int context_menu_item_at(struct desktop_state *desktop, int x, int y) {
    struct ctx_entry entries[CTX_MAX_ENTRIES];
    int count = build_context_entries(desktop, desktop->context_menu_window, entries);
    struct rect r = context_menu_rect(desktop);
    int row;

    if (!point_in_rect(x, y, r.x, r.y, r.width, r.height)) {
        return -1;
    }

    row = (y - r.y - 8) / 22;
    if (row < 0) row = 0;
    if (row >= count) return -1;
    return row;
}

static void compose_scene_rect(struct desktop_state *desktop, struct framebuffer *fb, const struct rect *rect) {
    int order;
    struct rect full_window_rect;

    if (desktop->background_dirty) {
        render_background_surface(desktop);
    }

    for (order = 0; order < WINDOW_COUNT; ++order) {
        if (desktop->window_dirty[order]) {
            render_window_surface(desktop, order);
        }
    }

    fb_blit_rect(fb, &desktop->background_fb, rect);

    for (order = 0; order < WINDOW_COUNT; ++order) {
        int index = desktop->z_order[order];
        struct window_state *window = &desktop->windows[index];

        if (!window->visible) {
            continue;
        }

        full_window_rect = window_rect(window);
        if (!rects_intersect(&full_window_rect, rect)) {
            continue;
        }

        /* Shadows are composited live (not baked into the background) so that
         * dragging a window never forces a full-screen background re-render,
         * which was the main source of flicker during motion. */
        draw_shadow_block(fb, window->x, window->y, window->width, window->height);
        blit_window_surface_region(fb, &desktop->window_fbs[index], window->x, window->y, rect);
    }

    draw_context_menu(desktop, fb);
}

void desktop_init(struct desktop_state *desktop, uint32_t screen_width, uint32_t screen_height) {
    int info_width = clamp_value((int)screen_width / 3, 430, 620);
    int info_height = clamp_value((int)screen_height / 3, 250, 320);
    int files_width = clamp_value((int)screen_width / 4, 300, 420);
    int files_height = clamp_value((int)screen_height / 3, 240, 320);
    int terminal_width = clamp_value((int)screen_width - 520, 640, 1180);
    int terminal_height = clamp_value((int)screen_height - 360, 330, 700);
    int tasks_width = clamp_value((int)screen_width / 3, 460, 640);
    int tasks_height = clamp_value((int)screen_height / 2, 340, 560);
    int right_margin = screen_width >= 1600u ? 120 : 60;
    int top_margin = screen_height >= 1000u ? 104 : 88;
    size_t i;

    serial_write("DESKTOP_INIT: screen=");
    serial_write_hex_u64(screen_width);
    serial_write("x");
    serial_write_hex_u64(screen_height);
    serial_write("\n");

    for (i = 0; i < 3; ++i) {
        struct desktop_icon icon = desktop_icon_at_size((int)i, screen_width, screen_height);
        serial_write("ICON[");
        serial_write_hex_u64(i);
        serial_write("]: x=");
        serial_write_hex_u64(icon.x);
        serial_write(" y=");
        serial_write_hex_u64(icon.y);
        serial_write(" label=");
        serial_write(icon.label);
        serial_write("\n");
    }

    desktop->screen_width = screen_width;
    desktop->screen_height = screen_height;
    /* Cohesive dark "glass" theme: all windows share a dark slate body and
     * header, each keeping its own accent hue. */
    desktop->windows[WINDOW_INFO] = (struct window_state){"SYSTEM", (screen_width >= 1920u || screen_height >= 1080u) ? 232 : 168, top_margin, info_width, info_height, 0x001a2233u, 0x00222d42u, 0x0035d0b9u, WINDOW_INFO, 1, 0, 0, 0, 0, 0};
    desktop->windows[WINDOW_FILES] = (struct window_state){"FILES", (int)screen_width - files_width - right_margin, top_margin + 36, files_width, files_height, 0x001a2233u, 0x00222d42u, 0x00f5b15au, WINDOW_FILES, 1, 0, 0, 0, 0, 0};
    desktop->windows[WINDOW_TERMINAL] = (struct window_state){"TERMINAL", clamp_value(((int)screen_width - terminal_width) / 2 + 40, 220, 340), clamp_value((int)screen_height / 5, 160, 240), terminal_width, terminal_height, 0x000e1622u, 0x00222d42u, 0x0064f2ccu, WINDOW_TERMINAL, 0, 0, 0, 0, 0, 0};
    desktop->windows[WINDOW_TASK_MANAGER] = (struct window_state){"TASK MANAGER", clamp_value((int)screen_width - tasks_width - 150, 260, 1320), clamp_value(top_margin + 92, 160, 320), tasks_width, tasks_height, 0x001a2233u, 0x00222d42u, 0x00ef7f7fu, WINDOW_TASK_MANAGER, 0, 0, 0, 0, 0, 0};
    /* Userspace app windows: hidden until an app creates them. */
    {
        int i;
        for (i = 0; i < MAX_USER_APPS; i++) {
            int win_idx = WINDOW_APP_FIRST + i;
            desktop->windows[win_idx] = (struct window_state){"APP", 320 + i * 24, 160 + i * 24, 480, 320, 0x001a2233u, 0x00222d42u, 0x008f7bf0u, (uint8_t)win_idx, 0, 0, 0, 0, 0, 0};
            desktop->user_apps[i].created = 0;
            desktop->user_apps[i].pid = 0;
            desktop->user_apps[i].content_width = 0;
            desktop->user_apps[i].content_height = 0;
            desktop->user_apps[i].event_head = 0;
            desktop->user_apps[i].event_tail = 0;
            desktop->user_apps[i].menu_count = 0;
            desktop->user_apps[i].title[0] = '\0';
        }
    }
    desktop->context_menu_open = 0;
    desktop->context_menu_x = 0;
    desktop->context_menu_y = 0;
    desktop->context_menu_window = -1;
    desktop->launcher_count = 0;
    desktop->dragging_launcher = -1;
    desktop->launcher_drag_offset_x = 0;
    desktop->launcher_drag_offset_y = 0;
    desktop->launcher_drag_start_x = 0;
    desktop->launcher_drag_start_y = 0;
    desktop->launcher_drag_moved = 0;
    desktop_refresh_launchers(desktop);

    desktop->z_order[0] = WINDOW_INFO;
    desktop->z_order[1] = WINDOW_FILES;
    desktop->z_order[2] = WINDOW_TERMINAL;
    desktop->z_order[3] = WINDOW_TASK_MANAGER;
    {
        int i;
        for (i = 0; i < MAX_USER_APPS; i++) {
            desktop->z_order[4 + i] = WINDOW_APP_FIRST + i;
        }
    }
    desktop->focused_window = WINDOW_FILES;
    desktop->dragging_window = -1;
    desktop->drag_offset_x = 0;
    desktop->drag_offset_y = 0;
    desktop->resizing_window = -1;
    desktop->resize_edges = 0;
    desktop->resize_start_mouse_x = 0;
    desktop->resize_start_mouse_y = 0;
    desktop->resize_start_x = 0;
    desktop->resize_start_y = 0;
    desktop->resize_start_width = 0;
    desktop->resize_start_height = 0;
    desktop->mouse_x = (int)(screen_width / 2u);
    desktop->mouse_y = (int)(screen_height / 2u);
    desktop->mouse_buttons = 0;
    desktop->background_dirty = 1;
    desktop->window_dirty[WINDOW_INFO] = 1;
    desktop->window_dirty[WINDOW_FILES] = 1;
    desktop->window_dirty[WINDOW_TERMINAL] = 1;
    desktop->window_dirty[WINDOW_TASK_MANAGER] = 1;
    desktop->window_dirty_rects[WINDOW_INFO] = rect_from_bounds(0, 0, desktop->windows[WINDOW_INFO].width, desktop->windows[WINDOW_INFO].height);
    desktop->window_dirty_rects[WINDOW_FILES] = rect_from_bounds(0, 0, desktop->windows[WINDOW_FILES].width, desktop->windows[WINDOW_FILES].height);
    desktop->window_dirty_rects[WINDOW_TERMINAL] = rect_from_bounds(0, 0, desktop->windows[WINDOW_TERMINAL].width, desktop->windows[WINDOW_TERMINAL].height);
    desktop->window_dirty_rects[WINDOW_TASK_MANAGER] = rect_from_bounds(0, 0, desktop->windows[WINDOW_TASK_MANAGER].width, desktop->windows[WINDOW_TASK_MANAGER].height);
    desktop->dirty = 1;
    desktop->dirty_rect = rect_from_bounds(0, 0, (int)screen_width, (int)screen_height);
    fb_init(&desktop->background_fb, (uintptr_t)desktop->background_storage, screen_width, screen_height, screen_width * 4u, 32u);
    fb_init(&desktop->window_fbs[WINDOW_INFO], (uintptr_t)surface_storage_for_window(desktop, WINDOW_INFO), desktop->windows[WINDOW_INFO].width, desktop->windows[WINDOW_INFO].height, desktop->windows[WINDOW_INFO].width * 4u, 32u);
    fb_init(&desktop->window_fbs[WINDOW_FILES], (uintptr_t)surface_storage_for_window(desktop, WINDOW_FILES), desktop->windows[WINDOW_FILES].width, desktop->windows[WINDOW_FILES].height, desktop->windows[WINDOW_FILES].width * 4u, 32u);
    fb_init(&desktop->window_fbs[WINDOW_TERMINAL], (uintptr_t)surface_storage_for_window(desktop, WINDOW_TERMINAL), desktop->windows[WINDOW_TERMINAL].width, desktop->windows[WINDOW_TERMINAL].height, desktop->windows[WINDOW_TERMINAL].width * 4u, 32u);
    fb_init(&desktop->window_fbs[WINDOW_TASK_MANAGER], (uintptr_t)surface_storage_for_window(desktop, WINDOW_TASK_MANAGER), desktop->windows[WINDOW_TASK_MANAGER].width, desktop->windows[WINDOW_TASK_MANAGER].height, desktop->windows[WINDOW_TASK_MANAGER].width * 4u, 32u);
    {
        int i;
        for (i = 0; i < MAX_USER_APPS; i++) {
            int win_idx = WINDOW_APP_FIRST + i;
            fb_init(&desktop->window_fbs[win_idx], (uintptr_t)surface_storage_for_window(desktop, win_idx), desktop->windows[win_idx].width, desktop->windows[win_idx].height, desktop->windows[win_idx].width * 4u, 32u);
        }
    }
    app_init_text(&desktop->apps[WINDOW_INFO], &desktop->app_storage[WINDOW_INFO].text, INFO_LINES, sizeof(INFO_LINES) / sizeof(INFO_LINES[0]));
    app_init_text(&desktop->apps[WINDOW_FILES], &desktop->app_storage[WINDOW_FILES].text, FILES_LINES, sizeof(FILES_LINES) / sizeof(FILES_LINES[0]));
    app_init_terminal(&desktop->apps[WINDOW_TERMINAL], &desktop->app_storage[WINDOW_TERMINAL].terminal);
    app_init_task_manager(&desktop->apps[WINDOW_TASK_MANAGER], &desktop->app_storage[WINDOW_TASK_MANAGER].task_manager, desktop);

    clamp_window_to_screen(desktop, &desktop->windows[WINDOW_INFO]);
    clamp_window_to_screen(desktop, &desktop->windows[WINDOW_FILES]);
    clamp_window_to_screen(desktop, &desktop->windows[WINDOW_TERMINAL]);
    clamp_window_to_screen(desktop, &desktop->windows[WINDOW_TASK_MANAGER]);
}

static void app_enqueue_event_slot(struct desktop_state *desktop, int slot, uint32_t type, int x, int y, uint32_t buttons, uint32_t key) {
    int next;
    if (slot < 0 || slot >= MAX_USER_APPS) return;
    next = (desktop->user_apps[slot].event_tail + 1) % WINSYS_EVENT_QUEUE;
    if (next == desktop->user_apps[slot].event_head) {
        return; /* queue full, drop */
    }
    desktop->user_apps[slot].events[desktop->user_apps[slot].event_tail].type = type;
    desktop->user_apps[slot].events[desktop->user_apps[slot].event_tail].x = x;
    desktop->user_apps[slot].events[desktop->user_apps[slot].event_tail].y = y;
    desktop->user_apps[slot].events[desktop->user_apps[slot].event_tail].buttons = buttons;
    desktop->user_apps[slot].events[desktop->user_apps[slot].event_tail].key = key;
    desktop->user_apps[slot].event_tail = next;
}


/* Content-area origin of a user-app window, in screen coordinates. */
static void app_content_origin_for_window(struct desktop_state *desktop, int win_idx, int *ox, int *oy) {
    struct window_state *w = &desktop->windows[win_idx];
    *ox = w->x + (large_ui(desktop) ? 22 : 16);
    *oy = w->y + (large_ui(desktop) ? 58 : 48);
}

static void apply_window_resize(struct desktop_state *desktop, int index, int mouse_x, int mouse_y) {
    struct window_state *window = &desktop->windows[index];
    int x = desktop->resize_start_x;
    int y = desktop->resize_start_y;
    int w = desktop->resize_start_width;
    int h = desktop->resize_start_height;
    int dx = mouse_x - desktop->resize_start_mouse_x;
    int dy = mouse_y - desktop->resize_start_mouse_y;
    int min_w = large_ui(desktop) ? 180 : 140;
    int min_h = large_ui(desktop) ? 140 : 110;
    int max_w;
    int max_h;
    int taskbar_y = (int)desktop->screen_height - ui_taskbar_height(desktop);
    struct rect before = window_rect(window);

    if (window->maximized) {
        return;
    }

    if (is_user_app_slot(index)) {
        app_window_max_size(desktop, &max_w, &max_h);
    } else {
        window_max_surface(index, &max_w, &max_h);
    }

    if (desktop->resize_edges & RESIZE_RIGHT) w += dx;
    if (desktop->resize_edges & RESIZE_BOTTOM) h += dy;
    if (desktop->resize_edges & RESIZE_LEFT) { x += dx; w -= dx; }
    if (desktop->resize_edges & RESIZE_TOP) { y += dy; h -= dy; }

    if (w < min_w) {
        if (desktop->resize_edges & RESIZE_LEFT) x -= min_w - w;
        w = min_w;
    }
    if (h < min_h) {
        if (desktop->resize_edges & RESIZE_TOP) y -= min_h - h;
        h = min_h;
    }
    if (w > max_w) {
        if (desktop->resize_edges & RESIZE_LEFT) x -= max_w - w;
        w = max_w;
    }
    if (h > max_h) {
        if (desktop->resize_edges & RESIZE_TOP) y -= max_h - h;
        h = max_h;
    }
    if (x < ui_dock_width(desktop) + 6) {
        int delta = ui_dock_width(desktop) + 6 - x;
        x += delta;
        if (desktop->resize_edges & RESIZE_LEFT) w -= delta;
    }
    if (y < 8) {
        int delta = 8 - y;
        y += delta;
        if (desktop->resize_edges & RESIZE_TOP) h -= delta;
    }
    if (x + w > (int)desktop->screen_width - 8) w = (int)desktop->screen_width - 8 - x;
    if (y + h > taskbar_y - 4) h = taskbar_y - 4 - y;
    if (w < min_w) w = min_w;
    if (h < min_h) h = min_h;

    if (window->x == x && window->y == y && window->width == w && window->height == h) {
        return;
    }

    window->x = x;
    window->y = y;
    window->width = w;
    window->height = h;
    fb_init(&desktop->window_fbs[index], (uintptr_t)surface_storage_for_window(desktop, index), w, h, w * 4u, 32u);
    if (is_user_app_slot(index) && desktop->user_apps[slot_index(index)].created) {
        update_app_content_size_slot(desktop, slot_index(index));
    }
    mark_window_dirty(desktop, index);
    mark_dirty_rect(desktop, before);
    mark_dirty_rect(desktop, window_rect(window));
}

void desktop_handle_input(struct desktop_state *desktop, const struct mouse_state *mouse, const struct keyboard_state *keyboard) {
    size_t i;
    int index;

    desktop->mouse_x = mouse->x;
    desktop->mouse_y = mouse->y;
    desktop->mouse_buttons = mouse->buttons;

    if (mouse->right_pressed) {
        for (i = 0; i < (size_t)dock_icon_count(desktop); ++i) {
            struct desktop_icon icon = dock_icon_at(desktop, (int)i);
            if (point_in_icon(mouse->x, mouse->y, &icon, ui_dock_icon_size(desktop))) {
                desktop->context_menu_open = 1;
                desktop->context_menu_x = mouse->x;
                desktop->context_menu_y = mouse->y;
                desktop->context_menu_window = icon.window_index;
                mark_dirty_rect(desktop, context_menu_rect(desktop));
                return;
            }
        }
    }

    if (desktop->resizing_window >= 0 && (mouse->buttons & 0x01u) != 0u) {
        apply_window_resize(desktop, desktop->resizing_window, mouse->x, mouse->y);
        return;
    }

    if (desktop->dragging_launcher >= 0 && (mouse->buttons & 0x01u) != 0u) {
        int li = desktop->dragging_launcher;
        int sz = ui_dock_icon_size(desktop);
        struct rect before = rect_from_bounds(desktop->launcher_x[li] - 8, desktop->launcher_y[li] - 8, sz + 80, sz + 34);
        desktop->launcher_x[li] = mouse->x - desktop->launcher_drag_offset_x;
        desktop->launcher_y[li] = mouse->y - desktop->launcher_drag_offset_y;
        if (desktop->launcher_x[li] < 8) desktop->launcher_x[li] = 8;
        if (desktop->launcher_y[li] < 48) desktop->launcher_y[li] = 48;
        if (desktop->launcher_x[li] > (int)desktop->screen_width - sz - 8) desktop->launcher_x[li] = (int)desktop->screen_width - sz - 8;
        if (desktop->launcher_y[li] > (int)desktop->screen_height - ui_taskbar_height(desktop) - sz - 8) desktop->launcher_y[li] = (int)desktop->screen_height - ui_taskbar_height(desktop) - sz - 8;
        if (desktop->launcher_x[li] != desktop->launcher_drag_start_x || desktop->launcher_y[li] != desktop->launcher_drag_start_y) {
            desktop->launcher_drag_moved = 1;
        }
        mark_background_dirty(desktop);
        mark_dirty_rect(desktop, before);
        mark_dirty_rect(desktop, rect_from_bounds(desktop->launcher_x[li] - 8, desktop->launcher_y[li] - 8, sz + 80, sz + 34));
        return;
    }

    if (desktop->dragging_window >= 0 && (mouse->buttons & 0x01u) != 0u) {
        struct window_state *window = &desktop->windows[desktop->dragging_window];
        struct rect before = window_rect(window);
        window->x = mouse->x - desktop->drag_offset_x;
        window->y = mouse->y - desktop->drag_offset_y;
        clamp_window_to_screen(desktop, window);
        mark_dirty_rect(desktop, before);
        mark_dirty_rect(desktop, window_rect(window));
    }

    if (mouse->left_pressed) {
        if (desktop->context_menu_open) {
            struct ctx_entry entries[CTX_MAX_ENTRIES];
            int item = context_menu_item_at(desktop, mouse->x, mouse->y);
            int menu_window = desktop->context_menu_window;
            struct rect menu_rect = context_menu_rect(desktop);
            int count = build_context_entries(desktop, menu_window, entries);

            desktop->context_menu_open = 0;
            mark_dirty_rect(desktop, menu_rect);

            if (item >= 0 && item < count && menu_window >= 0 && menu_window < WINDOW_COUNT) {
                struct window_state *window = &desktop->windows[menu_window];
                struct ctx_entry *e = &entries[item];

                if (e->kind == CTX_SHOW) {
                    window->visible = 1;
                    mark_window_dirty(desktop, menu_window);
                    focus_window(desktop, menu_window);
                } else if (e->kind == CTX_HIDE) {
                    struct rect before = window_rect(window);
                    window->visible = 0;
                    if (desktop->focused_window == menu_window) desktop->focused_window = -1;
                    mark_dirty_rect(desktop, before);
                } else if (e->kind == CTX_QUIT_APP) {
                    if (is_user_app_slot(menu_window)) {
                        int s = slot_index(menu_window);
                        if (desktop->user_apps[s].pid != 0) {
                            (void)process_kill(desktop->user_apps[s].pid);
                        }
                    }
                } else if (e->kind == CTX_APP_KERNEL) {
                    /* App does its own work (e.g. spawn a fresh shell); the
                     * compositor surfaces the window so the result is visible. */
                    app_menu_action(&desktop->apps[window->app_slot], e->action);
                    window->visible = 1;
                    mark_window_dirty(desktop, menu_window);
                    focus_window(desktop, menu_window);
                } else if (e->kind == CTX_APP_USER) {
                    if (is_user_app_slot(menu_window)) {
                        app_enqueue_event_slot(desktop, slot_index(menu_window), WINSYS_EVENT_MENU_ACTION, 0, 0, 0, e->action);
                    }
                }
                mark_dock_dirty(desktop);
            }
            return;
        }

        index = topmost_window_at(desktop, mouse->x, mouse->y);
        if (index >= 0) {
            struct window_state *window = &desktop->windows[index];
            struct rect before = window_rect(window);
            int btn;
            int edges;

            focus_window(desktop, index);

            btn = window_button_hit(desktop, window, mouse->x, mouse->y);
            if (btn == WIN_BTN_CLOSE || btn == WIN_BTN_MIN) {
                if (btn == WIN_BTN_CLOSE && is_user_app_slot(index) && desktop->user_apps[slot_index(index)].created) {
                    app_enqueue_event_slot(desktop, slot_index(index), WINSYS_EVENT_CLOSE, 0, 0, 0, 0);
                }
                /* Minimise and close both hide the window; reopen from its dock
                 * icon. Repaint the dock so the running-dot updates. */
                window->visible = 0;
                if (desktop->focused_window == index) {
                    desktop->focused_window = -1;
                }
                mark_dock_dirty(desktop);
                mark_dirty_rect(desktop, before);
            } else if (btn == WIN_BTN_MAX) {
                window_toggle_maximize(desktop, index);
                mark_window_dirty(desktop, index);
                mark_dirty_rect(desktop, before);
                mark_dirty_rect(desktop, window_rect(window));
            } else if ((edges = resize_hit(desktop, window, mouse->x, mouse->y)) != 0) {
                desktop->resizing_window = index;
                desktop->resize_edges = edges;
                desktop->resize_start_mouse_x = mouse->x;
                desktop->resize_start_mouse_y = mouse->y;
                desktop->resize_start_x = window->x;
                desktop->resize_start_y = window->y;
                desktop->resize_start_width = window->width;
                desktop->resize_start_height = window->height;
                mark_dirty_rect(desktop, window_rect(window));
            } else if (titlebar_hit(desktop, window, mouse->x, mouse->y)) {
                desktop->dragging_window = index;
                desktop->drag_offset_x = mouse->x - window->x;
                desktop->drag_offset_y = mouse->y - window->y;
                mark_dirty_rect(desktop, window_rect(window));
            }
        } else {
            for (i = 0; i < (size_t)desktop->launcher_count; ++i) {
                struct desktop_icon icon = launcher_icon_at(desktop, (int)i);
                if (point_in_icon(mouse->x, mouse->y, &icon, ui_dock_icon_size(desktop))) {
                    desktop->dragging_launcher = (int)i;
                    desktop->launcher_drag_offset_x = mouse->x - icon.x;
                    desktop->launcher_drag_offset_y = mouse->y - icon.y;
                    desktop->launcher_drag_start_x = icon.x;
                    desktop->launcher_drag_start_y = icon.y;
                    desktop->launcher_drag_moved = 0;
                    return;
                }
            }

            for (i = 0; i < (size_t)dock_icon_count(desktop); ++i) {
                struct desktop_icon icon = dock_icon_at(desktop, (int)i);

                if (point_in_icon(mouse->x, mouse->y, &icon, ui_dock_icon_size(desktop))) {
                    struct window_state *window = &desktop->windows[icon.window_index];
                    window->visible = 1;
                    mark_window_dirty(desktop, icon.window_index);
                    mark_dock_dirty(desktop);
                    focus_window(desktop, icon.window_index);
                    if (icon.window_index < DESKTOP_ICON_COUNT) {
                        app_activate(&desktop->apps[window->app_slot]);
                    }
                    break;
                }
            }
        }
    }

    if (mouse->left_released) {
        if (desktop->dragging_launcher >= 0) {
            int li = desktop->dragging_launcher;
            if (desktop->launcher_drag_moved) {
                persist_launcher(desktop, li);
                mark_background_dirty(desktop);
                mark_dirty_rect(desktop, rect_from_bounds(0, 0, (int)desktop->screen_width, (int)desktop->screen_height));
            } else {
                (void)process_spawn_path(desktop->launcher_execs[li], 0, 0);
            }
        }
        desktop->dragging_launcher = -1;
        desktop->launcher_drag_moved = 0;
        desktop->dragging_window = -1;
        desktop->resizing_window = -1;
        desktop->resize_edges = 0;
    }

    /* Deliver input to a focused userspace app window via its event queue. */
    if (is_user_app_slot(desktop->focused_window) &&
        desktop->windows[desktop->focused_window].visible &&
        desktop->dragging_window < 0 && desktop->resizing_window < 0) {
        int fw = desktop->focused_window;
        int s = slot_index(fw);
        int ox;
        int oy;
        size_t k;
        app_content_origin_for_window(desktop, fw, &ox, &oy);
        if (mouse->moved) {
            app_enqueue_event_slot(desktop, s, WINSYS_EVENT_MOUSE_MOVE, mouse->x - ox, mouse->y - oy, mouse->buttons, 0);
        }
        if (mouse->wheel != 0) {
            app_enqueue_event_slot(desktop, s, WINSYS_EVENT_SCROLL, 0, mouse->wheel, 0, 0);
        }
        if (mouse->left_pressed) {
            app_enqueue_event_slot(desktop, s, WINSYS_EVENT_MOUSE_DOWN, mouse->x - ox, mouse->y - oy, 1, 0);
        }
        if (mouse->left_released) {
            app_enqueue_event_slot(desktop, s, WINSYS_EVENT_MOUSE_UP, mouse->x - ox, mouse->y - oy, 0, 0);
        }
        if (mouse->right_pressed) {
            app_enqueue_event_slot(desktop, s, WINSYS_EVENT_CONTEXT_MENU, mouse->x - ox, mouse->y - oy, 2, 0);
        }
        if (mouse->right_released) {
            app_enqueue_event_slot(desktop, s, WINSYS_EVENT_MOUSE_UP, mouse->x - ox, mouse->y - oy, 0, 0);
        }
        for (k = 0; k < keyboard->count; ++k) {
            app_enqueue_event_slot(desktop, s, WINSYS_EVENT_KEY, 0, 0, 0, (uint32_t)(uint8_t)keyboard->chars[k]);
        }
        if (keyboard->enter_pressed) {
            app_enqueue_event_slot(desktop, s, WINSYS_EVENT_KEY, 0, 0, 0, (uint32_t)'\n');
        }
        if (keyboard->backspace_pressed) {
            app_enqueue_event_slot(desktop, s, WINSYS_EVENT_KEY, 0, 0, 0, 0x08u);
        }
        return;
    }

    if (desktop->focused_window >= 0 && desktop->focused_window < DESKTOP_ICON_COUNT && desktop->windows[desktop->focused_window].visible) {
        if (app_handle_keyboard(&desktop->apps[desktop->windows[desktop->focused_window].app_slot], keyboard)) {
            struct app_draw_context app_ctx;
            struct rect damage_rect;
            struct window_state *window = &desktop->windows[desktop->focused_window];
            struct window_state local_window = *window;

            local_window.x = 0;
            local_window.y = 0;
            build_app_draw_context(desktop, &local_window, 1, &app_ctx);
            if (app_consume_damage(&desktop->apps[desktop->windows[desktop->focused_window].app_slot], &app_ctx, &damage_rect)) {
                mark_window_dirty_region(desktop, desktop->focused_window, damage_rect);
                mark_dirty_rect(desktop, surface_rect_to_screen(window, &damage_rect));
            } else {
                mark_window_dirty(desktop, desktop->focused_window);
                mark_dirty_rect(desktop, window_rect(window));
            }
        }
    }
}

void desktop_poll_apps(struct desktop_state *desktop) {
	size_t i;

	/* A kernel app whose owning process (e.g. the terminal's /bin/sh) has died
	 * gets its window cleanly closed here. The app drops its session state and
	 * only restarts on an explicit user gesture (dock click / context menu). */
	for (i = 0; i < DESKTOP_ICON_COUNT; ++i) {
		struct window_state *window = &desktop->windows[i];
		uint32_t owner = app_window_owner_pid(&desktop->apps[window->app_slot]);

		if (owner != 0 && !process_pid_alive(owner)) {
			struct rect before = window_rect(window);
			if (window->visible) {
				window->visible = 0;
				if (desktop->focused_window == (int)i) {
					desktop->focused_window = -1;
				}
				mark_dirty_rect(desktop, before);
				mark_dock_dirty(desktop);
			}
			app_window_closed(&desktop->apps[window->app_slot]);
		}
	}

	/* Only the built-in kernel apps (windows 0..3) redraw themselves here;
	 * the userspace app window (WINDOW_APP) is updated via SYS_WINDOW_PRESENT. */
	for (i = 0; i < DESKTOP_ICON_COUNT; ++i) {
		const struct window_state *window = &desktop->windows[i];

		if (!window->visible) {
			continue;
		}

	if (app_needs_redraw(&desktop->apps[window->app_slot])) {
		struct app_draw_context app_ctx;
		struct rect damage_rect;
		struct window_state local_window = *window;

		local_window.x = 0;
		local_window.y = 0;
		build_app_draw_context(desktop, &local_window, desktop->focused_window == (int)i, &app_ctx);
		if (app_consume_damage(&desktop->apps[window->app_slot], &app_ctx, &damage_rect)) {
			mark_window_dirty_region(desktop, (int)i, damage_rect);
			mark_dirty_rect(desktop, surface_rect_to_screen(window, &damage_rect));
		} else {
			mark_window_dirty(desktop, (int)i);
			mark_dirty_rect(desktop, window_rect(window));
		}
	}
	}
}

void desktop_render(struct desktop_state *desktop, struct framebuffer *fb) {
    struct rect rect = rect_from_bounds(0, 0, (int)desktop->screen_width, (int)desktop->screen_height);
    fb_reset_clip(fb);
    compose_scene_rect(desktop, fb, &rect);
}

void desktop_render_rect(struct desktop_state *desktop, struct framebuffer *fb, const struct rect *rect) {
    fb_set_clip(fb, rect);
    compose_scene_rect(desktop, fb, rect);
    fb_reset_clip(fb);
}

void desktop_draw_cursor_overlay(const struct desktop_state *desktop, struct framebuffer *fb) {
    draw_cursor((struct desktop_state *)desktop, fb, desktop->mouse_x, desktop->mouse_y);
}

void desktop_cursor_rect_at(const struct desktop_state *desktop, int x, int y, struct rect *rect) {
    *rect = cursor_rect(desktop, x, y);
}

int desktop_take_dirty_rect(struct desktop_state *desktop, struct rect *rect) {
    if (!desktop->dirty) {
        return 0;
    }

    *rect = desktop->dirty_rect;
    desktop->dirty = 0;
    desktop->dirty_rect = rect_from_bounds(0, 0, 0, 0);
    return 1;
}

/* ---- Window server (userspace GUI apps) ---- */

int desktop_app_create(struct desktop_state *desktop, uint32_t pid, const char *title, int width, int height) {
    int slot;
    int win_idx;
    struct window_state *w;
    int frame_w;
    int frame_h;
    size_t i;

    if (width < 80) width = 80;
    if (height < 60) height = 60;
    if (width > (int)WINDOW_APP_CONTENT_MAX_WIDTH) width = (int)WINDOW_APP_CONTENT_MAX_WIDTH;
    if (height > (int)WINDOW_APP_CONTENT_MAX_HEIGHT) height = (int)WINDOW_APP_CONTENT_MAX_HEIGHT;

    /* Reuse existing slot if this PID already has one; else find free slot. */
    slot = find_slot_by_pid(desktop, pid);
    if (slot < 0) slot = find_free_slot(desktop);
    win_idx = WINDOW_APP_FIRST + slot;
    w = &desktop->windows[win_idx];

    /* Window size = content size + frame insets (matches build_app_draw_context). */
    frame_w = width + (large_ui(desktop) ? 44 : 32);
    frame_h = height + (large_ui(desktop) ? 82 : 66);

    for (i = 0; i + 1 < sizeof(desktop->user_apps[slot].title) && title && title[i]; ++i) {
        desktop->user_apps[slot].title[i] = title[i];
    }
    desktop->user_apps[slot].title[(title) ? i : 0] = '\0';

    w->title = desktop->user_apps[slot].title;
    w->width = frame_w;
    w->height = frame_h;
    w->x = (int)desktop->screen_width / 2 - frame_w / 2 + slot * 24;
    w->y = (int)desktop->screen_height / 2 - frame_h / 2 + slot * 24;
    w->visible = 1;
    w->maximized = 0;
    if (w->x < ui_dock_width(desktop) + 16) w->x = ui_dock_width(desktop) + 16;
    if (w->y < 16) w->y = 16;

    desktop->user_apps[slot].created = 1;
    desktop->user_apps[slot].pid = pid;
    desktop->user_apps[slot].content_width = width;
    desktop->user_apps[slot].content_height = height;
    desktop->user_apps[slot].event_head = 0;
    desktop->user_apps[slot].event_tail = 0;
    desktop->user_apps[slot].menu_count = 0;

    fb_init(&desktop->window_fbs[win_idx], (uintptr_t)desktop->user_apps[slot].surface_storage, w->width, w->height, w->width * 4u, 32u);

    focus_window(desktop, win_idx);
    mark_dock_dirty(desktop);
    mark_window_dirty(desktop, win_idx);
    mark_dirty_rect(desktop, window_rect(w));
    return win_idx;
}

int desktop_app_present(struct desktop_state *desktop, uint32_t pid, int win_id, const uint32_t *src, int src_w, int src_h) {
    int slot;
    int win_idx;
    int row;
    int copy_w;
    int copy_h;

    if (src == 0) return -1;

    /* Accept win_id as the window index, or fall back to PID lookup. */
    if (is_user_app_slot(win_id)) {
        slot = slot_index(win_id);
        if (slot < 0 || slot >= MAX_USER_APPS) return -1;
        if (!desktop->user_apps[slot].created || desktop->user_apps[slot].pid != pid) return -1;
        win_idx = win_id;
    } else {
        slot = find_slot_by_pid(desktop, pid);
        if (slot < 0) return -1;
        win_idx = WINDOW_APP_FIRST + slot;
    }

    copy_w = src_w < desktop->user_apps[slot].content_width ? src_w : desktop->user_apps[slot].content_width;
    copy_h = src_h < desktop->user_apps[slot].content_height ? src_h : desktop->user_apps[slot].content_height;
    if (copy_w > (int)WINDOW_APP_CONTENT_MAX_WIDTH) copy_w = (int)WINDOW_APP_CONTENT_MAX_WIDTH;
    if (copy_h > (int)WINDOW_APP_CONTENT_MAX_HEIGHT) copy_h = (int)WINDOW_APP_CONTENT_MAX_HEIGHT;

    for (row = 0; row < copy_h; ++row) {
        uint32_t *dst = &desktop->user_apps[slot].content_storage[(size_t)row * (size_t)WINDOW_APP_CONTENT_MAX_WIDTH];
        const uint32_t *s = &src[(size_t)row * (size_t)src_w];
        int col;
        for (col = 0; col < copy_w; ++col) {
            dst[col] = s[col];
        }
    }

    mark_window_dirty(desktop, win_idx);
    mark_dirty_rect(desktop, window_rect(&desktop->windows[win_idx]));
    return 0;
}

int desktop_app_poll_event(struct desktop_state *desktop, uint32_t pid, int win_id, struct winsys_event *out) {
    int slot;

    if (out == 0) return 0;

    if (is_user_app_slot(win_id)) {
        slot = slot_index(win_id);
        if (slot < 0 || slot >= MAX_USER_APPS) return 0;
        if (!desktop->user_apps[slot].created || desktop->user_apps[slot].pid != pid) return 0;
    } else {
        slot = find_slot_by_pid(desktop, pid);
        if (slot < 0) return 0;
    }

    if (desktop->user_apps[slot].event_head == desktop->user_apps[slot].event_tail) {
        return 0; /* empty */
    }
    *out = desktop->user_apps[slot].events[desktop->user_apps[slot].event_head];
    desktop->user_apps[slot].event_head = (desktop->user_apps[slot].event_head + 1) % WINSYS_EVENT_QUEUE;
    return 1;
}

int desktop_app_set_menu(struct desktop_state *desktop, uint32_t pid, int win_id, const struct winsys_menu_item *items, int count) {
    int slot;
    int i;

    if (desktop == 0) return -1;

    if (is_user_app_slot(win_id)) {
        slot = slot_index(win_id);
        if (slot < 0 || slot >= MAX_USER_APPS) return -1;
        if (!desktop->user_apps[slot].created || desktop->user_apps[slot].pid != pid) return -1;
    } else {
        slot = find_slot_by_pid(desktop, pid);
        if (slot < 0) return -1;
    }

    if (count < 0) count = 0;
    if (count > WINSYS_MAX_MENU_ITEMS) count = WINSYS_MAX_MENU_ITEMS;
    if (count > 0 && items == 0) {
        return -1;
    }

    for (i = 0; i < count; ++i) {
        int j;
        for (j = 0; j + 1 < WINSYS_MENU_LABEL_MAX && items[i].label[j]; ++j) {
            desktop->user_apps[slot].menu[i].label[j] = items[i].label[j];
        }
        desktop->user_apps[slot].menu[i].label[j] = '\0';
        desktop->user_apps[slot].menu[i].action_id = items[i].action_id;
    }
    desktop->user_apps[slot].menu_count = count;
    return 0;
}

void desktop_app_close_for_pid(struct desktop_state *desktop, uint32_t pid) {
    int slot;
    int win_idx;
    struct window_state *w;
    struct rect before;
    struct rect old_dock;

    if (desktop == 0) return;

    slot = find_slot_by_pid(desktop, pid);
    if (slot < 0) return;

    win_idx = WINDOW_APP_FIRST + slot;
    w = &desktop->windows[win_idx];
    before = window_rect(w);
    old_dock = dock_dirty_rect(desktop);
    w->visible = 0;
    desktop->user_apps[slot].created = 0;
    desktop->user_apps[slot].pid = 0;
    desktop->user_apps[slot].content_width = 0;
    desktop->user_apps[slot].content_height = 0;
    desktop->user_apps[slot].event_head = 0;
    desktop->user_apps[slot].event_tail = 0;
    desktop->user_apps[slot].menu_count = 0;
    if (desktop->focused_window == win_idx) {
        desktop->focused_window = -1;
    }
    mark_dirty_rect(desktop, old_dock);
    mark_dock_dirty(desktop);
    mark_dirty_rect(desktop, before);
}
