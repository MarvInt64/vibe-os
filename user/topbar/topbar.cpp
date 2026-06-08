/*
 * topbar - VibeOS desktop top bar userspace app.
 *
 * This process owns a full-width, frameless, always-on-top window pinned to the
 * top of the screen. The visible bar shows the VibeOS logo, the focused app
 * label, app menus, status indicators, uptime and a power menu.
 *
 * The window is taller than the bar so dropdowns can be drawn inside the same
 * surface. Pixels outside the bar/dropdown are painted with TRANSPARENT_KEY so
 * the desktop remains visible and input can pass through transparent areas.
 */

/* Standard headers — provide all primitive types and the libc API. */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <vibeos.h>
#include <sys/syscall.h>

#include "svg.h"

/* ---- Window system constants — use vibeos.h canonical names ------------- */

/* Local aliases kept for readability inside this file. */
#define WIN_FRAMELESS     VOS_WINDOW_FRAMELESS
#define WIN_NO_DOCK       VOS_WINDOW_NO_DOCK
#define WIN_POSITIONED    VOS_WINDOW_POSITIONED
#define WIN_ALWAYS_ON_TOP VOS_WINDOW_ALWAYS_ON_TOP
#define WIN_TRANSLUCENT   VOS_WINDOW_TRANSLUCENT
#define WIN_NO_SHADOW     VOS_WINDOW_NO_SHADOW

#define TRANSPARENT_KEY   VOS_TRANSPARENT_KEY

/* Event type constants from vibeos.h vos_event.type. */
#define EV_MOUSE_MOVE 1
#define EV_MOUSE_DOWN 2
#define EV_KEY        4
#define EV_CLOSE      5
#define EV_RESIZE     9

/* Menu-bar item flag aliases. */
#define MB_TITLE    VOS_MB_TITLE
#define MB_DIVIDER  VOS_MB_DIVIDER
#define MB_DANGER   VOS_MB_DANGER
#define MB_MAX_ITEMS VOS_DESKTOP_MENU_MAX
#define APP_LABEL_MAX 20

/* Use the vibeos.h canonical types throughout; local aliases for brevity. */
typedef struct vos_window_options  win_options;
typedef struct vos_event           win_event;
typedef struct vos_menubar_item    menubar_item;
typedef struct vos_desktop_status  desktop_status;

/* Thin helpers that call the vibeos.h / libc API. */
static void sleep_ms(unsigned int ms)        { vos_sleep_ms(ms); }

/* ------------------------------------------------------------------------- */
/* Theme and layout constants                                                */
/* ------------------------------------------------------------------------- */

#define COLOR_BAR       0x001a2c44u
#define COLOR_BORDER    0x0039506au
#define COLOR_TEXT      0x00eaf2fau
#define COLOR_DIM       0x00b7c7d8u
#define COLOR_ACCENT    0x004da3ffu
#define COLOR_GREEN     0x0063d9a5u
#define COLOR_RED       0x00e36c7au
#define COLOR_HIGHLIGHT 0x00233850u
#define COLOR_DROPDOWN  0x00203549u

#define BAR_HEIGHT             54
#define WINDOW_HEIGHT          340
#define MENU_ITEM_HEIGHT       26
#define MAX_WINDOW_WIDTH       1920
#define DEFAULT_WINDOW_WIDTH   1024
#define MAX_TOP_LEVEL_MENUS    16
#define MENU_TITLE_PADDING_X   8
#define MENU_TITLE_GAP         22
#define DROPDOWN_PADDING_TOP   5
#define DROPDOWN_PADDING_X     14
#define POWER_MENU_WIDTH       160
#define POWER_ICON_SIZE        20
#define POWER_HIT_WIDTH        44
#define LOGO_BASE_SIZE         44
#define LOGO_MAX_SIZE          52
#define LOGO_HOVER_STEPS       10
#define HISTORY_SIZE           24

/* ------------------------------------------------------------------------- */
/* Global app state                                                          */
/* ------------------------------------------------------------------------- */

static int g_screen_width;
static int g_window_id;
static uint32_t *g_canvas;

