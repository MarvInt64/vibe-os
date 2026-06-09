/*
 * MIT License
 *
 * Copyright (c) 2026 Marvin Kicha
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "window.h"
#include "alloc.h"
#include "journal.h"
#include "process.h"
#include "render.h"
#include "serial.h"
#include "string.h"
#include "timer.h"
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

#define WINDOW_SHADOW_SPREAD 6

static struct desktop_icon launcher_icon_at(const struct desktop_state *desktop, int index);

/* Legacy transparent color key. Newer app surfaces may also use ARGB alpha in
 * the high byte; keeping the key preserves old 0x00RRGGBB callers. */
#define WINDOW_TRANSPARENT_KEY 0x00ff00ffu

struct desktop_theme {
    uint32_t bg;
    uint32_t surface;
    uint32_t surface_hi;
    uint32_t border;
    uint32_t border_hi;
    uint32_t text;
    uint32_t text_dim;
    uint32_t accent;
    uint32_t ok;
    uint32_t warn;
    uint32_t danger;
    /* Menu / dropdown surfaces (top-bar dropdowns + context menus). */
    uint32_t menu_bg;
    uint32_t menu_item;
    uint32_t menu_muted;
    /* Opacity (0–255) applied when compositing window surfaces. 255 = opaque;
     * lower values let the wallpaper show through for a glass look. */
    uint32_t window_alpha;
};

static struct desktop_theme g_chrome_theme = {
    /* Palette from the VibeOS design spec (blue-gray glass, cyan-blue accent).
       bg          surface      surface_hi   border */
    0x0015273cu, 0x001b3048u, 0x00233850u, 0x0039506au,
    /* border_hi   text         text_dim     accent */
    0x005a7da3u, 0x00eaf2fau, 0x00b7c7d8u, 0x004da3ffu,
    /* ok          warn         danger */
    0x0063d9a5u, 0x00e6b65cu, 0x00e36c7au,
    /* menu_bg     menu_item    menu_muted */
    0x000c1b2au, 0x00d6e3efu, 0x007f93a8u,
    /* window_alpha: slight glass translucency for app windows */
    243u
};

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

static uint32_t backdrop_noise(int x, int y) {
    uint32_t n = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
    n = (n ^ (n >> 13)) * 1274126177u;
    return (n ^ (n >> 16)) & 0xffu;
}

static int large_ui(const struct desktop_state *desktop) {
	return desktop->screen_width >= 1920u || desktop->screen_height >= 1080u;
}

static int window_frameless(const struct window_state *window) {
    return (window->flags & WINSYS_WINDOW_FRAMELESS) != 0u;
}

static int window_always_on_top(const struct window_state *window) {
    return (window->flags & WINSYS_WINDOW_ALWAYS_ON_TOP) != 0u;
}

static int ui_titlebar_height(const struct desktop_state *desktop) {
    return large_ui(desktop) ? 38 : 30;
}

/* Work-area reservation for the userspace shell dock. The dock itself is no
 * longer rendered by the kernel; this keeps maximized/initial windows from
 * covering the always-on-top dock app until shell-owned work-area hints exist. */
static int ui_taskbar_height(const struct desktop_state *desktop) {
    return large_ui(desktop) ? 104 : 80;
}