static int g_open_menu_index = -1;
static int g_power_menu_open = 0;
static int g_menu_title_x[MAX_TOP_LEVEL_MENUS];
static int g_menu_title_width[MAX_TOP_LEVEL_MENUS];
static int g_menu_title_count;

static char g_logo_svg[4096];
static int g_logo_loaded;
static int g_logo_hovered;
static int g_logo_hover_value;
static uint32_t g_logo_pixels[LOGO_MAX_SIZE * LOGO_MAX_SIZE];

static uint32_t g_power_pixels[POWER_ICON_SIZE * POWER_ICON_SIZE];
static int g_power_loaded;

static int g_cpu_history[HISTORY_SIZE];
static int g_ui_history[HISTORY_SIZE];
static int g_history_pos;
static int g_history_full;
static uint32_t g_last_sampled_second = 0xffffffffu;

/* ------------------------------------------------------------------------- */
/* Small utilities                                                           */
/* ------------------------------------------------------------------------- */

static int clamp_int(int value, int min_value, int max_value) {
    if (value < min_value) return min_value;
    if (value > max_value) return max_value;
    return value;
}

static int string_length(const char *text) {
    int length = 0;
    while (text[length]) ++length;
    return length;
}

static void uint_to_string(unsigned value, char *out) {
    char reversed[12];
    int count = 0;
    int write_index = 0;

    if (value == 0) {
        out[0] = '0';
        out[1] = 0;
        return;
    }

    while (value) {
        reversed[count++] = (char)('0' + value % 10);
        value /= 10;
    }

    while (count) {
        out[write_index++] = reversed[--count];
    }
    out[write_index] = 0;
}

static void two_digit_uint_to_string(unsigned value, char *out) {
    out[0] = (char)('0' + (value / 10) % 10);
    out[1] = (char)('0' + value % 10);
    out[2] = 0;
}

static int read_text_file(const char *path, char *buffer, int capacity) {
    int fd;
    int bytes_read;

    if (!buffer || capacity <= 1) return 0;

    fd = (int)open(path, O_RDONLY);
    if (fd < 0) return 0;

    bytes_read = (int)read(fd, buffer, (size_t)(capacity - 1));
    close(fd);

    if (bytes_read <= 0) return 0;

    buffer[bytes_read] = 0;
    return 1;
}

/* ------------------------------------------------------------------------- */
/* Drawing                                                                   */
/* ------------------------------------------------------------------------- */

static void clear_canvas(void) {
    int pixel_count = g_screen_width * WINDOW_HEIGHT;
    int i;

    for (i = 0; i < pixel_count; ++i) {
        g_canvas[i] = TRANSPARENT_KEY;
    }
}

static void fill_rect(int x, int y, int width, int height, uint32_t color) {
    int yy;
    int xx;

    for (yy = y; yy < y + height; ++yy) {
        if (yy < 0 || yy >= WINDOW_HEIGHT) continue;

        for (xx = x; xx < x + width; ++xx) {
            if (xx < 0 || xx >= g_screen_width) continue;
            g_canvas[yy * g_screen_width + xx] = color;
        }
    }
}

static void draw_rect_border(int x, int y, int width, int height, uint32_t color) {
    fill_rect(x, y, width, 1, color);
    fill_rect(x, y + height - 1, width, 1, color);
    fill_rect(x, y, 1, height, color);
    fill_rect(x + width - 1, y, 1, height, color);
}

static void blend_pixel(int x, int y, uint32_t color, int alpha) {
    uint32_t *dst;
    uint32_t sr;
    uint32_t sg;
    uint32_t sb;
    uint32_t dr;
    uint32_t dg;
    uint32_t db;

    if (alpha <= 0) return;
    if (x < 0 || y < 0 || x >= g_screen_width || y >= WINDOW_HEIGHT) return;

    if (alpha >= 255) {
        g_canvas[y * g_screen_width + x] = color;
        return;
    }

    dst = &g_canvas[y * g_screen_width + x];

    sr = (color >> 16) & 255u;
    sg = (color >> 8) & 255u;
    sb = color & 255u;

    dr = (*dst >> 16) & 255u;
    dg = (*dst >> 8) & 255u;
    db = *dst & 255u;

    dr = (sr * (uint32_t)alpha + dr * (255u - (uint32_t)alpha)) / 255u;
    dg = (sg * (uint32_t)alpha + dg * (255u - (uint32_t)alpha)) / 255u;
    db = (sb * (uint32_t)alpha + db * (255u - (uint32_t)alpha)) / 255u;

    *dst = (dr << 16) | (dg << 8) | db;
}

static int measure_text(const char *text) {
    if (!text || !text[0]) return 0;
    return (int)__sc2(SYS_TEXT_METRICS, (uint64_t)(size_t)text, 1);
}

static void draw_text(int x, int y, const char *text, uint32_t color) {
    if (!text || !text[0]) return;
    vos_text_draw(g_canvas, g_screen_width, WINDOW_HEIGHT, x, y, text, color, 1);
}

static void draw_separator(int *x) {
    fill_rect(*x, 16, 1, 22, COLOR_BORDER);
    *x += 8;
}

/* ------------------------------------------------------------------------- */
/* SVG icons                                                                 */
/* ------------------------------------------------------------------------- */

static void load_logo(void) {
    g_logo_loaded = read_text_file("/icons/vibeos-logo.svg", g_logo_svg, (int)sizeof(g_logo_svg));
}

static uint32_t interpolate_color(uint32_t from, uint32_t to, int step, int max_step) {
    int from_r = (int)((from >> 16) & 255u);
    int from_g = (int)((from >> 8) & 255u);
    int from_b = (int)(from & 255u);

    int to_r = (int)((to >> 16) & 255u);
    int to_g = (int)((to >> 8) & 255u);
    int to_b = (int)(to & 255u);

    int r = from_r + (to_r - from_r) * step / max_step;
    int g = from_g + (to_g - from_g) * step / max_step;
    int b = from_b + (to_b - from_b) * step / max_step;

    return (uint32_t)((r << 16) | (g << 8) | b);
}

static void draw_svg_pixels(const uint32_t *pixels, int size, int x, int y) {
    int px;
    int py;

    for (py = 0; py < size; ++py) {
        for (px = 0; px < size; ++px) {
            uint32_t pixel = pixels[py * size + px];
            int alpha = (int)((pixel >> 24) & 255u);

            if (alpha > 0) {
                blend_pixel(x + px, y + py, pixel & 0x00ffffffu, alpha);
            }
        }
    }
}

static void draw_logo(int x, int y) {
    int size;
    int offset_x;
    int offset_y;
    uint32_t color;

    if (!g_logo_loaded) return;

    size = LOGO_BASE_SIZE + (6 * g_logo_hover_value) / LOGO_HOVER_STEPS;
    size = clamp_int(size, LOGO_BASE_SIZE, LOGO_MAX_SIZE);

    color = interpolate_color(COLOR_ACCENT, 0x00d44dffu, g_logo_hover_value, LOGO_HOVER_STEPS);
    svg_render_rgba(g_logo_svg, g_logo_pixels, size, color);

    offset_x = x + (LOGO_BASE_SIZE - size) / 2;
    offset_y = y + (LOGO_BASE_SIZE - size) / 2;

    draw_svg_pixels(g_logo_pixels, size, offset_x, offset_y);
}

static void load_power_icon(void) {
    static char svg_buffer[1024];

    if (!read_text_file("/icons/power.svg", svg_buffer, (int)sizeof(svg_buffer))) return;

    svg_render_rgba(svg_buffer, g_power_pixels, POWER_ICON_SIZE, COLOR_TEXT);
    g_power_loaded = 1;
}

static void draw_power_icon(int x, int y) {
    if (!g_power_loaded) return;
    draw_svg_pixels(g_power_pixels, POWER_ICON_SIZE, x, y);
}

/* ------------------------------------------------------------------------- */
/* History and sparklines                                                    */
/* ------------------------------------------------------------------------- */

static void sample_status_history(const desktop_status *status) {
    if (status->uptime_seconds == g_last_sampled_second) return;

    g_last_sampled_second = status->uptime_seconds;
    g_cpu_history[g_history_pos] = (int)status->cpu_pct;
    g_ui_history[g_history_pos] = (int)status->ui_pct;

    g_history_pos = (g_history_pos + 1) % HISTORY_SIZE;
    if (g_history_pos == 0) g_history_full = 1;
}