static int ui_left_work_area_inset(const struct desktop_state *desktop) {
    (void)desktop;
    return 12;
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

static int parse_decimal(const char *s, int fallback);

static void desktop_theme_apply_color(const char *data, const char *key, uint32_t *slot) {
    char value[16];

    if (line_value(data, key, value, sizeof(value))) {
        *slot = parse_hex_color(value, *slot);
    }
}

/* Resolve a user's home directory from /etc/passwd.  Returns the length
 * written to buf (excluding NUL), or 0 on failure — in which case buf is
 * set to "/" as a safe fallback.  Used so the desktop follows whoever
 * started the GUI, rather than hardcoding /home/user. */
static int resolve_user_home(char *buf, size_t cap, uint32_t uid) {
    char data[1024];
    if (buf == 0 || cap < 2) return 0;
    buf[0] = '\0';

    ssize_t n = vfs_read("/etc/passwd", 0, data, sizeof(data) - 1u);
    if (n <= 0) { copy_text(buf, cap, "/"); return 1; }
    data[n] = '\0';

    /* Walk lines: name:x:uid:gid:gecos:home:shell */
    char *line = data;
    while (*line) {
        char *nl = line;
        while (*nl && *nl != '\n') ++nl;
        int eol = (*nl == '\n');
        *nl = '\0';

        /* Skip to field 2 (uid), counting colons manually. */
        char *f = line;
        for (int col = 0; col < 2; ++col) {
            while (*f && *f != ':') ++f;
            if (!*f) goto next_line;
            ++f;
        }
        /* Parse uid. */
        uint32_t line_uid = 0;
        while (*f >= '0' && *f <= '9') {
            line_uid = line_uid * 10u + (uint32_t)(*f - '0');
            ++f;
        }
        if (*f != ':') goto next_line;

        if (line_uid == uid) {
            /* Found the user — field 5 is home (skip 3 more colons). */
            for (int col = 0; col < 3; ++col) {
                while (*f && *f != ':') ++f;
                if (!*f) goto next_line;
                ++f;
            }
            /* Copy home field. */
            size_t i = 0;
            while (*f && *f != ':' && *f != '\n' && i < cap - 1)
                buf[i++] = *f++;
            buf[i] = '\0';
            return (int)i;
        }

next_line:
        line = nl + (eol ? 1 : 0);
    }

    /* uid not found — fallback. */
    copy_text(buf, cap, uid == 0 ? "/root" : "/");
    return (int)strlen(buf);
}

static void desktop_theme_load(void) {
    char data[1024];
    char home[64];
    char theme_path[96];
    resolve_user_home(home, sizeof(home), g_desktop_uid);
    copy_text(theme_path, sizeof(theme_path), home);
    append_text(theme_path, sizeof(theme_path), "/.config/vibeos.theme");
    ssize_t n = vfs_read(theme_path, 0, data, sizeof(data) - 1u);

    if (n <= 0) return;
    data[n] = '\0';

    desktop_theme_apply_color(data, "bg", &g_chrome_theme.bg);
    desktop_theme_apply_color(data, "surface", &g_chrome_theme.surface);
    desktop_theme_apply_color(data, "surface_hi", &g_chrome_theme.surface_hi);
    desktop_theme_apply_color(data, "border", &g_chrome_theme.border);
    desktop_theme_apply_color(data, "border_hi", &g_chrome_theme.border_hi);
    desktop_theme_apply_color(data, "text", &g_chrome_theme.text);
    desktop_theme_apply_color(data, "text_dim", &g_chrome_theme.text_dim);
    desktop_theme_apply_color(data, "accent", &g_chrome_theme.accent);
    desktop_theme_apply_color(data, "ok", &g_chrome_theme.ok);
    desktop_theme_apply_color(data, "warn", &g_chrome_theme.warn);
    desktop_theme_apply_color(data, "danger", &g_chrome_theme.danger);
    desktop_theme_apply_color(data, "menu_bg", &g_chrome_theme.menu_bg);
    desktop_theme_apply_color(data, "menu_item", &g_chrome_theme.menu_item);
    desktop_theme_apply_color(data, "menu_muted", &g_chrome_theme.menu_muted);
    {
        char value[16];
        if (line_value(data, "window_alpha", value, sizeof(value))) {
            int a = parse_decimal(value, (int)g_chrome_theme.window_alpha);
            if (a < 0) a = 0; if (a > 255) a = 255;
            g_chrome_theme.window_alpha = (uint32_t)a;
        }
    }
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
    char home[64], desktop_dir[96];
    desktop->launcher_count = 0;

    resolve_user_home(home, sizeof(home), g_desktop_uid);
    copy_text(desktop_dir, sizeof(desktop_dir), home);
    append_text(desktop_dir, sizeof(desktop_dir), "/Desktop");

    if (!vfs_stat_path(home, &st)) (void)vfs_mkdir(home);
    if (!vfs_stat_path(desktop_dir, &st)) (void)vfs_mkdir(desktop_dir);

    /* Also ensure ~/.config exists so the theme file and other per-user
     * configuration can be written without the user creating the directory
     * by hand. */
    {
        char config_dir[96];
        copy_text(config_dir, sizeof(config_dir), home);
        append_text(config_dir, sizeof(config_dir), "/.config");
        if (!vfs_stat_path(config_dir, &st)) (void)vfs_mkdir(config_dir);
    }

    /* Seed default launcher icons only if the Desktop directory is empty.
     * No desktop shortcuts are created by default — the user adds them via
     * the file browser or terminal. */

    while (desktop->launcher_count < DESKTOP_LAUNCHER_MAX) {
        struct vfs_dir_entry entry;
        char path[128];
        char data[512];
        ssize_t n;
        char color[16];
        int li;

        if (!vfs_readdir(desktop_dir, index++, &entry)) break;
        if (entry.kind != VFS_NODE_FILE || !text_ends_with(entry.name, ".desktop")) continue;
        if (strcmp(entry.name, "Dock.desktop") == 0) continue;

        copy_text(path, sizeof(path), desktop_dir);
        append_text(path, sizeof(path), "/");
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

static int ui_launcher_icon_size(const struct desktop_state *desktop) { return large_ui(desktop) ? 52 : 40; }

static struct desktop_icon launcher_icon_at(const struct desktop_state *desktop, int index) {
    struct desktop_icon icon;
    int sz = ui_launcher_icon_size(desktop);
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

static void copy_capped(char *dst, size_t cap, const char *src) {
    size_t i = 0;
    if (cap == 0) return;
    if (src != 0) {
        while (i + 1 < cap && src[i] != '\0') {
            dst[i] = src[i];
            ++i;
        }
    }
    dst[i] = '\0';
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
    clip_rect_to_screen(desktop, &rect);
    if (rect_is_empty(&rect)) {
        return;
    }
    
    int start_tile_x = rect.x / desktop->tile_size;
    int end_tile_x = (rect.x + rect.width - 1) / desktop->tile_size;
    int start_tile_y = rect.y / desktop->tile_size;
    int end_tile_y = (rect.y + rect.height - 1) / desktop->tile_size;
    
    for (int y = start_tile_y; y <= end_tile_y; y++) {
        for (int x = start_tile_x; x <= end_tile_x; x++) {
            if (x < 0 || y < 0 || x >= desktop->tiles_x || y >= desktop->tiles_y) continue;
            int tile_idx = y * desktop->tiles_x + x;
            if (tile_idx / 8 < MAX_TILE_BIT_ARRAY) {
                desktop->dirty_tiles[tile_idx / 8] |= (1 << (tile_idx % 8));
                desktop->dirty = 1;
            }
        }
    }
}

static void mark_background_dirty(struct desktop_state *desktop) {
    desktop->background_dirty = 1;
    for(int i = 0; i < MAX_TILE_BIT_ARRAY; i++) desktop->dirty_tiles[i] = 0xFF;
    desktop->dirty = 1;
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
    if (window_frameless(window)) {
        app_ctx->content_x = 0;
        app_ctx->content_y = 0;
        app_ctx->content_width = window->width;
        app_ctx->content_height = window->height;
    } else {
        app_ctx->content_x = large_ui(desktop) ? 22 : 16;
        app_ctx->content_y = large_ui(desktop) ? 58 : 48;
        app_ctx->content_width = window->width - (large_ui(desktop) ? 44 : 32);
        app_ctx->content_height = window->height - (large_ui(desktop) ? 82 : 66);
    }
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
    return rect_from_bounds(x - 1, y - 1, 14, size);
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
static int ensure_surface_buffer(struct desktop_state *desktop, int slot, int w, int h);
static int ensure_content_buffer(struct desktop_state *desktop, int slot, int w, int h, uint32_t fill);
static void reflow_content_buffer(struct desktop_state *desktop, int slot, int old_w, int old_h, int new_w, int new_h, uint32_t fill);

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

static void normalize_z_order(struct desktop_state *desktop) {
    int next[WINDOW_COUNT];
    int n = 0;
    int order;

    for (order = 0; order < WINDOW_COUNT; ++order) {
        int index = desktop->z_order[order];
        if (index >= 0 && index < WINDOW_COUNT && !window_always_on_top(&desktop->windows[index])) {
            next[n++] = index;
        }
    }
    for (order = 0; order < WINDOW_COUNT; ++order) {
        int index = desktop->z_order[order];
        if (index >= 0 && index < WINDOW_COUNT && window_always_on_top(&desktop->windows[index])) {
            next[n++] = index;
        }
    }
    for (order = 0; order < WINDOW_COUNT && order < n; ++order) {
        desktop->z_order[order] = next[order];
    }
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

    if (window_frameless(window)) return WIN_BTN_NONE;

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
    if (window_frameless(window)) return 0;
    return point_in_rect(px, py, window->x, window->y, window->width, ui_titlebar_height(desktop));
}

static int resize_hit(const struct desktop_state *desktop, const struct window_state *window, int px, int py) {
    int edge_grip = large_ui(desktop) ? 12 : 9;
    int corner_grip = large_ui(desktop) ? 26 : 20;
    int edges = 0;

    if (window_frameless(window) || !point_in_rect(px, py, window->x, window->y, window->width, window->height)) {
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
    if (window_frameless(window)) {
        *cw = window->width;
        *ch = window->height;
        return;
    }
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
    int old_w;
    int old_h;
    app_content_size_for_window(desktop, &desktop->windows[win_idx], &cw, &ch);
    if (cw > (int)WINDOW_APP_CONTENT_MAX_WIDTH) cw = (int)WINDOW_APP_CONTENT_MAX_WIDTH;
    if (ch > (int)WINDOW_APP_CONTENT_MAX_HEIGHT) ch = (int)WINDOW_APP_CONTENT_MAX_HEIGHT;
    if (desktop->user_apps[slot].content_width != cw || desktop->user_apps[slot].content_height != ch) {
        uint32_t fill = (desktop->windows[win_idx].flags & WINSYS_WINDOW_TRANSLUCENT)
                      ? WINDOW_TRANSPARENT_KEY : g_chrome_theme.bg;
        old_w = desktop->user_apps[slot].content_width;
        old_h = desktop->user_apps[slot].content_height;
        /* Grow the backing store first; only adopt the new size if it fit. */
        if (!ensure_content_buffer(desktop, slot, cw, ch, fill)) return;
        reflow_content_buffer(desktop, slot, old_w, old_h, cw, ch, fill);
        desktop->user_apps[slot].content_width = cw;
        desktop->user_apps[slot].content_height = ch;
        app_enqueue_event_slot(desktop, slot, WINSYS_EVENT_RESIZE, cw, ch, 0, 0);
    }
}

static void clamp_window_to_screen(struct desktop_state *desktop, struct window_state *window) {
    /* Allow windows to be dragged close to the screen edge. Initial placement
     * still avoids the shell work area. */
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
    /* Track the last focused real app window so the top bar keeps showing its
     * menu even when a shell panel (dock / top bar) is clicked. Shell panels
     * are frameless NO_DOCK windows. */
    if (!((desktop->windows[index].flags & WINSYS_WINDOW_NO_DOCK) &&
          window_frameless(&desktop->windows[index]))) {
        desktop->menu_target_window = index;
    }
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
    normalize_z_order(desktop);

    if (previous_focus >= 0 && previous_focus < WINDOW_COUNT && previous_focus != index) {
        mark_window_dirty(desktop, previous_focus);
        mark_dirty_rect(desktop, window_rect(&desktop->windows[previous_focus]));
    }
    if (previous_focus != index) {
        mark_window_dirty(desktop, index);
        mark_dirty_rect(desktop, window_rect(&desktop->windows[index]));
    }
}

/* A frameless app window's pixels are transparent where its presented content
 * holds the transparent key. Clicks on those pixels fall through to the window
 * below — this lets a mostly-transparent overlay (the top bar, the dock) cover
 * a large area while only its opaque parts are interactive. */
static int window_pixel_transparent(struct desktop_state *desktop, int index, int px, int py) {
    struct window_state *window = &desktop->windows[index];
    int s, lx, ly;
    if (!is_user_app_slot(index) || !window_frameless(window)) return 0;
    s = slot_index(index);
    if (s < 0) return 0;
    lx = px - window->x;
    ly = py - window->y;
    if (lx < 0 || ly < 0 ||
        lx >= desktop->user_apps[s].content_width ||
        ly >= desktop->user_apps[s].content_height ||
        desktop->user_apps[s].content_storage == 0) {
        return 1;   /* outside presented content = nothing drawn there */
    }
    return desktop->user_apps[s].content_storage[(size_t)ly * desktop->user_apps[s].content_width + lx]
           == WINDOW_TRANSPARENT_KEY;
}

static int topmost_window_at(struct desktop_state *desktop, int px, int py) {
    int order;

    for (order = WINDOW_COUNT - 1; order >= 0; --order) {
        int index = desktop->z_order[order];
        struct window_state *window = &desktop->windows[index];

        if (window->visible && point_in_rect(px, py, window->x, window->y, window->width, window->height) &&
            !window_pixel_transparent(desktop, index, px, py)) {
            return index;
        }
    }

    return -1;
}

static void draw_window_frame(struct desktop_state *desktop, struct framebuffer *fb, const struct window_state *window, int focused) {
    int titlebar_height = ui_titlebar_height(desktop);
    int button_y = window->y + (large_ui(desktop) ? 13 : 10);
    int button_size = large_ui(desktop) ? 14 : 11;
    int button_gap = large_ui(desktop) ? 24 : 18;
    int icon_size = large_ui(desktop) ? 22 : 18;
    int icon_x = window->x + (large_ui(desktop) ? 16 : 12);
    int icon_y = window->y + (titlebar_height - icon_size) / 2;
    int x_close = window->x + window->width - button_gap - (large_ui(desktop) ? 4 : 0);
    int x_max = window->x + window->width - (button_gap * 2) - (large_ui(desktop) ? 6 : 0);
    int x_min = window->x + window->width - (button_gap * 3) - (large_ui(desktop) ? 8 : 0);
    uint32_t border = focused ? g_chrome_theme.border_hi : g_chrome_theme.border;
    /* Flat, modern chrome: a single body colour and a single (slightly lighter)
     * title colour — no vertical gradient ("dark wave") and no glossy top band.
     * Passing highlight == fill suppresses draw_rounded_panel's gloss highlight. */
    uint32_t body_fill  = g_chrome_theme.surface;
    uint32_t title_fill = g_chrome_theme.surface_hi;
    int radius = large_ui(desktop) ? 8 : 5;

    draw_rounded_panel(fb, window->x, window->y, window->width, window->height,
                       radius, body_fill, body_fill, border, body_fill);
    draw_rounded_panel(fb, window->x + 1, window->y + 1, window->width - 2, titlebar_height,
                       radius > 2 ? radius - 2 : radius, title_fill, title_fill,
                       title_fill, title_fill);
    /* Thin, neutral hairline under the title bar (no accent glow). */
    fb_fill_rect(fb, window->x + 1, window->y + titlebar_height, window->width - 2, 1,
                 g_chrome_theme.border);

    /* App icon: dark rounded square with an accent outline and a monochrome
     * glyph — matches the reference (no bright filled colour chip). */
    {
        uint32_t acc = window->accent ? window->accent : g_chrome_theme.accent;
        uint32_t chip = mix_color(g_chrome_theme.surface, acc, 1u, 5u);
        char icon_label[2];
        /* Flat dark chip (top == bottom == highlight) with an accent outline. */
        draw_rounded_panel(fb, icon_x, icon_y, icon_size, icon_size, 4,
                           chip, chip,
                           focused ? mix_color(acc, g_chrome_theme.surface, 1u, 3u)
                                   : mix_color(acc, g_chrome_theme.surface, 1u, 1u),
                           chip);
        icon_label[0] = window->title && window->title[0] ? window->title[0] : 'A';
        icon_label[1] = '\0';
        draw_text(fb, icon_x + 6, icon_y + 2, icon_label,
                  focused ? mix_color(acc, 0x00ffffffu, 1u, 3u) : g_chrome_theme.text_dim, 1);
    }

    /* Thin monochrome line controls: minimize, maximize, close. */
    {
        uint32_t ctrl = focused ? g_chrome_theme.text : g_chrome_theme.text_dim;
        int cyl = button_y + button_size / 2;
        int i;
        /* minimize: a horizontal dash */
        fb_fill_rect(fb, x_min, cyl, button_size, 2, ctrl);
        /* maximize: a hollow square */
        fb_fill_rect(fb, x_max, button_y, button_size, 1, ctrl);
        fb_fill_rect(fb, x_max, button_y + button_size - 1, button_size, 1, ctrl);
        fb_fill_rect(fb, x_max, button_y, 1, button_size, ctrl);
        fb_fill_rect(fb, x_max + button_size - 1, button_y, 1, button_size, ctrl);
        /* close: an X */
        for (i = 0; i < button_size; ++i) {
            fb_fill_rect(fb, x_close + i, button_y + i, 2, 1, ctrl);
            fb_fill_rect(fb, x_close + (button_size - 1 - i), button_y + i, 2, 1, ctrl);
        }
    }

    draw_text(fb, icon_x + icon_size + 14, window->y + (titlebar_height - 16) / 2 + 1, window->title,
              focused ? g_chrome_theme.text : g_chrome_theme.text_dim, 1);
}

static void draw_icon(struct desktop_state *desktop, struct framebuffer *fb, const struct desktop_icon *icon) {
    int sz = ui_launcher_icon_size(desktop);
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
    int sz = ui_launcher_icon_size(desktop);
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

static void draw_cursor(struct desktop_state *desktop, struct framebuffer *fb, int x, int y) {
    /* 12x16 arrow pointer bitmap */
    static const uint16_t cursor_mask[] = {
        0x8000, 0xC000, 0xE000, 0xF000, 0xF800, 0xFC00, 0xFE00, 
        0xFF00, 0xFC00, 0xDC00, 0x8C00, 0x0C00, 0x0000
    };
    (void)desktop;
    
    int x_min = 0, y_min = 0, x_max = (int)fb->width, y_max = (int)fb->height;
    if (fb->clip_enabled) {
        x_min = fb->clip_x;
        y_min = fb->clip_y;
        x_max = fb->clip_x + fb->clip_width;
        y_max = fb->clip_y + fb->clip_height;
    }

    /* Step 1: Draw black border/shadow in all 8 directions around the cursor */
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            if (dx == 0 && dy == 0) continue;
            
            for (int row = 0; row < 13; ++row) {
                uint16_t mask = cursor_mask[row];
                int py = y + row + dy;
                if (py < y_min || py >= y_max) continue;
                uint32_t *row_pixels = (uint32_t *)((uint8_t *)fb->base + (uint32_t)py * fb->pitch);
                
                for (int col = 0; col < 12; ++col) {
                    if (mask & (0x8000 >> col)) {
                        int px = x + col + dx;
                        if (px >= x_min && px < x_max) {
                            row_pixels[px] = 0x00000000u;
                        }
                    }
                }
            }
        }
    }

    /* Step 2: Draw the white cursor body */
    for (int row = 0; row < 13; ++row) {
        uint16_t mask = cursor_mask[row];
        int py = y + row;
        if (py < y_min || py >= y_max) continue;
        uint32_t *row_pixels = (uint32_t *)((uint8_t *)fb->base + (uint32_t)py * fb->pitch);
        
        for (int col = 0; col < 12; ++col) {
            if (mask & (0x8000 >> col)) {
                int px = x + col;
                if (px >= x_min && px < x_max) {
                    row_pixels[px] = 0x00ffffffu;
                }
            }
        }
    }
}

/* Grow a user slot's surface buffer to at least w*h pixels (grow-only, so
 * interactive resize doesn't thrash the heap). Returns 1 on success. */
static int ensure_surface_buffer(struct desktop_state *desktop, int slot, int w, int h) {
    struct user_app_slot *a = &desktop->user_apps[slot];
    int need = w * h;
    uint32_t *p;
    if (need <= 0) return 1;
    if (a->surface_storage && need <= a->surface_cap_px) return 1;
    p = (uint32_t *)gfx_alloc((size_t)need * sizeof(uint32_t));
    if (p == 0) return 0;
    if (a->surface_storage) kfree(a->surface_storage);
    a->surface_storage = p;
    a->surface_cap_px = need;
    return 1;
}

/* Grow a user slot's content buffer to at least w*h pixels (grow-only),
 * clearing freshly grown storage to the transparent key so unpresented rows
 * never leak heap garbage. Returns 1 on success. */
static int ensure_content_buffer(struct desktop_state *desktop, int slot, int w, int h, uint32_t fill) {
    struct user_app_slot *a = &desktop->user_apps[slot];
    int need = w * h, i;
    uint32_t *old;
    int old_w, old_h;
    uint32_t *p;
    if (need <= 0) return 1;
    if (a->content_storage && need <= a->content_cap_px) return 1;
    old = a->content_storage;
    old_w = a->content_width;
    old_h = a->content_height;
    p = (uint32_t *)gfx_alloc((size_t)need * sizeof(uint32_t));
    if (p == 0) return 0;
    a->content_storage = p;
    a->content_cap_px = need;
    for (i = 0; i < need; ++i) a->content_storage[i] = fill;
    if (old && old_w > 0 && old_h > 0) {
        int copy_w = old_w < w ? old_w : w;
        int copy_h = old_h < h ? old_h : h;
        for (int y = 0; y < copy_h; ++y)
            for (int x = 0; x < copy_w; ++x)
                a->content_storage[y * w + x] = old[y * old_w + x];
        kfree(old);
    } else if (old) {
        kfree(old);
    }
    return 1;
}

static void reflow_content_buffer(struct desktop_state *desktop, int slot, int old_w, int old_h, int new_w, int new_h, uint32_t fill) {
    struct user_app_slot *a;
    int copy_w;
    int copy_h;
    int y;

    if (desktop == 0 || slot < 0 || slot >= MAX_USER_APPS) {
        return;
    }
    a = &desktop->user_apps[slot];
    if (a->content_storage == 0 || new_w <= 0 || new_h <= 0) {
        return;
    }
    if (old_w <= 0 || old_h <= 0) {
        for (y = 0; y < new_h; ++y) {
            int x;
            uint32_t *row = a->content_storage + (size_t)y * (size_t)new_w;
            for (x = 0; x < new_w; ++x) {
                row[x] = fill;
            }
        }
        return;
    }
    copy_w = old_w < new_w ? old_w : new_w;
    copy_h = old_h < new_h ? old_h : new_h;
    if (new_w >= old_w) {
        for (y = copy_h - 1; y >= 0; --y) {
            uint32_t *dst = a->content_storage + (size_t)y * (size_t)new_w;
            uint32_t *src = a->content_storage + (size_t)y * (size_t)old_w;
            memmove(dst, src, (size_t)copy_w * sizeof(uint32_t));
        }
    } else {
        for (y = 0; y < copy_h; ++y) {
            uint32_t *dst = a->content_storage + (size_t)y * (size_t)new_w;
            uint32_t *src = a->content_storage + (size_t)y * (size_t)old_w;
            memmove(dst, src, (size_t)copy_w * sizeof(uint32_t));
        }
    }
    for (y = 0; y < new_h; ++y) {
        int x;
        uint32_t *row = a->content_storage + (size_t)y * (size_t)new_w;
        if (y >= copy_h) {
            for (x = 0; x < new_w; ++x) {
                row[x] = fill;
            }
        } else {
            for (x = copy_w; x < new_w; ++x) {
                row[x] = fill;
            }
        }
    }
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
                int s = slot_index(index);
                struct window_state *w = &desktop->windows[index];
                /* Grow the backing store to fit the (possibly just resized or
                 * maximized) window before it is handed to fb_init. */
                if (desktop->user_apps[s].created)
                    (void)ensure_surface_buffer(desktop, s, w->width, w->height);
                return desktop->user_apps[s].surface_storage;
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
        int availw = (int)desktop->screen_width - ui_left_work_area_inset(desktop) - 24;
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
        w->x = ui_left_work_area_inset(desktop) + 12;
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

int desktop_set_wallpaper(struct desktop_state *desktop, const uint32_t *src, int src_w, int src_h) {
    int x, y;

    if (desktop == 0 || src == 0 || src_w <= 0 || src_h <= 0) {
        return -1;
    }
    if ((uint32_t)src_w > DESKTOP_MAX_WIDTH || (uint32_t)src_h > DESKTOP_MAX_HEIGHT) {
        return -1;
    }
    for (y = 0; y < src_h; ++y) {
        const uint32_t *srow = src + (long)y * src_w;
        uint32_t *drow = desktop->wallpaper_storage + (long)y * src_w;
        for (x = 0; x < src_w; ++x) {
            drow[x] = srow[x] & 0x00ffffffu;
        }
    }
    desktop->wallpaper_width = (uint32_t)src_w;
    desktop->wallpaper_height = (uint32_t)src_h;
    desktop->wallpaper_active = 1;
    desktop->background_dirty = 1;
    /* Repaint the whole screen: background_dirty alone only re-renders the
     * offscreen background_fb; without a full-screen dirty rect the compositor
     * would blit just a sub-region and leave the old backdrop on screen. */
    mark_dirty_rect(desktop, rect_from_bounds(0, 0, (int)desktop->screen_width, (int)desktop->screen_height));
    return 0;
}

/* ---- Global top-bar menu: defined by the focused window's app ---------- */

/* The focused window's menu bar (a flat winsys_menubar_item list), or NULL. */
static const struct winsys_menubar_item *tb_bar(struct desktop_state *d, int *count) {
    int f = d->menu_target_window;
    if (f >= 0 && f < WINDOW_COUNT && is_user_app_slot(f) && d->windows[f].visible) {
        int sl = slot_index(f);
        if (sl >= 0 && sl < MAX_USER_APPS && d->user_apps[sl].created &&
            d->user_apps[sl].menubar_count > 0) {
            *count = d->user_apps[sl].menubar_count;
            return d->user_apps[sl].menubar;
        }
    }
    *count = 0;
    return 0;
}

/* The active-app label: focused window title (clamped) or "Desktop". */
static void tb_app_label(struct desktop_state *d, char *out, int cap) {
    const char *t = "Desktop";
    int i;
    int f = d->menu_target_window;
    if (f >= 0 && f < WINDOW_COUNT && d->windows[f].visible && d->windows[f].title[0])
        t = d->windows[f].title;
    for (i = 0; t[i] && i < cap - 1 && i < 16; ++i) out[i] = t[i];
    out[i] = '\0';
}

static uint64_t g_desktop_activity_units;

uint64_t desktop_activity_units(void) {
    return g_desktop_activity_units;
}

static void desktop_note_activity(int pixels) {
    uint64_t units = 1;
    if (pixels > 0) {
        units += (uint64_t)pixels / 32768u;
    }
    g_desktop_activity_units += units;
}

/* Sample current CPU / UI / MEM load percentages (0..99). Called once per
 * top-bar status query; the app keeps its own sparkline history. */
static void get_topbar_stats(int *cpu_out, int *ui_out, int *mem_out) {
    static uint64_t previous_runtime;
    static uint64_t previous_activity;
    static uint64_t previous_tick;
    static int last_cpu_pct;
    static int last_ui_pct;
    uint64_t total_runtime = 0;
    uint64_t total_activity;
    uint32_t count = process_snapshot_count();
    uint32_t i;

    for (i = 0; i < count; ++i) {
        struct process_snapshot snap;
        if (process_get_snapshot(i, &snap) && snap.loaded) {
            total_runtime += snap.runtime_ticks;
        }
    }

    {
        uint64_t now = timer_tick_count();
        total_activity = desktop_activity_units();
        if (previous_tick != 0 && now > previous_tick) {
            uint64_t runtime_delta = total_runtime >= previous_runtime ? total_runtime - previous_runtime : 0;
            uint64_t activity_delta = total_activity >= previous_activity ? total_activity - previous_activity : 0;
            uint64_t tick_delta = now - previous_tick;
            last_cpu_pct = (int)((runtime_delta * 100) / tick_delta);
            last_ui_pct = (int)((activity_delta * 100) / (tick_delta * 4u));
            if (last_cpu_pct > 99) last_cpu_pct = 99;
            if (last_cpu_pct < 0) last_cpu_pct = 0;
            if (last_ui_pct > 99) last_ui_pct = 99;
            if (last_ui_pct < 0) last_ui_pct = 0;
        }
        previous_tick = now;
        previous_runtime = total_runtime;
        previous_activity = total_activity;
    }

    {
        uint64_t total_heap = kmalloc_get_physical_total();
        uint64_t used_heap = kmalloc_get_physical_used();
        int mem_pct = 0;
        if (total_heap > 0) {
            mem_pct = (int)((used_heap * 100) / total_heap);
        }
        if (mem_pct > 99) mem_pct = 99;
        *mem_out = mem_pct;
    }

    *cpu_out = last_cpu_pct;
    *ui_out = last_ui_pct;
}

/* Fill the top-bar app's status snapshot: load, uptime, focused app label and
 * its declared menu bar. (See SYS_DESKTOP_STATUS.) */
void desktop_fill_status(struct desktop_state *desktop, struct winsys_desktop_status *out) {
    int cpu = 0, ui = 0, mem = 0, n = 0, i;
    const struct winsys_menubar_item *bar;
    get_topbar_stats(&cpu, &ui, &mem);
    out->cpu_pct = (uint32_t)cpu;
    out->ui_pct = (uint32_t)ui;
    out->mem_pct = (uint32_t)mem;
    out->uptime_seconds = (uint32_t)(timer_tick_count() / timer_frequency_hz());
    out->net_up = 0;
    tb_app_label(desktop, out->app_label, (int)sizeof(out->app_label));
    bar = tb_bar(desktop, &n);
    if (n > WINSYS_MAX_MENUBAR_ITEMS) n = WINSYS_MAX_MENUBAR_ITEMS;
    out->menu_count = (uint32_t)(bar ? n : 0);
    for (i = 0; i < n && bar; ++i) out->menu[i] = bar[i];
}

/* Deliver a picked menu action to the focused window (mirrors what the old
 * kernel-drawn top bar did on click). */
void desktop_dispatch_menu_action(struct desktop_state *desktop, uint32_t action_id) {
    int f = desktop->menu_target_window;
    if (f >= 0 && f < WINDOW_COUNT && is_user_app_slot(f) && desktop->windows[f].visible) {
        app_enqueue_event_slot(desktop, slot_index(f),
            WINSYS_EVENT_MENU_ACTION, 0, 0, 0, action_id);
    }
}

static void render_background_surface(struct desktop_state *desktop) {
    struct framebuffer *fb = &desktop->background_fb;
    int order;
    int x;
    int y;

    int w = (int)desktop->screen_width;
    int sx;

    fb_reset_clip(fb);

    if (desktop->wallpaper_active) {
        int h = (int)desktop->screen_height;
        int src_w = (int)desktop->wallpaper_width;
        int src_h = (int)desktop->wallpaper_height;
        for (y = 0; y < h; ++y) {
            int syi = (int)((long)y * src_h / h);
            const uint32_t *srow = desktop->wallpaper_storage + (long)syi * src_w;
            uint32_t *drow = fb->base + (long)y * w;
            for (x = 0; x < w; ++x) {
                int sxi = (int)((long)x * src_w / w);
                drow[x] = srow[sxi];
            }
        }
    } else {
        int h = (int)desktop->screen_height;
        int cx = w / 2;
        int cy = h / 3;
        for (y = 0; y < h; ++y) {
            uint32_t row_a = mix_color(0x0015273cu, 0x001b3048u, (uint32_t)y, (uint32_t)(h > 1 ? h - 1 : 1));
            uint32_t row_b = mix_color(0x00071118u, 0x00142636u, (uint32_t)y, (uint32_t)(h > 1 ? h - 1 : 1));
            for (x = 0; x < w; ++x) {
                int dx = x - cx;
                int dy = y - cy;
                int dist = (dx * dx) / 7 + (dy * dy) / 3;
                uint32_t c = mix_color(row_a, row_b, (uint32_t)x, (uint32_t)(w > 1 ? w - 1 : 1));
                uint32_t glow = dist < 52000 ? (uint32_t)(96 - dist / 542) : 0u;
                c = mix_color(c, 0x003b638du, glow, 255u);
                if ((x % 48) == 0 || (y % 48) == 0) {
                    c = mix_color(c, 0x00ffffffu, 9u, 255u);
                }
                c = mix_color(c, 0x00ffffffu, backdrop_noise(x, y) & 5u, 255u);
                fb_put_pixel(fb, x, y, c);
            }
        }
    }

    /* The top bar (logo, brand, app menus, CPU/UI/MEM, clock, power) is drawn
     * by the userspace top-bar app (/bin/topbar), which queries the kernel via
     * SYS_DESKTOP_STATUS and composites over this background. */
    (void)sx;

    for (order = 0; order < desktop->launcher_count; ++order) {
        struct desktop_icon icon = launcher_icon_at(desktop, order);
        draw_desktop_launcher_icon(desktop, fb, &icon);
    }

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
    fb_fill_rect(fb, 0, 0, window->width, window->height, g_chrome_theme.bg);
    if (!window_frameless(window)) {
        draw_window_frame(desktop, fb, &local_window, focused);
    }

    if (is_user_app_slot(index)) {
        /* Content comes from the userspace app's last-presented pixel buffer. */
        int s = slot_index(index);
        int cx = app_ctx.content_x;
        int cy = app_ctx.content_y;
        /* Read the stored pixels at the stride they were actually written with
         * (data_width), which lags content_width during an interactive resize.
         * Using content_width here would shear the image until the app's next
         * present. Fall back to content_width before the first present. */
        int stride = desktop->user_apps[s].data_width;
        if (stride <= 0) stride = desktop->user_apps[s].content_width;
        int cw = stride;
        int ch = desktop->user_apps[s].content_height;
        int row;
        if (cw > app_ctx.content_width) cw = app_ctx.content_width;
        if (ch > app_ctx.content_height) ch = app_ctx.content_height;
        /* memcpy each row straight into the surface fb instead of a bounds-checked
         * fb_put_pixel per pixel — the content area is in-bounds by construction
         * (clamped here), so this is the same result far cheaper. */
        for (row = 0; row < ch; ++row) {
            int py = cy + row;
            int n = cw;
            if (py < 0 || py >= (int)fb->height || cx < 0) continue;
            if (cx + n > (int)fb->width) n = (int)fb->width - cx;
            if (n <= 0) continue;
            {
                uint32_t *dst = (uint32_t *)((uint8_t *)fb->base + (size_t)py * fb->pitch) + cx;
                const uint32_t *src = &desktop->user_apps[s].content_storage[(size_t)row * (size_t)stride];
                memcpy(dst, src, (size_t)n * sizeof(uint32_t));
            }
        }
    } else {
        app_draw(&desktop->apps[window->app_slot], &app_ctx);
    }
    desktop->window_dirty[index] = 0;
    desktop->window_dirty_rects[index] = rect_from_bounds(0, 0, 0, 0);
}

/*
 * Composite a window surface region onto the destination framebuffer.
 *
 * Two code paths:
 *   FAST (fully opaque, alpha == 255): straight memcpy per row, ~10x faster
 *   than the alpha-blend loop.  Transparent-key pixels (rounded corners) are
 *   still skipped, but only the first and last few pixels of each row are
 *   typically affected — the hot loop is a single memcpy over the interior.
 *
 *   ALPHA: per-pixel alpha-blend for translucent windows (alpha < 255).
 *   The source alpha channel and the global window alpha are combined into a
 *   single effective alpha to avoid a second multiplication per pixel.
 */

/* Exact division by 255 for v in [0, 65025] without a divide instruction:
 * (v + 1 + (v >> 8)) >> 8. Integer division is slow (especially under TCG), and
 * the alpha-blend inner loop did four of them per pixel — this removes them. */
static inline uint32_t div255(uint32_t v) {
    return (v + 1u + (v >> 8)) >> 8;
}

static void blit_window_surface_region(struct framebuffer *dest,
                                       const struct framebuffer *src,
                                       int dest_x, int dest_y,
                                       const struct rect *clip_rect,
                                       uint32_t alpha) {
    int x0 = dest_x > clip_rect->x ? dest_x : clip_rect->x;
    int y0 = dest_y > clip_rect->y ? dest_y : clip_rect->y;
    int x1 = dest_x + (int)src->width  < clip_rect->x + clip_rect->width  ? dest_x + (int)src->width  : clip_rect->x + clip_rect->width;
    int y1 = dest_y + (int)src->height < clip_rect->y + clip_rect->height ? dest_y + (int)src->height : clip_rect->y + clip_rect->height;

    if (x0 >= x1 || y0 >= y1) return;

    if (alpha >= 255u) {
        /* Fast path: the window is fully opaque — no blending needed.
         * We still have to skip WINDOW_TRANSPARENT_KEY pixels (rounded corners)
         * so we can't use a single memcpy for the full row width.  But we can
         * memcpy contiguous interior runs, which covers > 95% of pixels. */
        int y;
        for (y = y0; y < y1; ++y) {
            uint32_t       *dst_row = (uint32_t *)((uint8_t *)dest->base + (uint32_t)y * dest->pitch);
            const uint32_t *src_row = (const uint32_t *)((const uint8_t *)src->base + (uint32_t)(y - dest_y) * src->pitch);
            int run_start = -1;
            int x;

            for (x = x0; x <= x1; ++x) {
                uint32_t px = (x < x1) ? src_row[x - dest_x] : WINDOW_TRANSPARENT_KEY;

                if (px != WINDOW_TRANSPARENT_KEY) {
                    if (run_start < 0) run_start = x;   /* start a run */
                } else if (run_start >= 0) {
                    /* Flush the accumulated opaque run as a single memcpy. */
                    memcpy(dst_row + run_start,
                           src_row + (run_start - dest_x),
                           (size_t)(x - run_start) * sizeof(uint32_t));
                    run_start = -1;
                }
            }
        }
        return;
    }

    /* Alpha-blend path for translucent windows. */
    {
        int y;
        for (y = y0; y < y1; ++y) {
            uint32_t       *dst_row = (uint32_t *)((uint8_t *)dest->base + (uint32_t)y * dest->pitch);
            const uint32_t *src_row = (const uint32_t *)((const uint8_t *)src->base + (uint32_t)(y - dest_y) * src->pitch);
            int x;
            for (x = x0; x < x1; ++x) {
                uint32_t px = src_row[x - dest_x];
                uint32_t sa, eff, d, sr, sg, sb, dr, dg, db, r, g, b;

                if (px == WINDOW_TRANSPARENT_KEY) continue;

                sa  = px >> 24;
                if (sa == 0u) sa = 255u;
                eff = div255(sa * alpha);
                if (eff == 0u) continue;
                if (eff >= 255u) { dst_row[x] = px; continue; }

                d  = dst_row[x];
                sr = (px >> 16) & 255u; sg = (px >>  8) & 255u; sb = px & 255u;
                dr = (d  >> 16) & 255u; dg = (d  >>  8) & 255u; db = d  & 255u;
                r = div255(sr * eff + dr * (255u - eff));
                g = div255(sg * eff + dg * (255u - eff));
                b = div255(sb * eff + db * (255u - eff));
                dst_row[x] = (r << 16) | (g << 8) | b;
            }
        }
    }
}

static void draw_shadow_block(struct framebuffer *fb, int x, int y, int width, int height) {
    /* Soft, subtle shadow: only marginally darker than the desktop so windows
     * float without a hard black outline. Radius matches the window corners. */
    draw_soft_shadow(fb, x, y, width, height, 8, WINDOW_SHADOW_SPREAD, 0x000e1828u);
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
    CTX_APP_USER = 4,
    CTX_SWITCH_WINDOW = 5,
    CTX_SEPARATOR = 6
};

#define CTX_MAX_ENTRIES (3 + WINSYS_MAX_MENU_ITEMS + 1 + WINDOW_COUNT)

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

static int string_contains(const char *haystack, const char *needle);

static int is_same_app(struct desktop_state *desktop, int win1, int win2) {
    if (win1 == win2) return 1;
    if (win1 < 0 || win1 >= WINDOW_COUNT || win2 < 0 || win2 >= WINDOW_COUNT) return 0;

    const char *t1 = desktop->windows[win1].title;
    const char *t2 = desktop->windows[win2].title;

    if (!t1 || !t2) return 0;

    if ((strcmp(t1, "TERMINAL") == 0 || string_contains(t1, "Terminal") || string_contains(t1, "terminal")) &&
        (strcmp(t2, "TERMINAL") == 0 || string_contains(t2, "Terminal") || string_contains(t2, "terminal"))) {
        return 1;
    }
    if ((string_contains(t1, "DOOM") || string_contains(t1, "doom")) &&
        (string_contains(t2, "DOOM") || string_contains(t2, "doom"))) {
        return 1;
    }
    if ((string_contains(t1, "Browser") || string_contains(t1, "browser")) &&
        (string_contains(t2, "Browser") || string_contains(t2, "browser"))) {
        return 1;
    }
    if ((strcmp(t1, "TASK MANAGER") == 0 || string_contains(t1, "Tasks") || string_contains(t1, "taskmgr")) &&
        (strcmp(t2, "TASK MANAGER") == 0 || string_contains(t2, "Tasks") || string_contains(t2, "taskmgr"))) {
        return 1;
    }
    if ((string_contains(t1, "Demo") || string_contains(t1, "demo")) &&
        (string_contains(t2, "Demo") || string_contains(t2, "demo"))) {
        return 1;
    }
    if ((string_contains(t1, "C++") || string_contains(t1, "cpp")) &&
        (string_contains(t2, "C++") || string_contains(t2, "cpp"))) {
        return 1;
    }
    if ((strcmp(t1, "INFO") == 0 || string_contains(t1, "Info") || string_contains(t1, "sysinfo")) &&
        (strcmp(t2, "INFO") == 0 || string_contains(t2, "Info") || string_contains(t2, "sysinfo"))) {
        return 1;
    }

    if (strcmp(t1, t2) == 0) return 1;

    return 0;
}

static void format_instance_name(struct desktop_state *desktop, int win, char *dst, size_t cap) {
    const char *title = desktop->windows[win].title;
    if (!title) title = "App";

    size_t i = 0;
    size_t max_title_len = (cap > 11) ? (cap - 11) : 5;

    while (i < max_title_len && title[i] != '\0') {
        dst[i] = title[i];
        i++;
    }
    dst[i] = '\0';

    if (is_user_app_slot(win)) {
        int slot = slot_index(win);
        uint32_t pid = desktop->user_apps[slot].pid;
        const char *suffix1 = " (";
        size_t j = 0;
        while (i + 1 < cap && suffix1[j]) {
            dst[i++] = suffix1[j++];
        }
        dst[i] = '\0';

        char pid_str[16];
        int len = 0;
        uint32_t temp = pid;
        if (temp == 0) {
            pid_str[len++] = '0';
        } else {
            while (temp > 0 && len < 15) {
                pid_str[len++] = '0' + (temp % 10);
                temp /= 10;
            }
        }
        for (int k = 0; k < len / 2; ++k) {
            char t = pid_str[k];
            pid_str[k] = pid_str[len - 1 - k];
            pid_str[len - 1 - k] = t;
        }
        pid_str[len] = '\0';

        j = 0;
        while (i + 1 < cap && pid_str[j]) {
            dst[i++] = pid_str[j++];
        }

        if (i + 1 < cap) {
            dst[i++] = ')';
        }
        dst[i] = '\0';
    } else {
        const char *suffix2 = " (Kernel)";
        size_t j = 0;
        while (i + 1 < cap && suffix2[j]) {
            dst[i++] = suffix2[j++];
        }
        dst[i] = '\0';
    }
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

    /* List multiple running instances macOS-style at the bottom of the context menu */
    {
        int count = 0;
        int idxs[WINDOW_COUNT];
        int j;
        for (j = 0; j < WINDOW_COUNT; ++j) {
            int active = 0;
            if (is_user_app_slot(j)) {
                if (desktop->user_apps[slot_index(j)].created) {
                    active = 1;
                }
            } else {
                if (desktop->windows[j].title && desktop->windows[j].title[0]) {
                    active = 1;
                }
            }

            if (active && is_same_app(desktop, win, j)) {
                idxs[count++] = j;
            }
        }

        if (count > 1) {
            if (n < CTX_MAX_ENTRIES) {
                ctx_set_label(&out[n], "");
                out[n].kind = CTX_SEPARATOR;
                out[n].action = 0;
                ++n;
            }
            for (j = 0; j < count; ++j) {
                if (n < CTX_MAX_ENTRIES) {
                    char label[64];
                    format_instance_name(desktop, idxs[j], label, sizeof(label));
                    ctx_set_label(&out[n], label);
                    out[n].kind = CTX_SWITCH_WINDOW;
                    out[n].action = (uint32_t)idxs[j];
                    ++n;
                }
            }
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
        if (entries[i].kind == CTX_SEPARATOR) continue;
        int len = 0;
        while (entries[i].label[len]) ++len;
        if (entries[i].kind == CTX_SWITCH_WINDOW) len += 2;
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
    /* Same dark glass-panel look as the top-bar dropdowns. */
    {
        uint32_t dbg = g_chrome_theme.menu_bg;
        uint32_t item_text = g_chrome_theme.menu_item;
        draw_soft_shadow(fb, r.x, r.y, r.width, r.height, 10, 5, 0x00060a12u);
        draw_rounded_panel(fb, r.x, r.y, r.width, r.height, 10, dbg, dbg, g_chrome_theme.border, dbg);
        for (i = 0; i < count; ++i) {
            int ry = r.y + 8 + i * 22;
            int hov = (i == desktop->context_menu_hover);

            if (entries[i].kind == CTX_SEPARATOR) {
                fb_fill_rect(fb, r.x + 8, ry + 10, r.width - 16, 1, g_chrome_theme.border);
                continue;
            }

            uint32_t color;
            if (entries[i].kind == CTX_QUIT_APP) color = g_chrome_theme.danger;
            else if (entries[i].kind == CTX_APP_KERNEL || entries[i].kind == CTX_APP_USER) color = g_chrome_theme.accent;
            else if (entries[i].kind == CTX_SWITCH_WINDOW) {
                int target_win = (int)entries[i].action;
                int is_focused = (desktop->focused_window == target_win);
                int is_visible = (target_win >= 0 && target_win < WINDOW_COUNT && desktop->windows[target_win].visible);
                if (is_focused && is_visible) {
                    color = g_chrome_theme.accent;
                } else if (hov) {
                    color = g_chrome_theme.text;
                } else {
                    color = item_text;
                }
            }
            else color = hov ? g_chrome_theme.text : item_text;

            if (hov) fb_fill_rect(fb, r.x + 4, ry - 1, r.width - 8, 20,
                                  mix_color(g_chrome_theme.accent, dbg, 1u, 6u));

            if (entries[i].kind == CTX_SWITCH_WINDOW) {
                int target_win = (int)entries[i].action;
                int is_focused = (desktop->focused_window == target_win);
                int is_visible = (target_win >= 0 && target_win < WINDOW_COUNT && desktop->windows[target_win].visible);
                if (is_focused && is_visible) {
                    fb_fill_rect(fb, r.x + 10, ry + 8, 4, 4, g_chrome_theme.accent);
                    draw_text(fb, r.x + 20, ry + 2, entries[i].label, color, 1);
                } else {
                    draw_text(fb, r.x + 20, ry + 2, entries[i].label, color, 1);
                }
            } else {
                draw_text(fb, r.x + 14, ry + 2, entries[i].label, color, 1);
            }
        }
    }
    (void)y;
}

static struct rect desktop_modal_rect(const struct desktop_state *desktop) {
    int w = 430;
    int h = 168;
    int sw = (int)desktop->screen_width;
    int sh = (int)desktop->screen_height;
    if (w > sw - 32) w = sw - 32;
    if (w < 260) w = 260;
    return rect_from_bounds((sw - w) / 2, (sh - h) / 2, w, h);
}

static struct rect desktop_modal_button_rect(const struct rect *modal) {
    return rect_from_bounds(modal->x + modal->width - 104, modal->y + modal->height - 48, 76, 30);
}

void desktop_show_error(struct desktop_state *desktop, const char *title, const char *message) {
    struct rect r;
    if (desktop == 0) return;
    copy_capped(desktop->modal_title, sizeof(desktop->modal_title), title ? title : "Error");
    copy_capped(desktop->modal_message, sizeof(desktop->modal_message), message ? message : "The operation could not be completed.");
    desktop->modal_open = 1;
    r = desktop_modal_rect(desktop);
    mark_dirty_rect(desktop, rect_from_bounds(r.x - 16, r.y - 16, r.width + 32, r.height + 32));
}

static void draw_modal_message(struct framebuffer *fb, int x, int y, int max_chars, const char *message, uint32_t color) {
    char line[56];
    int line_pos = 0;
    int line_y = y;
    int i = 0;
    if (max_chars > (int)sizeof(line) - 1) max_chars = (int)sizeof(line) - 1;
    while (message != 0 && message[i] != '\0' && line_y < y + 52) {
        char ch = message[i++];
        if (ch == '\n' || line_pos >= max_chars) {
            line[line_pos] = '\0';
            draw_text(fb, x, line_y, line, color, 1);
            line_y += 20;
            line_pos = 0;
            if (ch == '\n') continue;
        }
        line[line_pos++] = ch;
    }
    if (line_pos > 0 && line_y < y + 60) {
        line[line_pos] = '\0';
        draw_text(fb, x, line_y, line, color, 1);
    }
}

static void draw_desktop_modal(struct desktop_state *desktop, struct framebuffer *fb) {
    struct rect r;
    struct rect b;
    uint32_t shade = mix_color(g_chrome_theme.bg, 0x00000000u, 1u, 2u);
    if (!desktop->modal_open) return;
    r = desktop_modal_rect(desktop);
    b = desktop_modal_button_rect(&r);

    fb_fill_rect(fb, 0, 0, (int)desktop->screen_width, (int)desktop->screen_height, shade);
    draw_rounded_panel(fb, r.x, r.y, r.width, r.height, 10,
                       mix_color(g_chrome_theme.surface_hi, g_chrome_theme.bg, 1u, 4u),
                       g_chrome_theme.surface, g_chrome_theme.border_hi, g_chrome_theme.surface_hi);
    draw_text(fb, r.x + 24, r.y + 22, desktop->modal_title, g_chrome_theme.text, 1);
    fb_fill_rect(fb, r.x + 24, r.y + 50, r.width - 48, 1, g_chrome_theme.border);
    draw_modal_message(fb, r.x + 24, r.y + 68, (r.width - 48) / 8, desktop->modal_message, g_chrome_theme.text_dim);
    draw_rounded_panel(fb, b.x, b.y, b.width, b.height, 6,
                       mix_color(g_chrome_theme.accent, g_chrome_theme.surface, 1u, 5u),
                       mix_color(g_chrome_theme.accent, g_chrome_theme.bg, 1u, 4u),
                       g_chrome_theme.border_hi, g_chrome_theme.accent);
    draw_text(fb, b.x + 26, b.y + 8, "OK", g_chrome_theme.text, 1);
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
    if (entries[row].kind == CTX_SEPARATOR) return -1;
    return row;
}

/*
 * Direct-compose a user-app window onto the destination framebuffer.
 *
 * For non-translucent user-app windows we skip the window_surface
 * intermediate buffer entirely.  Instead of:
 *
 *   content_storage → window_surface → backbuffer          (2 full copies)
 *
 * we do:
 *
 *   content_storage → backbuffer (content region, memcpy)  (1 copy)
 *   draw chrome     → backbuffer directly                  (draw calls only)
 *
 * This saves one full-window memcpy per frame — for a 640×400 game at 35 fps
 * that is ~35 MB/s of memory bandwidth freed.
 *
 * Preconditions: window is a user-app slot, visible, not WINSYS_WINDOW_TRANSLUCENT.
 */
static void compose_user_app_direct(struct desktop_state *desktop,
                                    struct framebuffer *dest,
                                    int index,
                                    const struct rect *clip) {
    struct window_state   *window  = &desktop->windows[index];
    struct window_state    local_w = *window;
    int                    s       = slot_index(index);
    struct app_draw_context ctx;
    int  focused = (desktop->focused_window == index);
    int  stride, cw, ch, row;
    int  abs_cx, abs_cy;

    /* Compute content placement (content_x/y are window-relative). */
    build_app_draw_context(desktop, &local_w, focused, &ctx);

    /* Draw chrome directly onto the backbuffer.  We temporarily set local_w's
     * position to the screen position so the draw helpers land at the right
     * coordinates.  All chrome helpers take a framebuffer* + an absolute-
     * position window_state so the framebuffer's origin must be the screen
     * origin — which is exactly what `dest` (the backbuffer) is. */
    if (!window_frameless(window)) {
        draw_window_frame(desktop, dest, window, focused);
    }

    /* Blit content_storage directly to the backbuffer content region. */
    stride = desktop->user_apps[s].data_width;
    if (stride <= 0) stride = desktop->user_apps[s].content_width;
    cw = stride;
    ch = desktop->user_apps[s].content_height;
    if (cw > ctx.content_width)  cw = ctx.content_width;
    if (ch > ctx.content_height) ch = ctx.content_height;

    abs_cx = window->x + ctx.content_x;   /* screen-absolute content origin */
    abs_cy = window->y + ctx.content_y;

    for (row = 0; row < ch; ++row) {
        int py = abs_cy + row;
        int n  = cw;
        int cx = abs_cx;

        if (py < clip->y || py >= clip->y + clip->height) continue;
        if (py < 0 || py >= (int)dest->height || cx < 0) continue;
        if (cx + n > (int)dest->width) n = (int)dest->width - cx;
        if (n <= 0) continue;

        {
            uint32_t       *dst = (uint32_t *)((uint8_t *)dest->base + (uint32_t)py * dest->pitch) + cx;
            const uint32_t *src = &desktop->user_apps[s].content_storage[(size_t)row * (size_t)stride];
            memcpy(dst, src, (size_t)n * sizeof(uint32_t));
        }
    }

    /* Mark the surface as clean so render_window_surface is not called later. */
    desktop->window_dirty[index] = 0;
    desktop->window_dirty_rects[index] = rect_from_bounds(0, 0, 0, 0);
}

static void compose_scene_rect(struct desktop_state *desktop, struct framebuffer *fb, const struct rect *rect) {
    int order;
    struct rect full_window_rect;

    if (desktop->background_dirty) {
        render_background_surface(desktop);
    }

    /* Render window surfaces for windows that need it.
     * User-app windows that will be direct-composed are skipped here; they
     * write straight to the backbuffer in the compositing loop below. */
    for (order = 0; order < WINDOW_COUNT; ++order) {
        int index = desktop->z_order[order];
        if (!desktop->window_dirty[index]) continue;
        if (is_user_app_slot(index) &&
            !(desktop->windows[index].flags & WINSYS_WINDOW_TRANSLUCENT)) {
            /* Will be handled by compose_user_app_direct — skip. */
            continue;
        }
        render_window_surface(desktop, index);
    }

    /* Occlusion culling: Subtract the opaque regions of all overlapping windows
     * that are on top of the background. */
    struct rect_list {
        struct rect rects[128];
        int count;
    };
    
    struct rect_list list;
    list.rects[0] = *rect;
    list.count = 1;

    for (order = 0; order < WINDOW_COUNT; ++order) {
        int index = desktop->z_order[order];
        struct window_state *window = &desktop->windows[index];

        if (!window->visible) continue;
        
        int opaque = 0;
        if (!(window->flags & WINSYS_WINDOW_TRANSLUCENT)) {
            if (is_user_app_slot(index)) {
                opaque = 1;
            } else {
                opaque = (g_chrome_theme.window_alpha == 255u);
            }
        }

        if (!opaque) continue;

        /* Opaque window rect (slightly inset to protect rounded corner transparency/borders) */
        int inset = 8;
        if (window->width <= inset * 2 || window->height <= inset * 2) continue;
        struct rect w_rect = rect_from_bounds(window->x + inset, window->y + inset, 
                                               window->width - (inset * 2), window->height - (inset * 2));
        
        if (!rects_intersect(&w_rect, rect)) continue;

        int original_count = list.count;
        struct rect_list next_list;
        next_list.count = 0;

        for (int i = 0; i < original_count; ++i) {
            struct rect R = list.rects[i];
            if (!rects_intersect(&R, &w_rect)) {
                if (next_list.count < 128) {
                    next_list.rects[next_list.count++] = R;
                }
                continue;
            }

            // Top part
            if (w_rect.y > R.y) {
                if (next_list.count < 128) {
                    next_list.rects[next_list.count++] = rect_from_bounds(R.x, R.y, R.width, w_rect.y - R.y);
                }
            }
            // Bottom part
            if (w_rect.y + w_rect.height < R.y + R.height) {
                if (next_list.count < 128) {
                    next_list.rects[next_list.count++] = rect_from_bounds(R.x, w_rect.y + w_rect.height, R.width, (R.y + R.height) - (w_rect.y + w_rect.height));
                }
            }
            // Left & Right parts inside the Y span
            int y0 = w_rect.y > R.y ? w_rect.y : R.y;
            int y1 = w_rect.y + w_rect.height < R.y + R.height ? w_rect.y + w_rect.height : R.y + R.height;
            if (y1 > y0) {
                if (w_rect.x > R.x) {
                    if (next_list.count < 128) {
                        next_list.rects[next_list.count++] = rect_from_bounds(R.x, y0, w_rect.x - R.x, y1 - y0);
                    }
                }
                if (w_rect.x + w_rect.width < R.x + R.width) {
                    if (next_list.count < 128) {
                        next_list.rects[next_list.count++] = rect_from_bounds(w_rect.x + w_rect.width, y0, (R.x + R.width) - (w_rect.x + w_rect.width), y1 - y0);
                    }
                }
            }
        }
        
        list = next_list;
    }

    for (int i = 0; i < list.count; ++i) {
        fb_blit_rect(fb, &desktop->background_fb, &list.rects[i]);
    }

    for (order = 0; order < WINDOW_COUNT; ++order) {
        int index = desktop->z_order[order];
        struct window_state *window = &desktop->windows[index];

        if (!window->visible) continue;

        full_window_rect = window_rect(window);
        if (!rects_intersect(&full_window_rect, rect)) continue;

        if (!(window->flags & WINSYS_WINDOW_NO_SHADOW)) {
            int si = (int)window->shadow_inset_top;
            draw_shadow_block(fb, window->x, window->y + si,
                              window->width, window->height - si);
        }

        if (is_user_app_slot(index) &&
            !(window->flags & WINSYS_WINDOW_TRANSLUCENT)) {
            /* Direct path: no window_surface intermediate buffer.
             * Only paint if the content has actually changed (dirty) OR if
             * another window was dirtied and forced a full scene redraw —
             * in the latter case the background blit already cleared this
             * region so we must repaint even if the app's own dirty flag is
             * clear.  We detect this by checking whether our region
             * intersects the dirty rect that triggered this compose call. */
            if (desktop->window_dirty[index] ||
                rects_intersect(&full_window_rect, rect)) {
                compose_user_app_direct(desktop, fb, index, rect);
            }
        } else {
            /* Standard path via window surface (translucent, kernel apps). */
            blit_window_surface_region(fb, &desktop->window_fbs[index],
                                       window->x, window->y, rect,
                                       (window->flags & WINSYS_WINDOW_TRANSLUCENT)
                                           ? 245u
                                           : g_chrome_theme.window_alpha);
        }
    }

    draw_context_menu(desktop, fb);
    draw_desktop_modal(desktop, fb);
}

void desktop_init(struct desktop_state *desktop, uint32_t screen_width, uint32_t screen_height) {
    int info_width = clamp_value((int)screen_width / 3, 430, 620);
    int info_height = clamp_value((int)screen_height / 3, 250, 320);
    int files_width = clamp_value((int)screen_width / 4, 300, 420);
    int files_height = clamp_value((int)screen_height / 3, 240, 320);
    int terminal_width = clamp_value((int)screen_width / 2 - 40, 620, 720);
    int terminal_height = clamp_value((int)screen_height / 2 - 30, 360, 440);
    int tasks_width = clamp_value((int)screen_width / 2 - 40, 560, 720);
    int tasks_height = clamp_value((int)screen_height / 2 - 80, 340, 430);
    int right_margin = screen_width >= 1600u ? 120 : 60;
    int top_margin = screen_height >= 1000u ? 104 : 88;

    desktop_theme_load();

    desktop->screen_width = screen_width;
    desktop->screen_height = screen_height;
    /* Kernel demo windows stay available, but the initial scene is now the
     * desktop-shell layout from the design reference. */
    desktop->windows[WINDOW_INFO] = (struct window_state){"SYSTEM", (screen_width >= 1920u || screen_height >= 1080u) ? 232 : 168, top_margin, info_width, info_height, 0x001a2233u, 0x00222d42u, 0x0035d0b9u, 0, WINDOW_INFO, 0, 0, 0, 0, 0, 0};
    desktop->windows[WINDOW_FILES] = (struct window_state){"FILES", (int)screen_width - files_width - right_margin, top_margin + 36, files_width, files_height, 0x001a2233u, 0x00222d42u, 0x00f5b15au, 0, WINDOW_FILES, 0, 0, 0, 0, 0, 0};
    desktop->windows[WINDOW_TERMINAL] = (struct window_state){"TERMINAL", (int)screen_width - terminal_width - right_margin, top_margin, terminal_width, terminal_height, 0x000e1622u, 0x00222d42u, 0x0064f2ccu, 0, WINDOW_TERMINAL, 0, 0, 0, 0, 0, 0};
    desktop->windows[WINDOW_TASK_MANAGER] = (struct window_state){"TASK MANAGER", (int)screen_width - tasks_width - right_margin, top_margin + terminal_height + 22, tasks_width, tasks_height, 0x001a2233u, 0x00222d42u, 0x00ef7f7fu, 0, WINDOW_TASK_MANAGER, 0, 0, 0, 0, 0, 0};
    /* Userspace app windows: hidden until an app creates them. */
    {
        int i;
        for (i = 0; i < MAX_USER_APPS; i++) {
            int win_idx = WINDOW_APP_FIRST + i;
            desktop->windows[win_idx] = (struct window_state){"APP", 320 + i * 24, 160 + i * 24, 480, 320, 0x001a2233u, 0x00222d42u, 0x008f7bf0u, 0, (uint8_t)win_idx, 0, 0, 0, 0, 0, 0};
            desktop->user_apps[i].created = 0;
            desktop->user_apps[i].pid = 0;
            desktop->user_apps[i].content_width = 0;
            desktop->user_apps[i].content_height = 0;
            desktop->user_apps[i].data_width = 0;
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
    desktop->context_menu_hover = -1;
    desktop->modal_open = 0;
    desktop->modal_title[0] = '\0';
    desktop->modal_message[0] = '\0';
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
    desktop->focused_window = WINDOW_TERMINAL;
    desktop->menu_target_window = -1;
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
    
    /* Initialize tile-based dirty tracking */
    desktop->tile_size = 32;
    desktop->tiles_x = (int)(screen_width / desktop->tile_size) + 1;
    desktop->tiles_y = (int)(screen_height / desktop->tile_size) + 1;
    for(int i = 0; i < MAX_TILE_BIT_ARRAY; i++) desktop->dirty_tiles[i] = 0;

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
    if (window_frameless(w)) {
        *ox = w->x;
        *oy = w->y;
    } else {
        *ox = w->x + (large_ui(desktop) ? 22 : 16);
        *oy = w->y + (large_ui(desktop) ? 58 : 48);
    }
}

void desktop_layout_shell_panels(struct desktop_state *desktop) {
    int i;

    if (desktop == 0) {
        return;
    }

    for (i = 0; i < WINDOW_COUNT; ++i) {
        struct window_state *w = &desktop->windows[i];
        struct rect old_rect;
        int slot;
        int new_w;
        int new_h;
        int old_cw;
        int old_ch;
        int changed = 0;

        if (!is_user_app_slot(i) || !w->title || !w->title[0] || !window_frameless(w)) {
            continue;
        }

        old_rect = window_rect(w);
        slot = slot_index(i);
        old_cw = desktop->user_apps[slot].content_width;
        old_ch = desktop->user_apps[slot].content_height;
        new_w = w->width;
        new_h = w->height;

        if (strcmp(w->title, "Top Bar") == 0) {
            new_w = (int)desktop->screen_width;
            w->x = 0;
            w->y = 0;
            changed = 1;
        } else if (strcmp(w->title, "Dock") == 0) {
            int dock_margin = 24;
            int max_w = (int)desktop->screen_width - dock_margin;
            if (max_w < 96) max_w = (int)desktop->screen_width;
            new_w = 560;
            if (new_w > max_w) new_w = max_w;
            if (new_w < 1) new_w = 1;
            w->x = ((int)desktop->screen_width - new_w) / 2;
            w->y = (int)desktop->screen_height - new_h - dock_margin;
            if (w->x < 0) w->x = 0;
            if (w->y < 0) w->y = 0;
            changed = 1;
        }

        if (!changed) {
            continue;
        }

        w->width = new_w;
        w->height = new_h;
        if (!ensure_surface_buffer(desktop, slot, w->width, w->height)) {
            continue;
        }
        fb_init(&desktop->window_fbs[i],
                (uintptr_t)desktop->user_apps[slot].surface_storage,
                w->width, w->height, w->width * 4u, 32u);
        update_app_content_size_slot(desktop, slot);
        if (desktop->user_apps[slot].content_storage != 0 &&
            (desktop->user_apps[slot].content_width != old_cw ||
             desktop->user_apps[slot].content_height != old_ch)) {
            int n = desktop->user_apps[slot].content_width *
                    desktop->user_apps[slot].content_height;
            int p;
            for (p = 0; p < n; ++p) {
                desktop->user_apps[slot].content_storage[p] = WINDOW_TRANSPARENT_KEY;
            }
            desktop->user_apps[slot].data_width = desktop->user_apps[slot].content_width;
        }
        mark_window_dirty(desktop, i);
        mark_dirty_rect(desktop, old_rect);
        mark_dirty_rect(desktop, window_rect(w));
    }
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

    if (window->flags & WINSYS_WINDOW_ASPECT_RATIO) {
        int W_0 = desktop->resize_start_width;
        int H_0 = desktop->resize_start_height;
        int X_0 = desktop->resize_start_x;
        int Y_0 = desktop->resize_start_y;

        if (W_0 > 0 && H_0 > 0) {
            /* 1. Compute effective min/max sizes that satisfy both width/height boundaries and maintain ratio. */
            int eff_min_w = min_w;
            int eff_min_h = min_h;
            int min_w_from_h = (min_h * W_0 + H_0 - 1) / H_0;
            if (min_w_from_h > eff_min_w) eff_min_w = min_w_from_h;
            int min_h_from_w = (min_w * H_0 + W_0 - 1) / W_0;
            if (min_h_from_w > eff_min_h) eff_min_h = min_h_from_w;

            int eff_max_w = max_w;
            int eff_max_h = max_h;
            int max_w_from_h = max_h * W_0 / H_0;
            if (max_w_from_h < eff_max_w) eff_max_w = max_w_from_h;
            int max_h_from_w = max_w * H_0 / W_0;
            if (max_h_from_w < eff_max_h) eff_max_h = max_h_from_w;

            /* 2. Adjust max sizes based on screen boundaries. */
            int limit_max_w = eff_max_w;
            int limit_max_h = eff_max_h;
            if (desktop->resize_edges & RESIZE_LEFT) {
                int max_w_space = (X_0 + W_0) - (ui_left_work_area_inset(desktop) + 6);
                if (max_w_space < limit_max_w) limit_max_w = max_w_space;
            }
            if (desktop->resize_edges & RESIZE_RIGHT) {
                int max_w_space = ((int)desktop->screen_width - 8) - X_0;
                if (max_w_space < limit_max_w) limit_max_w = max_w_space;
            }
            if (desktop->resize_edges & RESIZE_TOP) {
                int max_h_space = (Y_0 + H_0) - 8;
                if (max_h_space < limit_max_h) limit_max_h = max_h_space;
            }
            if (desktop->resize_edges & RESIZE_BOTTOM) {
                int max_h_space = (taskbar_y - 4) - Y_0;
                if (max_h_space < limit_max_h) limit_max_h = max_h_space;
            }

            /* Propagate screen boundary limits to both dimensions to maintain aspect ratio */
            int max_w_from_h_limit = limit_max_h * W_0 / H_0;
            if (max_w_from_h_limit < limit_max_w) limit_max_w = max_w_from_h_limit;
            int max_h_from_w_limit = limit_max_w * H_0 / W_0;
            if (max_h_from_w_limit < limit_max_h) limit_max_h = max_h_from_w_limit;

            /* Ensure limits are not below minimums */
            if (limit_max_w < eff_min_w) limit_max_w = eff_min_w;
            if (limit_max_h < eff_min_h) limit_max_h = eff_min_h;

            /* 3. Compute prospective dimensions based on mouse movement */
            int w_proposal = W_0;
            if (desktop->resize_edges & RESIZE_LEFT) {
                w_proposal = W_0 - dx;
            } else if (desktop->resize_edges & RESIZE_RIGHT) {
                w_proposal = W_0 + dx;
            }

            int h_proposal = H_0;
            if (desktop->resize_edges & RESIZE_TOP) {
                h_proposal = H_0 - dy;
            } else if (desktop->resize_edges & RESIZE_BOTTOM) {
                h_proposal = H_0 + dy;
            }

            /* 4. Decide which dimension is the master for resizing */
            int has_h = (desktop->resize_edges & (RESIZE_LEFT | RESIZE_RIGHT)) != 0;
            int has_v = (desktop->resize_edges & (RESIZE_TOP | RESIZE_BOTTOM)) != 0;
            int use_w_as_master = 0;
            if (has_h && has_v) {
                int dx_abs = dx < 0 ? -dx : dx;
                int dy_abs = dy < 0 ? -dy : dy;
                if (dx_abs * H_0 > dy_abs * W_0) {
                    use_w_as_master = 1;
                } else {
                    use_w_as_master = 0;
                }
            } else if (has_h) {
                use_w_as_master = 1;
            } else {
                use_w_as_master = 0;
            }

            /* 5. Calculate new width/height and apply position changes */
            if (use_w_as_master) {
                w = w_proposal;
                if (w < eff_min_w) w = eff_min_w;
                if (w > limit_max_w) w = limit_max_w;
                h = (w * H_0 + W_0 / 2) / W_0;
            } else {
                h = h_proposal;
                if (h < eff_min_h) h = eff_min_h;
                if (h > limit_max_h) h = limit_max_h;
                w = (h * W_0 + H_0 / 2) / H_0;
            }

            x = X_0;
            y = Y_0;
            if (desktop->resize_edges & RESIZE_LEFT) {
                x = (X_0 + W_0) - w;
            }
            if (desktop->resize_edges & RESIZE_TOP) {
                y = (Y_0 + H_0) - h;
            }
        }
    } else {
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
        if (x < ui_left_work_area_inset(desktop) + 6) {
            int delta = ui_left_work_area_inset(desktop) + 6 - x;
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
    }

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

static int string_contains(const char *haystack, const char *needle) {
    if (!haystack || !needle) return 0;
    int i = 0;
    while (haystack[i]) {
        int j = 0;
        while (needle[j] && haystack[i + j] == needle[j]) {
            j++;
        }
        if (!needle[j]) return 1;
        i++;
    }
    return 0;
}

static int find_window_for_dock_icon(struct desktop_state *desktop, int icon_idx) {
    /* Fixed system windows. */
    if (icon_idx == 0) return WINDOW_TERMINAL;
    if (icon_idx == 4) return WINDOW_TASK_MANAGER;

    /* Position-independent: scan ALL running user apps and match by
     * title pattern.  Works regardless of icon layout / drag-reorder. */
    int slot;
    for (slot = 0; slot < MAX_USER_APPS; ++slot) {
        if (!desktop->user_apps[slot].created) continue;
        const char *title = desktop->user_apps[slot].title;
        if (string_contains(title, "DOOM") || string_contains(title, "doom"))
            return WINDOW_APP_FIRST + slot;
        if (string_contains(title, "Browser") || string_contains(title, "browser"))
            return WINDOW_APP_FIRST + slot;
        if (string_contains(title, "Files") || string_contains(title, "files")
            || string_contains(title, "File") || string_contains(title, "file"))
            return WINDOW_APP_FIRST + slot;
        if (string_contains(title, "Editor") || string_contains(title, "editor")
            || string_contains(title, "Text") || string_contains(title, "text"))
            return WINDOW_APP_FIRST + slot;
        if (string_contains(title, "Audio") || string_contains(title, "Player")
            || string_contains(title, "audio"))
            return WINDOW_APP_FIRST + slot;
        if (string_contains(title, "C++") || string_contains(title, "cpp"))
            return WINDOW_APP_FIRST + slot;
    }

    /* Fallback: first running user app. */
    for (slot = 0; slot < MAX_USER_APPS; ++slot) {
        if (desktop->user_apps[slot].created)
            return WINDOW_APP_FIRST + slot;
    }
    return -1;
}

void desktop_handle_input(struct desktop_state *desktop, const struct mouse_state *mouse, const struct keyboard_state *keyboard) {
    size_t i;
    int index;
    int scroll_delivered = 0;

    desktop->mouse_x = mouse->x;
    desktop->mouse_y = mouse->y;
    desktop->mouse_buttons = mouse->buttons;
    if (mouse->left_pressed) {
        journal_log_hex(JOURNAL_APP, 0, "desktop left press buttons=", mouse->buttons);
    }

    if (desktop->modal_open) {
        size_t ki;
        int close_modal = 0;
        struct rect r = desktop_modal_rect(desktop);
        struct rect b = desktop_modal_button_rect(&r);
        for (ki = 0; ki < keyboard->count; ++ki) {
            if (keyboard->chars[ki] == 0x1b || keyboard->chars[ki] == '\n' || keyboard->chars[ki] == '\r') {
                close_modal = 1;
            }
        }
        if (mouse->left_pressed) {
            close_modal = point_in_rect(mouse->x, mouse->y, b.x, b.y, b.width, b.height);
        }
        if (close_modal) {
            desktop->modal_open = 0;
            mark_dirty_rect(desktop, rect_from_bounds(r.x - 16, r.y - 16, r.width + 32, r.height + 32));
        }
        return;
    }

    if (desktop->context_menu_open && mouse->moved) {
        int item = context_menu_item_at(desktop, mouse->x, mouse->y);
        if (item != desktop->context_menu_hover) {
            desktop->context_menu_hover = item;
            mark_dirty_rect(desktop, context_menu_rect(desktop));
        }
    }

    if (mouse->right_pressed) {
        if (desktop->context_menu_open) {
            struct rect menu_rect = context_menu_rect(desktop);
            desktop->context_menu_open = 0;
            desktop->context_menu_hover = -1;
            mark_dirty_rect(desktop, menu_rect);
        }

        index = topmost_window_at(desktop, mouse->x, mouse->y);
        if (index >= 0) {
            struct window_state *window = &desktop->windows[index];
            if (is_user_app_slot(index) && strcmp(desktop->user_apps[slot_index(index)].title, "Dock") == 0) {
                int rx = mouse->x - window->x;
                int ry = mouse->y - window->y;
                if (ry >= 40 && ry <= 98) {
                    int idx;
                    for (idx = 0; idx < 6; ++idx) {
                        /* Dock hbox starts at x=50, icons 58px wide, gap 38px. */
                        int x_start = 50 + idx * (58 + 38);
                        if (rx >= x_start && rx <= x_start + 58) {
                            int target_win = find_window_for_dock_icon(desktop, idx);
                            if (target_win >= 0) {
                                desktop->context_menu_open = 1;
                                desktop->context_menu_window = target_win;
                                desktop->context_menu_x = mouse->x;
                                desktop->context_menu_y = mouse->y;
                                desktop->context_menu_hover = -1;
                                mark_dirty_rect(desktop, context_menu_rect(desktop));
                                return;
                            }
                        }
                    }
                }
            } else if (titlebar_hit(desktop, window, mouse->x, mouse->y)) {
                desktop->context_menu_open = 1;
                desktop->context_menu_window = index;
                desktop->context_menu_x = mouse->x;
                desktop->context_menu_y = mouse->y;
                desktop->context_menu_hover = -1;
                mark_dirty_rect(desktop, context_menu_rect(desktop));
                return;
            } else {
                focus_window(desktop, index);
            }
        }
    }

    if (desktop->resizing_window >= 0 && (mouse->buttons & 0x01u) != 0u) {
        apply_window_resize(desktop, desktop->resizing_window, mouse->x, mouse->y);
        return;
    }

    if (desktop->dragging_launcher >= 0 && (mouse->buttons & 0x01u) != 0u) {
        int li = desktop->dragging_launcher;
        int sz = ui_launcher_icon_size(desktop);
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

    /* The global top bar (menus, indicators) is owned by the userspace top-bar
     * app now; it handles its own clicks and dispatches menu actions via
     * SYS_MENU_DISPATCH. The kernel no longer interprets clicks in that area. */

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
                    mark_background_dirty(desktop);
                    mark_window_dirty(desktop, menu_window);
                    focus_window(desktop, menu_window);
                } else if (e->kind == CTX_HIDE) {
                    serial_write("[ctx] HIDE win="); serial_write_hex_u64((uint64_t)menu_window);
                    serial_write("\r\n");
                    struct rect before = window_rect(window);
                    window->visible = 0;
                    if (desktop->focused_window == menu_window) desktop->focused_window = -1;
                    mark_background_dirty(desktop);
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
                } else if (e->kind == CTX_SWITCH_WINDOW) {
                    int target_win = (int)e->action;
                    if (target_win >= 0 && target_win < WINDOW_COUNT) {
                        struct window_state *t_win = &desktop->windows[target_win];
                        t_win->visible = 1;
                        mark_window_dirty(desktop, target_win);
                        focus_window(desktop, target_win);
                    }
                }
                
            }
            return;
        }

        index = topmost_window_at(desktop, mouse->x, mouse->y);
        journal_log_hex(JOURNAL_APP, 0, "mouse down hit window=", (uint64_t)(index < 0 ? 0xffffu : (uint32_t)index));
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
                /* Minimise and close hide the window. Userspace shell policy
                 * decides how hidden windows are surfaced again. */
                window->visible = 0;
                if (desktop->focused_window == index) {
                    desktop->focused_window = -1;
                }
                
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
                if (point_in_icon(mouse->x, mouse->y, &icon, ui_launcher_icon_size(desktop))) {
                    desktop->dragging_launcher = (int)i;
                    desktop->launcher_drag_offset_x = mouse->x - icon.x;
                    desktop->launcher_drag_offset_y = mouse->y - icon.y;
                    desktop->launcher_drag_start_x = icon.x;
                    desktop->launcher_drag_start_y = icon.y;
                    desktop->launcher_drag_moved = 0;
                    return;
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
                int pid = process_spawn_path(desktop->launcher_execs[li], 0, 0, g_desktop_uid, g_desktop_uid);
                if (pid == -SYSCALL_ENOMEM) {
                    desktop_show_error(desktop, "Not Enough Memory",
                                       "There is not enough memory or no free process slot available to start this app.");
                } else if (pid < 0) {
                    desktop_show_error(desktop, "App Could Not Start",
                                       "The app could not be started. Check whether the file is still available.");
                }
            }
        }
        desktop->dragging_launcher = -1;
        desktop->launcher_drag_moved = 0;
        desktop->dragging_window = -1;
        desktop->resizing_window = -1;
        desktop->resize_edges = 0;
    }

    /* Deliver mouse-move events to shell panels (ALWAYS_ON_TOP | NO_DOCK |
     * FRAMELESS) that the cursor is currently over, regardless of focus.
     * This allows hover effects (tooltips, highlights) on the dock and
     * top bar without requiring the user to click on them first. */
    if (mouse->moved && desktop->dragging_window < 0 && desktop->resizing_window < 0) {
        int pi;
        for (pi = 0; pi < WINDOW_COUNT; ++pi) {
            struct window_state *pw = &desktop->windows[pi];
            uint32_t panel_flags = WINSYS_WINDOW_ALWAYS_ON_TOP
                                 | WINSYS_WINDOW_NO_DOCK
                                 | WINSYS_WINDOW_FRAMELESS;
            if (!pw->visible) continue;
            if ((pw->flags & panel_flags) != panel_flags) continue;
            if (pi == desktop->focused_window) continue; /* handled below */
            if (!is_user_app_slot(pi)) continue;
            if (!point_in_rect(mouse->x, mouse->y, pw->x, pw->y, pw->width, pw->height)) continue;
            {
                int ps = slot_index(pi);
                int pox, poy;
                app_content_origin_for_window(desktop, pi, &pox, &poy);
                app_enqueue_event_slot(desktop, ps, WINSYS_EVENT_MOUSE_MOVE,
                                       mouse->x - pox, mouse->y - poy, mouse->buttons, 0);
            }
        }
    }

    if (mouse->wheel != 0 && desktop->dragging_window < 0 && desktop->resizing_window < 0) {
        int sw = topmost_window_at(desktop, mouse->x, mouse->y);
        if (is_user_app_slot(sw) && desktop->windows[sw].visible) {
            int ss = slot_index(sw);
            app_enqueue_event_slot(desktop, ss, WINSYS_EVENT_SCROLL, 0, mouse->wheel, 0, 0);
            scroll_delivered = 1;
        }
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
        if (mouse->wheel != 0 && !scroll_delivered) {
            app_enqueue_event_slot(desktop, s, WINSYS_EVENT_SCROLL, 0, mouse->wheel, 0, 0);
        }
        if (mouse->left_pressed) {
            journal_log_hex(JOURNAL_APP, desktop->user_apps[s].pid, "mouse down enqueue window=", (uint64_t)(uint32_t)fw);
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

	/* Userspace app windows (created via SYS_WINDOW_CREATE_EX) whose owning
	 * process has exited — e.g. killed from the task manager — are closed here.
	 * desktop_app_close_for_pid is idempotent (no-op once the slot is freed), so
	 * this is safe even on arches that also close from the process-exit path. */
	for (i = 0; i < MAX_USER_APPS; ++i) {
		uint32_t pid = desktop->user_apps[i].pid;
		if (desktop->user_apps[i].created && pid != 0 && !process_pid_alive(pid)) {
			desktop_app_close_for_pid(desktop, pid);
		}
	}

	/* A kernel app whose owning process (e.g. the terminal's /bin/sh) has died
	 * gets its window cleanly closed here. The app drops its session state and
	 * only restarts on an explicit user gesture. */
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
    desktop_note_activity(rect.width * rect.height);
}

void desktop_render_rect(struct desktop_state *desktop, struct framebuffer *fb, const struct rect *rect) {
    fb_set_clip(fb, rect);
    compose_scene_rect(desktop, fb, rect);
    fb_reset_clip(fb);
    if (!(rect->y == 0 && rect->height <= 54)) {
        desktop_note_activity(rect->width * rect->height);
    }
}

void desktop_draw_cursor_overlay(const struct desktop_state *desktop, struct framebuffer *fb) {
    draw_cursor((struct desktop_state *)desktop, fb, desktop->mouse_x, desktop->mouse_y);
}

void desktop_cursor_rect_at(const struct desktop_state *desktop, int x, int y, struct rect *rect) {
    *rect = cursor_rect(desktop, x, y);
}

/* Merges all dirty tiles into a single rectangle covering them. */
int desktop_get_dirty_region(struct desktop_state *desktop, struct rect *rect) {
    if (!desktop->dirty) {
        return 0;
    }

    int min_tile_x = desktop->tiles_x;
    int max_tile_x = -1;
    int min_tile_y = desktop->tiles_y;
    int max_tile_y = -1;
    int found = 0;

    for (int tile_idx = 0; tile_idx < desktop->tiles_x * desktop->tiles_y; tile_idx++) {
        if (desktop->dirty_tiles[tile_idx / 8] & (1 << (tile_idx % 8))) {
            int tile_x = tile_idx % desktop->tiles_x;
            int tile_y = tile_idx / desktop->tiles_x;
            
            if (tile_x < min_tile_x) min_tile_x = tile_x;
            if (tile_x > max_tile_x) max_tile_x = tile_x;
            if (tile_y < min_tile_y) min_tile_y = tile_y;
            if (tile_y > max_tile_y) max_tile_y = tile_y;
            found = 1;
            
            // Clear the dirty bit
            desktop->dirty_tiles[tile_idx / 8] &= ~(1 << (tile_idx % 8));
        }
    }
    
    if (!found) {
        desktop->dirty = 0;
        return 0;
    }

    rect->x = min_tile_x * desktop->tile_size;
    rect->y = min_tile_y * desktop->tile_size;
    rect->width = (max_tile_x - min_tile_x + 1) * desktop->tile_size;
    rect->height = (max_tile_y - min_tile_y + 1) * desktop->tile_size;
    
    desktop->dirty = 0;
    return 1;
}

/* ---- Window server (userspace GUI apps) ---- */

int desktop_app_create_ex(struct desktop_state *desktop, uint32_t pid, const struct winsys_window_options *options) {
    int slot;
    int win_idx;
    struct window_state *w;
    int frame_w;
    int frame_h;
    int width;
    int height;
    uint32_t flags;
    size_t i;

    if (options == 0) return -1;
    width = options->width;
    height = options->height;
    flags = options->flags;

    if (width < 16) width = 16;
    if (height < 16) height = 16;
    if ((flags & WINSYS_WINDOW_FRAMELESS) == 0u) {
        if (width < 80) width = 80;
        if (height < 60) height = 60;
    }
    if (width > (int)WINDOW_APP_CONTENT_MAX_WIDTH) width = (int)WINDOW_APP_CONTENT_MAX_WIDTH;
    if (height > (int)WINDOW_APP_CONTENT_MAX_HEIGHT) height = (int)WINDOW_APP_CONTENT_MAX_HEIGHT;

    /* SINGLE_INSTANCE: if another window with the same title already
     * exists, bring it to front instead of creating a duplicate. */
    if ((flags & WINSYS_WINDOW_SINGLE_INSTANCE) != 0u && options->title) {
        for (int si = 0; si < MAX_USER_APPS; si++) {
            if (desktop->user_apps[si].created &&
                desktop->user_apps[si].title[0] &&
                strcmp(desktop->user_apps[si].title, options->title) == 0) {
                int existing = WINDOW_APP_FIRST + si;
                desktop->windows[existing].visible = 1;
                desktop->context_menu_open = 0;
                mark_window_dirty(desktop, existing);
                focus_window(desktop, existing);
                return existing;
            }
        }
    }

    /* Reuse existing slot if this PID already has one; else find free slot. */
    slot = find_slot_by_pid(desktop, pid);
    if (slot < 0) slot = find_free_slot(desktop);
    if (slot < 0) return -1;
    win_idx = WINDOW_APP_FIRST + slot;
    w = &desktop->windows[win_idx];

    /* Window size = content size + frame insets (matches build_app_draw_context). */
    if ((flags & WINSYS_WINDOW_FRAMELESS) != 0u) {
        frame_w = width;
        frame_h = height;
    } else {
        frame_w = width + (large_ui(desktop) ? 44 : 32);
        frame_h = height + (large_ui(desktop) ? 82 : 66);
    }

    for (i = 0; i + 1 < sizeof(desktop->user_apps[slot].title) && options->title && options->title[i]; ++i) {
        desktop->user_apps[slot].title[i] = options->title[i];
    }
    desktop->user_apps[slot].title[(options->title) ? i : 0] = '\0';

    w->title = desktop->user_apps[slot].title;
    w->width = frame_w;
    w->height = frame_h;
    w->flags = flags;
    if ((flags & WINSYS_WINDOW_POSITIONED) != 0u) {
        w->x = options->x;
        w->y = options->y;
    } else {
        w->x = (int)desktop->screen_width / 2 - frame_w / 2 + slot * 24;
        w->y = (int)desktop->screen_height / 2 - frame_h / 2 + slot * 24;
    }
    w->visible = 1;
    w->maximized = 0;
    w->shadow_inset_top = (options->shadow_inset_top > 0 && options->shadow_inset_top < 255)
                          ? (uint8_t)options->shadow_inset_top : 0u;
    if ((flags & WINSYS_WINDOW_POSITIONED) == 0u) {
        if (w->x < ui_left_work_area_inset(desktop) + 16) w->x = ui_left_work_area_inset(desktop) + 16;
        if (w->y < 16) w->y = 16;
    }
    /* Positioned app windows are requests, not trusted coordinates. Clamp them
     * back onto the physical screen so a stale display-mode result or runtime
     * resolution change cannot strand frameless windows offscreen. */
    {
        int max_x = (int)desktop->screen_width - w->width;
        int max_y = (int)desktop->screen_height - w->height;
        if (max_x < 0) max_x = 0;
        if (max_y < 0) max_y = 0;
        if (w->x < 0) w->x = 0;
        if (w->y < 0) w->y = 0;
        if (w->x > max_x) w->x = max_x;
        if (w->y > max_y) w->y = max_y;
    }

    /* Allocate the window's backing stores at its actual size. If memory is
     * exhausted, fail the create cleanly rather than leaving a half-built slot. */
    {
        uint32_t content_fill = (flags & WINSYS_WINDOW_TRANSLUCENT)
                              ? WINDOW_TRANSPARENT_KEY : g_chrome_theme.bg;
        if (!ensure_surface_buffer(desktop, slot, w->width, w->height) ||
            !ensure_content_buffer(desktop, slot, width, height, content_fill)) {
            return -1;
        }
    }

    desktop->user_apps[slot].created = 1;
    desktop->user_apps[slot].pid = pid;
    desktop->user_apps[slot].content_width = width;
    desktop->user_apps[slot].content_height = height;
    desktop->user_apps[slot].data_width = 0;   /* nothing presented yet */
    desktop->user_apps[slot].event_head = 0;
    desktop->user_apps[slot].event_tail = 0;
    desktop->user_apps[slot].menu_count = 0;

    fb_init(&desktop->window_fbs[win_idx], (uintptr_t)desktop->user_apps[slot].surface_storage, w->width, w->height, w->width * 4u, 32u);

    focus_window(desktop, win_idx);
    
    mark_window_dirty(desktop, win_idx);
    mark_dirty_rect(desktop, window_rect(w));
    return win_idx;
}

int desktop_app_create(struct desktop_state *desktop, uint32_t pid, const char *title, int width, int height) {
    struct winsys_window_options options;
    options.title = title;
    options.width = width;
    options.height = height;
    options.flags = 0;
    options.x = 0;
    options.y = 0;
    return desktop_app_create_ex(desktop, pid, &options);
}

static void app_content_origin(struct desktop_state *desktop, int win_idx, int *cx, int *cy) {
    struct window_state *wn = &desktop->windows[win_idx];
    if (window_frameless(wn)) { *cx = 0; *cy = 0; }
    else { *cx = large_ui(desktop) ? 22 : 16; *cy = large_ui(desktop) ? 58 : 48; }
}

/* Present a sub-rectangle [dx,dy,dw,dh] (in the app's content space) from the
 * app's full canvas `src`. dw<=0 means "the whole content area" (full present).
 * Partial presents copy only the damaged region into content_storage and poke
 * it straight into the already-rendered window surface, marking only that small
 * screen rect dirty — so a periodic updater (task manager) no longer forces a
 * full-window recomposite every tick. */
int desktop_app_present_rect(struct desktop_state *desktop, uint32_t pid, int win_id,
                             const uint32_t *src, int src_w, int src_h,
                             int dx, int dy, int dw, int dh) {
    int slot, win_idx, row, cw, ch, cx, cy;

    if (src == 0) return -1;
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
    if (desktop->user_apps[slot].content_storage == 0) return -1;

    cw = src_w < desktop->user_apps[slot].content_width ? src_w : desktop->user_apps[slot].content_width;
    ch = src_h < desktop->user_apps[slot].content_height ? src_h : desktop->user_apps[slot].content_height;
    if (cw > (int)WINDOW_APP_CONTENT_MAX_WIDTH) cw = (int)WINDOW_APP_CONTENT_MAX_WIDTH;
    if (ch > (int)WINDOW_APP_CONTENT_MAX_HEIGHT) ch = (int)WINDOW_APP_CONTENT_MAX_HEIGHT;

    /* Full present when no (valid) damage rect was supplied. */
    int full = (dw <= 0 || dh <= 0);
    if (full) { dx = 0; dy = 0; dw = cw; dh = ch; }
    /* Clamp the damage rect into the content area. */
    if (dx < 0) { dw += dx; dx = 0; }
    if (dy < 0) { dh += dy; dy = 0; }
    if (dx + dw > cw) dw = cw - dx;
    if (dy + dh > ch) dh = ch - dy;
    if (dw <= 0 || dh <= 0) return 0;

    /* Copy the damaged rows into the persistent content buffer. */
    for (row = 0; row < dh; ++row) {
        uint32_t *dst = &desktop->user_apps[slot].content_storage[(size_t)(dy + row) * (size_t)desktop->user_apps[slot].content_width + (size_t)dx];
        const uint32_t *s = &src[(size_t)(dy + row) * (size_t)src_w + (size_t)dx];
        memcpy(dst, s, (size_t)dw * sizeof(uint32_t));
    }

    /* The stored pixels are now laid out at content_width stride; record that so
     * the compositor reads them correctly until the next (post-resize) present. */
    desktop->user_apps[slot].data_width = desktop->user_apps[slot].content_width;

    /* If a full surface render is already pending, let it pick the content up;
     * a full present also goes the simple route (render whole surface). */
    if (full || desktop->window_dirty[win_idx]) {
        desktop_note_activity(cw * ch);
        mark_window_dirty(desktop, win_idx);
        mark_dirty_rect(desktop, window_rect(&desktop->windows[win_idx]));
        return 0;
    }

    /* Partial: poke just the damaged sub-rect into the (already-rendered) window
     * surface and mark only that screen region dirty. */
    app_content_origin(desktop, win_idx, &cx, &cy);
    {
        struct framebuffer *fb = &desktop->window_fbs[win_idx];
        int fx = cx + dx, fy = cy + dy, n = dw, r;
        if (fx + n > (int)fb->width) n = (int)fb->width - fx;
        for (r = 0; r < dh && fx >= 0 && n > 0; ++r) {
            int py = fy + r;
            if (py < 0 || py >= (int)fb->height) continue;
            uint32_t *dst = (uint32_t *)((uint8_t *)fb->base + (size_t)py * fb->pitch) + fx;
            const uint32_t *s = &desktop->user_apps[slot].content_storage[(size_t)(dy + r) * (size_t)desktop->user_apps[slot].content_width + (size_t)dx];
            memcpy(dst, s, (size_t)n * sizeof(uint32_t));
        }
    }
    {
        struct window_state *wn = &desktop->windows[win_idx];
        desktop_note_activity(dw * dh);
        mark_dirty_rect(desktop, rect_from_bounds(wn->x + cx + dx, wn->y + cy + dy, dw, dh));
    }
    return 0;
}

int desktop_app_present(struct desktop_state *desktop, uint32_t pid, int win_id, const uint32_t *src, int src_w, int src_h) {
    return desktop_app_present_rect(desktop, pid, win_id, src, src_w, src_h, 0, 0, 0, 0);
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

int desktop_app_set_menubar(struct desktop_state *desktop, uint32_t pid, int win_id, const struct winsys_menubar_item *items, int count) {
    int slot, i;

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
    if (count > WINSYS_MAX_MENUBAR_ITEMS) count = WINSYS_MAX_MENUBAR_ITEMS;
    if (count > 0 && items == 0) return -1;

    for (i = 0; i < count; ++i) {
        int j;
        for (j = 0; j + 1 < WINSYS_MENUBAR_LABEL_MAX && items[i].label[j]; ++j)
            desktop->user_apps[slot].menubar[i].label[j] = items[i].label[j];
        desktop->user_apps[slot].menubar[i].label[j] = '\0';
        for (j = 0; j + 1 < WINSYS_MENUBAR_SHORTCUT_MAX && items[i].shortcut[j]; ++j)
            desktop->user_apps[slot].menubar[i].shortcut[j] = items[i].shortcut[j];
        desktop->user_apps[slot].menubar[i].shortcut[j] = '\0';
        desktop->user_apps[slot].menubar[i].flags = items[i].flags;
        desktop->user_apps[slot].menubar[i].action_id = items[i].action_id;
    }
    desktop->user_apps[slot].menubar_count = count;
    return 0;
}

void desktop_app_close_for_pid(struct desktop_state *desktop, uint32_t pid) {
    int slot;
    int win_idx;
    struct window_state *w;
    struct rect before;

    if (desktop == 0) return;

    slot = find_slot_by_pid(desktop, pid);
    if (slot < 0) return;

    win_idx = WINDOW_APP_FIRST + slot;
    w = &desktop->windows[win_idx];
    before = window_rect(w);
    w->visible = 0;
    /* Release the window's backing stores back to the heap. */
    if (desktop->user_apps[slot].surface_storage) {
        gfx_free(desktop->user_apps[slot].surface_storage);
        desktop->user_apps[slot].surface_storage = 0;
        desktop->user_apps[slot].surface_cap_px = 0;
    }
    if (desktop->user_apps[slot].content_storage) {
        gfx_free(desktop->user_apps[slot].content_storage);
        desktop->user_apps[slot].content_storage = 0;
        desktop->user_apps[slot].content_cap_px = 0;
    }
    desktop->window_fbs[win_idx].base = 0;
    desktop->user_apps[slot].created = 0;
    desktop->user_apps[slot].pid = 0;
    desktop->user_apps[slot].content_width = 0;
    desktop->user_apps[slot].content_height = 0;
    desktop->user_apps[slot].data_width = 0;
    desktop->user_apps[slot].event_head = 0;
    desktop->user_apps[slot].event_tail = 0;
    desktop->user_apps[slot].menu_count = 0;
    if (desktop->focused_window == win_idx) {
        desktop->focused_window = -1;
    }
    if (desktop->menu_target_window == win_idx) {
        desktop->menu_target_window = -1;
    }
    mark_dirty_rect(desktop, before);
}

int desktop_shell_dock_active(const struct desktop_state *desktop) {
    int slot;
    if (desktop == 0) return 0;
    for (slot = 0; slot < MAX_USER_APPS; ++slot) {
        if (desktop->user_apps[slot].created &&
            strcmp(desktop->user_apps[slot].title, "Dock") == 0) {
            return 1;
        }
    }
    return 0;
}