static void draw_line(int x0, int y0, int x1, int y1, uint32_t color) {
    int dx = x1 - x0;
    int dy = y1 - y0;
    int adx = dx < 0 ? -dx : dx;
    int ady = dy < 0 ? -dy : dy;
    int steps = adx > ady ? adx : ady;
    int i;

    if (steps < 1) steps = 1;

    for (i = 0; i <= steps; ++i) {
        int x = x0 + dx * i / steps;
        int y = y0 + dy * i / steps;

        blend_pixel(x, y, color, 255);
        blend_pixel(x, y + 1, color, 90);
    }
}

static void draw_sparkline(int x, int y, int width, int height, const int *history, uint32_t color) {
    int count = g_history_full ? HISTORY_SIZE : g_history_pos;
    int previous_x = -1;
    int previous_y = -1;
    int i;

    if (count < 2) {
        fill_rect(x, y + height - 1, width, 1, COLOR_BORDER);
        return;
    }

    for (i = 0; i < count; ++i) {
        int history_index = (g_history_pos - count + i + HISTORY_SIZE * 2) % HISTORY_SIZE;
        int value = clamp_int(history[history_index], 0, 100);
        int point_x = x + (width - 1) * i / (count - 1);
        int point_y = y + height - 1 - (height - 1) * value / 100;

        if (previous_x >= 0) {
            draw_line(previous_x, previous_y, point_x, point_y, color);
        }

        previous_x = point_x;
        previous_y = point_y;
    }
}

/* ------------------------------------------------------------------------- */
/* Menu layout                                                               */
/* ------------------------------------------------------------------------- */

static int first_menu_x(const desktop_status *status) {
    return 122 + measure_text(status->app_label) + 24;
}

static int title_item_index(const desktop_status *status, int title_index) {
    int i;
    int title_count = 0;

    for (i = 0; i < (int)status->menu_count; ++i) {
        if (!(status->menu[i].flags & MB_TITLE)) continue;

        if (title_count == title_index) return i;
        ++title_count;
    }

    return -1;
}

static void layout_menus(const desktop_status *status) {
    int i;
    int x = first_menu_x(status);

    g_menu_title_count = 0;

    for (i = 0; i < (int)status->menu_count && g_menu_title_count < MAX_TOP_LEVEL_MENUS; ++i) {
        if (!(status->menu[i].flags & MB_TITLE)) continue;

        g_menu_title_x[g_menu_title_count] = x;
        g_menu_title_width[g_menu_title_count] = measure_text(status->menu[i].label);

        x += g_menu_title_width[g_menu_title_count] + MENU_TITLE_GAP;
        ++g_menu_title_count;
    }
}

static void get_dropdown_rect(
    const desktop_status *status,
    int menu_index,
    int *x,
    int *y,
    int *width,
    int *height,
    int *rows
) {
    int title_item = title_item_index(status, menu_index);
    int item_index;
    int row_count = 0;
    int max_width = 90;

    for (item_index = title_item + 1;
         item_index < (int)status->menu_count && !(status->menu[item_index].flags & MB_TITLE);
         ++item_index) {
        int item_width = measure_text(status->menu[item_index].label)
                       + measure_text(status->menu[item_index].shortcut)
                       + 56;

        if (item_width > max_width) max_width = item_width;
        ++row_count;
    }

    *x = (menu_index >= 0 && menu_index < g_menu_title_count)
       ? g_menu_title_x[menu_index] - MENU_TITLE_PADDING_X
       : 0;
    *y = BAR_HEIGHT;
    *width = max_width;
    *height = row_count * MENU_ITEM_HEIGHT + 10;

    if (rows) *rows = row_count;
}

static void get_power_menu_rect(int *x, int *y, int *width, int *height) {
    *width = POWER_MENU_WIDTH;
    *height = 2 * MENU_ITEM_HEIGHT + 10;
    *x = g_screen_width - *width - 6;
    *y = BAR_HEIGHT;
}

/* ------------------------------------------------------------------------- */
/* Rendering                                                                 */
/* ------------------------------------------------------------------------- */

static void draw_top_bar_background(void) {
    fill_rect(0, 0, g_screen_width, BAR_HEIGHT, COLOR_BAR);
    fill_rect(0, BAR_HEIGHT - 1, g_screen_width, 1, COLOR_BORDER);
}

static void draw_app_identity(const desktop_status *status) {
    draw_logo(6, 5);

    if (status->app_label[0]) {
        draw_text(58, 19, status->app_label, COLOR_DIM);
    }
}

static void draw_menu_titles(const desktop_status *status) {
    int i;

    layout_menus(status);

    for (i = 0; i < g_menu_title_count; ++i) {
        int title_item = title_item_index(status, i);
        uint32_t color = (i == g_open_menu_index) ? COLOR_ACCENT : COLOR_TEXT;

        if (i == g_open_menu_index) {
            fill_rect(
                g_menu_title_x[i] - MENU_TITLE_PADDING_X,
                6,
                g_menu_title_width[i] + MENU_TITLE_PADDING_X * 2,
                42,
                COLOR_HIGHLIGHT);
        }

        if (title_item >= 0) {
            draw_text(g_menu_title_x[i], 19, status->menu[title_item].label, color);
        }
    }
}

static void draw_percent_text(int *x, unsigned value) {
    char buffer[16];

    uint_to_string(value, buffer);
    draw_text(*x, 19, buffer, COLOR_TEXT);
    draw_text(*x + string_length(buffer) * 8, 19, "%", COLOR_TEXT);
    *x += 30;
}

static void draw_uptime(int *x, uint32_t uptime_seconds) {
    char time_text[9];

    two_digit_uint_to_string((uptime_seconds / 3600) % 24, &time_text[0]);
    time_text[2] = ':';
    two_digit_uint_to_string((uptime_seconds / 60) % 60, &time_text[3]);
    time_text[5] = ':';
    two_digit_uint_to_string(uptime_seconds % 60, &time_text[6]);
    time_text[8] = 0;

    draw_text(*x, 19, time_text, COLOR_TEXT);
    *x += 8 * 8 + 10;
}

static void draw_status_area(const desktop_status *status) {
    int x = g_screen_width - 470;
    int min_x = first_menu_x(status) + 40;

    if (x < min_x) x = min_x;

    draw_text(x, 19, "NET", status->net_up ? COLOR_GREEN : COLOR_DIM);
    x += 34;

    draw_separator(&x);

    draw_text(x, 19, "CPU", COLOR_TEXT);
    x += 30;
    draw_sparkline(x, 18, 42, 14, g_cpu_history, COLOR_ACCENT);
    x += 48;
    draw_percent_text(&x, status->cpu_pct);

    draw_separator(&x);

    draw_text(x, 19, "UI", COLOR_DIM);
    x += 22;
    draw_sparkline(x, 18, 30, 14, g_ui_history, COLOR_GREEN);
    x += 36;

    draw_separator(&x);

    draw_text(x, 19, "MEM", COLOR_TEXT);
    x += 34;
    fill_rect(x, 22, 30, 8, COLOR_HIGHLIGHT);
    fill_rect(x, 22, 30 * clamp_int((int)status->mem_pct, 0, 100) / 100, 8, COLOR_ACCENT);
    x += 36;
    draw_percent_text(&x, status->mem_pct);

    draw_separator(&x);
    draw_uptime(&x, status->uptime_seconds);

    fill_rect(x, 16, 1, 22, COLOR_BORDER);
    draw_power_icon(g_screen_width - 36, 17);
}

static void draw_menu_dropdown(const desktop_status *status) {
    int x;
    int y;
    int width;
    int height;
    int title_item;
    int item_index;
    int row = 0;

    if (g_open_menu_index < 0 || g_open_menu_index >= g_menu_title_count) return;

    get_dropdown_rect(status, g_open_menu_index, &x, &y, &width, &height, 0);
    fill_rect(x, y, width, height, COLOR_DROPDOWN);
    draw_rect_border(x, y, width, height, COLOR_BORDER);

    title_item = title_item_index(status, g_open_menu_index);

    for (item_index = title_item + 1;
         item_index < (int)status->menu_count && !(status->menu[item_index].flags & MB_TITLE);
         ++item_index, ++row) {
        int item_y = y + DROPDOWN_PADDING_TOP + row * MENU_ITEM_HEIGHT;
        const menubar_item *item = &status->menu[item_index];

        if (item->flags & MB_DIVIDER) {
            fill_rect(x + 8, item_y + MENU_ITEM_HEIGHT / 2, width - 16, 1, COLOR_BORDER);
            continue;
        }

        draw_text(
            x + DROPDOWN_PADDING_X,
            item_y + 5,
            item->label,
            (item->flags & MB_DANGER) ? COLOR_RED : COLOR_TEXT);

        if (item->shortcut[0]) {
            draw_text(
                x + width - DROPDOWN_PADDING_X - measure_text(item->shortcut),
                item_y + 5,
                item->shortcut,
                COLOR_DIM);
        }
    }
}

static void draw_power_menu(void) {
    int x;
    int y;
    int width;
    int height;
    int first_item_y;
    int second_item_y;

    if (!g_power_menu_open) return;

    get_power_menu_rect(&x, &y, &width, &height);
    fill_rect(x, y, width, height, COLOR_DROPDOWN);
    draw_rect_border(x, y, width, height, COLOR_BORDER);

    first_item_y = y + DROPDOWN_PADDING_TOP;
    second_item_y = first_item_y + MENU_ITEM_HEIGHT;

    draw_text(x + DROPDOWN_PADDING_X, first_item_y + 5, "Reboot System", COLOR_TEXT);
    draw_text(x + DROPDOWN_PADDING_X, second_item_y + 5, "Shutdown System", COLOR_RED);
}

static void render(const desktop_status *status) {
    clear_canvas();

    draw_top_bar_background();
    draw_app_identity(status);
    draw_menu_titles(status);
    draw_status_area(status);

    if (g_open_menu_index >= 0) {
        draw_menu_dropdown(status);
    } else {
        draw_power_menu();
    }
}

/* ------------------------------------------------------------------------- */
/* Input                                                                     */
/* ------------------------------------------------------------------------- */

static int point_in_rect(int px, int py, int x, int y, int width, int height) {
    return px >= x && px < x + width && py >= y && py < y + height;
}

static void close_menus(void) {
    g_open_menu_index = -1;
    g_power_menu_open = 0;
}

static int handle_power_menu_click(int mouse_x, int mouse_y) {
    int x;
    int y;
    int width;
    int height;
    int row;

    if (!g_power_menu_open) return 0;

    get_power_menu_rect(&x, &y, &width, &height);
    if (!point_in_rect(mouse_x, mouse_y, x, y, width, height)) return 0;

    row = (mouse_y - y - DROPDOWN_PADDING_TOP) / MENU_ITEM_HEIGHT;

    if (row == 0) {
        __sc1(SYS_REBOOT, 0);
    } else if (row == 1) {
        __sc1(SYS_SHUTDOWN, 0);
    }

    g_power_menu_open = 0;
    return 1;
}

static int handle_open_menu_click(const desktop_status *status, int mouse_x, int mouse_y) {
    int x;
    int y;
    int width;
    int height;
    int rows;
    int row;
    int title_item;
    int item_index;
    int current_row = 0;

    if (g_open_menu_index < 0) return 0;

    get_dropdown_rect(status, g_open_menu_index, &x, &y, &width, &height, &rows);
    if (!point_in_rect(mouse_x, mouse_y, x, y, width, height)) return 0;

    row = (mouse_y - y - DROPDOWN_PADDING_TOP) / MENU_ITEM_HEIGHT;
    title_item = title_item_index(status, g_open_menu_index);

    for (item_index = title_item + 1;
         item_index < (int)status->menu_count && !(status->menu[item_index].flags & MB_TITLE);
         ++item_index, ++current_row) {
        if (current_row != row) continue;

        if (!(status->menu[item_index].flags & MB_DIVIDER)) {
            vos_menu_dispatch(status->menu[item_index].action_id);
        }
        break;
    }

    g_open_menu_index = -1;
    return 1;
}

static int handle_power_button_click(int mouse_x, int mouse_y) {
    if (!point_in_rect(mouse_x, mouse_y, g_screen_width - POWER_HIT_WIDTH, 0, POWER_HIT_WIDTH, BAR_HEIGHT)) {
        return 0;
    }

    g_power_menu_open = 1;
    g_open_menu_index = -1;
    return 1;
}

static int handle_logo_click(int mouse_x, int mouse_y) {
    if (!point_in_rect(mouse_x, mouse_y, 4, 0, 112, BAR_HEIGHT)) {
        return 0;
    }

    vos_spawn("/bin/sysinfo");
    g_open_menu_index = -1;
    return 1;
}

static int handle_menu_title_click(int mouse_x, int mouse_y) {
    int i;

    if (mouse_y >= BAR_HEIGHT) return 0;

    for (i = 0; i < g_menu_title_count; ++i) {
        int x = g_menu_title_x[i] - MENU_TITLE_PADDING_X;
        int width = g_menu_title_width[i] + MENU_TITLE_PADDING_X * 2;

        if (!point_in_rect(mouse_x, mouse_y, x, 0, width, BAR_HEIGHT)) continue;

        g_open_menu_index = (g_open_menu_index == i) ? -1 : i;
        return 1;
    }

    return 0;
}

static void handle_click(const desktop_status *status, int mouse_x, int mouse_y) {
    if (handle_power_menu_click(mouse_x, mouse_y)) return;

    g_power_menu_open = 0;

    if (handle_open_menu_click(status, mouse_x, mouse_y)) return;
    if (handle_power_button_click(mouse_x, mouse_y)) return;
    if (handle_logo_click(mouse_x, mouse_y)) return;
    if (handle_menu_title_click(mouse_x, mouse_y)) return;

    g_open_menu_index = -1;
}

static void update_logo_hover_state(const win_event *event) {
    g_logo_hovered = point_in_rect(event->x, event->y, 6, 5, LOGO_BASE_SIZE, LOGO_BASE_SIZE);
}

static int update_logo_animation(void) {
    if (g_logo_hovered && g_logo_hover_value < LOGO_HOVER_STEPS) {
        ++g_logo_hover_value;
        return 1;
    }

    if (!g_logo_hovered && g_logo_hover_value > 0) {
        --g_logo_hover_value;
        return 1;
    }

    return 0;
}

/* ------------------------------------------------------------------------- */
/* Presentation and main loop                                                */
/* ------------------------------------------------------------------------- */

static void present(void) {
    static int previous_present_height = WINDOW_HEIGHT;
    int current_height = (g_open_menu_index >= 0 || g_power_menu_open) ? WINDOW_HEIGHT : BAR_HEIGHT;
    int present_height = current_height > previous_present_height ? current_height : previous_present_height;
    uint64_t rect_size;

    /*
     * The first present uses the full window height to push TRANSPARENT_KEY into
     * the dropdown area. Later presents can update only the visible strip unless
     * a dropdown is open or has just closed.
     */
    previous_present_height = current_height;
    rect_size = (((uint64_t)(uint32_t)g_screen_width) << 16) | (uint32_t)present_height;

    vos_window_present_rect(g_window_id, g_canvas,
        g_screen_width, WINDOW_HEIGHT,
        0, 0, g_screen_width, (int)(rect_size & 0xffffu));
}

static uint32_t make_render_signature(const desktop_status *status) {
    return status->uptime_seconds
         ^ (status->cpu_pct << 8)
         ^ (status->ui_pct << 14)
         ^ (status->mem_pct << 20)
         ^ ((uint32_t)(g_open_menu_index + 1) << 26)
         ^ ((uint32_t)g_power_menu_open << 30)
         ^ (uint32_t)status->app_label[0]
         ^ status->menu_count;
}

static int create_topbar_window(void) {
    win_options options;

    options.title = "Top Bar";
    options.width = g_screen_width;
    options.height = WINDOW_HEIGHT;
    options.flags = WIN_FRAMELESS
                  | WIN_NO_DOCK
                  | WIN_POSITIONED
                  | WIN_ALWAYS_ON_TOP
                  | WIN_TRANSLUCENT
                  | WIN_NO_SHADOW
                  | VOS_WINDOW_SINGLE_INSTANCE;
    options.x = 0;
    options.y = 0;

    return (int)vos_window_create_ex(&options);
}

static void update_screen_width(void) {
    uint32_t mode = vos_display_mode_get();

    g_screen_width = (int)((mode >> 16) & 0xffffu);
    if (g_screen_width <= 0 || g_screen_width > MAX_WINDOW_WIDTH) {
        g_screen_width = DEFAULT_WINDOW_WIDTH;
    }
}

int main(void) {
    static uint32_t canvas_storage[MAX_WINDOW_WIDTH * WINDOW_HEIGHT];

    /* g_status holds the most recent desktop snapshot.  We only re-fetch it
     * once per second so the expensive kernel roundtrip and the resulting
     * redraws don't happen on every loop iteration (~10× per second). */
    static desktop_status g_status;

    uint32_t previous_signature = 0;
    int      stat_frames        = 0;   /* counts loop iterations since last stat fetch */

    /* STAT_INTERVAL: how many loop iterations between desktop-status fetches.
     * sleep_ticks(10) ≈ 100 ms per iter → 10 iters ≈ 1 second.
     * Keep it at 1 Hz so the sparklines look correct and CPU load is minimal. */
    static const int STAT_INTERVAL = 10;

    update_screen_width();
    g_canvas = canvas_storage;

    load_logo();
    load_power_icon();

    g_window_id = create_topbar_window();
    if (g_window_id < 0) return 1;

    /* Prime the status so the first render has valid data. */
    vos_desktop_status(&g_status);
    sample_status_history(&g_status);
    previous_signature = make_render_signature(&g_status);
    render(&g_status);
    present();

    for (;;) {
        win_event event;
        int redraw = 0;

        /* --- Event handling: always runs at full loop rate --------------- */
        while ((int)vos_event_poll(g_window_id, &event) == 1) {
            if (event.type == EV_MOUSE_DOWN) {
                /* Need current layout for hit-testing — fetch status now. */
                vos_desktop_status(&g_status);
                layout_menus(&g_status);
                handle_click(&g_status, event.x, event.y);
                stat_frames = 0;   /* reset so next periodic fetch is fresh */
                redraw = 1;
            } else if (event.type == EV_MOUSE_MOVE) {
                update_logo_hover_state(&event);
            } else if (event.type == EV_KEY && event.key == 0x1b) {
                close_menus();
                redraw = 1;
            } else if (event.type == EV_CLOSE) {
                return 0;
            } else if (event.type == EV_RESIZE) {
                if (event.x > 0 && event.x <= MAX_WINDOW_WIDTH) {
                    g_screen_width = event.x;
                    close_menus();
                    redraw = 1;
                }
            }
        }

        /* --- Logo hover animation: runs at full rate only when active ---- */
        if (update_logo_animation()) {
            redraw = 1;
        }

        /* --- Stat update: throttled to ~1 Hz ----------------------------- *
         * vos_desktop_status() is a kernel roundtrip that fills a large
         * struct.  Calling it every 100 ms caused the bar to consume
         * disproportionate CPU even when nothing was changing on screen.
         * Fetching once per second is plenty for stat bars and sparklines. */
        if (++stat_frames >= STAT_INTERVAL) {
            stat_frames = 0;

            /* Re-check screen width — resolution may have changed.
             * If it did, we need to re-layout (power button, clock, etc.). */
            int new_w = (int)((vos_display_mode_get() >> 16) & 0xffffu);
            if (new_w > 0 && new_w <= MAX_WINDOW_WIDTH && new_w != g_screen_width) {
                g_screen_width = new_w;
                redraw = 1;
            }

            vos_desktop_status(&g_status);
            sample_status_history(&g_status);

            uint32_t signature = make_render_signature(&g_status);
            if (signature != previous_signature) {
                previous_signature = signature;
                redraw = 1;
            }
        }

        if (redraw) {
            render(&g_status);
            present();
        }

        /* Sleep longer when idle (no animation).  At 100 Hz timer the ticks
         * below map to: animating ≈ 20 ms/frame, idle ≈ 100 ms/frame. */
        sleep_ms((g_logo_hover_value > 0 &&
                     g_logo_hover_value < LOGO_HOVER_STEPS) ? 20 : 100);
    }
}
