/* topbar — the VibeOS desktop top bar, as a userspace app.
 *
 * A full-width, frameless, always-on-top window pinned to y=0 that renders the
 * entire top bar the kernel used to draw: the VibeOS logo (via libsvg), the
 * brand, the focused window's app menus (with dropdowns), the CPU/UI/MEM load
 * indicators with sparklines, the uptime clock and a power glyph.
 *
 * The window is only the bar height plus room for one dropdown; everything
 * outside the bar/dropdown is painted with the transparent key, so the desktop
 * shows through and clicks fall through to the apps below (the kernel routes
 * input past transparent frameless pixels). Data comes from the kernel via
 * SYS_DESKTOP_STATUS once a frame; menu picks go back to the focused window
 * with SYS_MENU_DISPATCH. */

#include "svg.h"

typedef unsigned long  size_t;
typedef long           ssize_t;
typedef unsigned long  uint64_t;
typedef unsigned int   uint32_t;
typedef int            int32_t;
typedef unsigned short uint16_t;
typedef unsigned char  uint8_t;

/* ---- syscalls ---- */
#define SYS_READ 0
#define SYS_WRITE 1
#define SYS_YIELD 3
#define SYS_PROCESS_SPAWN 5
#define SYS_OPEN 7
#define SYS_CLOSE 8
#define SYS_WINDOW_PRESENT 18
#define SYS_EVENT_POLL 19
#define SYS_TIMER_SLEEP 20
#define SYS_DISPLAY_MODE 27
#define SYS_WINDOW_CREATE_EX 35
#define SYS_WINDOW_PRESENT_RECT 38
#define SYS_TEXT_DRAW 40
#define SYS_TEXT_METRICS 41
#define SYS_DESKTOP_STATUS 43
#define SYS_MENU_DISPATCH 44
#define SYS_REBOOT 51
#define SYS_SHUTDOWN 52

static inline ssize_t sc1(uint64_t n, uint64_t a0) {
    ssize_t r; __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "D"(a0) : "rcx","r11","memory"); return r;
}
static inline ssize_t sc2(uint64_t n, uint64_t a0, uint64_t a1) {
    ssize_t r; __asm__ volatile("int $0x80" : "=a"(r) : "a"(n),"D"(a0),"S"(a1) : "rcx","r11","memory"); return r;
}
static inline ssize_t sc3(uint64_t n, uint64_t a0, uint64_t a1, uint64_t a2) {
    ssize_t r; __asm__ volatile("int $0x80" : "=a"(r) : "a"(n),"D"(a0),"S"(a1),"d"(a2) : "rcx","r11","memory"); return r;
}
static inline ssize_t sc6(uint64_t n, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    ssize_t r;
    register uint64_t r10 __asm__("r10") = a3;
    register uint64_t r8  __asm__("r8")  = a4;
    register uint64_t r9  __asm__("r9")  = a5;
    __asm__ volatile("int $0x80" : "=a"(r)
        : "a"(n),"D"(a0),"S"(a1),"d"(a2),"r"(r10),"r"(r8),"r"(r9)
        : "rcx","r11","memory");
    return r;
}
static void nap(uint64_t ticks){ sc1(SYS_TIMER_SLEEP, ticks); }

/* ---- winsys ABI (mirrors kernel/include/winsys.h) ---- */
#define WIN_FRAMELESS 0x1u
#define WIN_NO_DOCK 0x2u
#define WIN_POSITIONED 0x4u
#define WIN_ALWAYS_ON_TOP 0x8u
#define WIN_TRANSLUCENT 0x10u
#define WIN_NO_SHADOW   0x20u
#define TRANSPARENT_KEY 0x00ff00ffu

struct win_options { const char *title; int32_t width, height; uint32_t flags; int32_t x, y; };
struct win_event { uint32_t type; int32_t x, y; uint32_t buttons; uint32_t key; };
#define EV_MOUSE_MOVE 1
#define EV_MOUSE_DOWN 2
#define EV_KEY 4
#define EV_CLOSE 5

#define MB_TITLE 1u
#define MB_DIVIDER 2u
#define MB_DANGER 4u
#define MB_LABEL_MAX 28
#define MB_SHORTCUT_MAX 16
#define MB_MAX_ITEMS 64
#define APP_LABEL_MAX 20

struct menubar_item { char label[MB_LABEL_MAX]; char shortcut[MB_SHORTCUT_MAX]; uint32_t flags; uint32_t action_id; };
struct desktop_status {
    uint32_t uptime_seconds, cpu_pct, ui_pct, mem_pct, net_up;
    char app_label[APP_LABEL_MAX];
    uint32_t menu_count;
    struct menubar_item menu[MB_MAX_ITEMS];
};

/* ---- palette (matches the kernel chrome theme) ---- */
#define COL_BAR     0x001a2c44u
#define COL_BORDER  0x0039506au
#define COL_TEXT    0x00eaf2fau
#define COL_DIM     0x00b7c7d8u
#define COL_ACCENT  0x004da3ffu
#define COL_GREEN   0x0063d9a5u
#define COL_RED     0x00e36c7au
#define COL_HILITE  0x00233850u
#define COL_DROP    0x00203549u

#define BAR_H 54
#define WIN_H 340         /* bar + room for one dropdown */
#define ITEM_H 26

static int g_w;                 /* screen / bar width */
static uint32_t *g_canvas;      /* g_w * WIN_H ARGB */
static int g_win;               /* window id */

/* ---- drawing primitives ---- */
static void clear_canvas(void) {
    int n = g_w * WIN_H, i;
    for (i = 0; i < n; ++i) g_canvas[i] = TRANSPARENT_KEY;
}
static void fill_rect(int x, int y, int w, int h, uint32_t c) {
    int yy, xx;
    for (yy = y; yy < y + h; ++yy) {
        if (yy < 0 || yy >= WIN_H) continue;
        for (xx = x; xx < x + w; ++xx) {
            if (xx < 0 || xx >= g_w) continue;
            g_canvas[yy * g_w + xx] = c;
        }
    }
}
static void blend_px(int x, int y, uint32_t c, int a) {
    uint32_t *d, dr, dg, db, sr, sg, sb;
    if (a <= 0 || x < 0 || y < 0 || x >= g_w || y >= WIN_H) return;
    if (a >= 255) { g_canvas[y * g_w + x] = c; return; }
    d = &g_canvas[y * g_w + x];
    sr = (c >> 16) & 255; sg = (c >> 8) & 255; sb = c & 255;
    dr = (*d >> 16) & 255; dg = (*d >> 8) & 255; db = *d & 255;
    dr = (sr * (uint32_t)a + dr * (255u - a)) / 255u;
    dg = (sg * (uint32_t)a + dg * (255u - a)) / 255u;
    db = (sb * (uint32_t)a + db * (255u - a)) / 255u;
    *d = (dr << 16) | (dg << 8) | db;
}
static int text_w(const char *s) {
    if (!s || !*s) return 0;
    return (int)sc2(SYS_TEXT_METRICS, (uint64_t)(size_t)s, 1);
}
static void draw_text(int x, int y, const char *s, uint32_t c) {
    if (!s || !*s) return;
    sc6(SYS_TEXT_DRAW, (uint64_t)(size_t)g_canvas, (uint64_t)(size_t)s,
        (((uint64_t)(uint32_t)g_w) << 16) | (uint32_t)WIN_H,
        (((uint64_t)(uint16_t)x) << 16) | (uint16_t)y, (uint64_t)c, 1);
}

static int str_len(const char *s) { int n = 0; while (s[n]) ++n; return n; }
static void utoa2(unsigned v, char *out) { out[0] = (char)('0' + (v / 10) % 10); out[1] = (char)('0' + v % 10); out[2] = 0; }
static void uitoa(unsigned v, char *out) {
    char tmp[12]; int n = 0, i = 0;
    if (v == 0) { out[0] = '0'; out[1] = 0; return; }
    while (v) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) out[i++] = tmp[--n];
    out[i] = 0;
}

/* ---- logo (cached SVG, rendered dynamically for anti-aliasing) ---- */
#define LOGO_MAX_SZ 52
static char g_logo_svg_data[4096];
static int g_logo_ready;
static int g_logo_hover = 0;
static int g_logo_hover_val = 0;
static uint32_t g_logo_anim[LOGO_MAX_SZ * LOGO_MAX_SZ];

static void load_logo(void) {
    int fd = (int)sc1(SYS_OPEN, (uint64_t)(size_t)"/icons/vibeos-logo.svg");
    int n;
    if (fd < 0) return;
    n = (int)sc3(SYS_READ, (uint64_t)fd, (uint64_t)(size_t)g_logo_svg_data, sizeof(g_logo_svg_data) - 1);
    sc1(SYS_CLOSE, (uint64_t)fd);
    if (n <= 0) return;
    g_logo_svg_data[n] = 0;
    g_logo_ready = 1;
}
static void blit_logo(int ox, int oy) {
    int x, y;
    if (!g_logo_ready) return;

    int size = 44 + (int)(6.0f * (g_logo_hover_val / 10.0f));
    if (size > LOGO_MAX_SZ) size = LOGO_MAX_SZ;

    int r1 = 0x4d, g1 = 0xa3, b1 = 0xff; // COL_ACCENT
    int r2 = 0xd4, g2 = 0x4d, b2 = 0xff; // Neon Purple / Pink
    int r = r1 + (r2 - r1) * g_logo_hover_val / 10;
    int g = g1 + (g2 - g1) * g_logo_hover_val / 10;
    int b = b1 + (b2 - b1) * g_logo_hover_val / 10;
    uint32_t col = (r << 16) | (g << 8) | b;

    svg_render_rgba(g_logo_svg_data, g_logo_anim, size, col);

    int dox = ox + (44 - size) / 2;
    int doy = oy + (44 - size) / 2;

    for (y = 0; y < size; ++y) {
        for (x = 0; x < size; ++x) {
            uint32_t p = g_logo_anim[y * size + x];
            int a = (int)((p >> 24) & 255u);
            if (a > 0) blend_px(dox + x, doy + y, p & 0xFFFFFFu, a);
        }
    }
}

#define POWER_SZ 20
static uint32_t g_power[POWER_SZ * POWER_SZ];
static int g_power_ready;
static void load_power(void) {
    static char buf[1024];
    int fd = (int)sc1(SYS_OPEN, (uint64_t)(size_t)"/icons/power.svg");
    int n;
    if (fd < 0) return;
    n = (int)sc3(SYS_READ, (uint64_t)fd, (uint64_t)(size_t)buf, sizeof(buf) - 1);
    sc1(SYS_CLOSE, (uint64_t)fd);
    if (n <= 0) return;
    buf[n] = 0;
    svg_render_rgba(buf, g_power, POWER_SZ, COL_TEXT);
    g_power_ready = 1;
}
static void blit_power(int ox, int oy) {
    int x, y;
    if (!g_power_ready) return;
    for (y = 0; y < POWER_SZ; ++y)
        for (x = 0; x < POWER_SZ; ++x) {
            uint32_t p = g_power[y * POWER_SZ + x];
            int a = (int)((p >> 24) & 255u);
            if (a > 0) blend_px(ox + x, oy + y, p & 0xFFFFFFu, a);
        }
}

/* ---- sparklines (app keeps its own per-second history) ---- */
#define HIST 24
static int g_cpu_hist[HIST], g_ui_hist[HIST];
static int g_hist_pos, g_hist_ready;
static uint32_t g_last_sec = 0xffffffffu;

static void sample_history(const struct desktop_status *st) {
    if (st->uptime_seconds == g_last_sec) return;
    g_last_sec = st->uptime_seconds;
    g_cpu_hist[g_hist_pos] = (int)st->cpu_pct;
    g_ui_hist[g_hist_pos] = (int)st->ui_pct;
    g_hist_pos = (g_hist_pos + 1) % HIST;
    if (g_hist_pos == 0) g_hist_ready = 1;
}
static void draw_sparkline(int x, int y, int w, int h, const int *hist, uint32_t c) {
    int count = g_hist_ready ? HIST : g_hist_pos;
    int i, prev_px = -1, prev_py = -1;
    if (count < 2) { fill_rect(x, y + h - 1, w, 1, COL_BORDER); return; }
    for (i = 0; i < count; ++i) {
        int idx = (g_hist_pos - count + i + HIST * 2) % HIST;
        int v = hist[idx]; if (v < 0) v = 0; if (v > 100) v = 100;
        int px = x + (w - 1) * i / (count - 1);
        int py = y + h - 1 - (h - 1) * v / 100;
        if (prev_px >= 0) {
            int dx = px - prev_px, dy = py - prev_py, steps, s;
            int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
            steps = adx > ady ? adx : ady; if (steps < 1) steps = 1;
            for (s = 0; s <= steps; ++s) {
                int lx = prev_px + dx * s / steps;
                int ly = prev_py + dy * s / steps;
                blend_px(lx, ly, c, 255);
                blend_px(lx, ly + 1, c, 90);
            }
        }
        prev_px = px; prev_py = py;
    }
}

/* ---- menu layout / hit testing ---- */
static int g_menu_open = -1;     /* index of open top-level title, -1 = none */
static int g_power_menu_open = 0; /* 1 = power menu dropdown is open, 0 = closed */
static int g_menu_x[16];         /* x of each title */
static int g_menu_w[16];         /* pixel width of each title label */
static int g_menu_n;             /* number of titles */

static int title_index(const struct desktop_status *st, int m) {
    int i, c = 0;
    for (i = 0; i < (int)st->menu_count; ++i)
        if (st->menu[i].flags & MB_TITLE) { if (c == m) return i; ++c; }
    return -1;
}
static int menu_start_x(const struct desktop_status *st) {
    return 122 + text_w(st->app_label) + 24;
}
static void layout_menus(const struct desktop_status *st) {
    int i, x = menu_start_x(st);
    g_menu_n = 0;
    for (i = 0; i < (int)st->menu_count && g_menu_n < 16; ++i) {
        if (!(st->menu[i].flags & MB_TITLE)) continue;
        g_menu_x[g_menu_n] = x;
        g_menu_w[g_menu_n] = text_w(st->menu[i].label);
        x += g_menu_w[g_menu_n] + 22;
        ++g_menu_n;
    }
}
/* Dropdown geometry for the m-th menu (rows = number of entries). */
static void dropdown_rect(const struct desktop_status *st, int m, int *rx, int *ry, int *rw, int *rh, int *rows_out) {
    int ti = title_index(st, m), i, rows = 0, maxw = 90;
    for (i = ti + 1; i < (int)st->menu_count && !(st->menu[i].flags & MB_TITLE); ++i) {
        int lw = text_w(st->menu[i].label) + text_w(st->menu[i].shortcut) + 56;
        if (lw > maxw) maxw = lw;
        ++rows;
    }
    *rx = (m >= 0 && m < g_menu_n) ? g_menu_x[m] - 8 : 0;
    *ry = BAR_H;
    *rw = maxw;
    *rh = rows * ITEM_H + 10;
    if (rows_out) *rows_out = rows;
}

/* ---- rendering ---- */
static void render(const struct desktop_status *st) {
    int i, sx;
    char buf[16];

    clear_canvas();

    /* bar background + bottom hairline */
    fill_rect(0, 0, g_w, BAR_H, COL_BAR);
    fill_rect(0, BAR_H - 1, g_w, 1, COL_BORDER);

    /* logo + brand + focused app label */
    blit_logo(6, 5);
    //draw_text(58, 19, "VibeOS", COL_TEXT);

    if (st->app_label[0]) {
        draw_text(58, 19, st->app_label, COL_DIM);
    }

    /* app menu titles */
    layout_menus(st);
    for (i = 0; i < g_menu_n; ++i) {
        int ti = title_index(st, i);
        if (i == g_menu_open) fill_rect(g_menu_x[i] - 8, 6, g_menu_w[i] + 16, 42, COL_HILITE);
        if (ti >= 0) draw_text(g_menu_x[i], 19, st->menu[ti].label,
                               (i == g_menu_open) ? COL_ACCENT : COL_TEXT);
    }

    /* ---- right side: NET | CPU spark % | UI spark | MEM bar % | clock | power ---- */
    sx = g_w - 470;
    {
        int min_sx = menu_start_x(st) + 40;
        if (sx < min_sx) sx = min_sx;
    }
    draw_text(sx, 19, "NET", st->net_up ? COL_GREEN : COL_DIM); sx += 34;
    fill_rect(sx, 16, 1, 22, COL_BORDER); sx += 8;
    draw_text(sx, 19, "CPU", COL_TEXT); sx += 30;
    draw_sparkline(sx, 18, 42, 14, g_cpu_hist, COL_ACCENT); sx += 48;
    uitoa(st->cpu_pct, buf); draw_text(sx, 19, buf, COL_TEXT);
    draw_text(sx + str_len(buf) * 8, 19, "%", COL_TEXT); sx += 30;
    fill_rect(sx, 16, 1, 22, COL_BORDER); sx += 8;
    draw_text(sx, 19, "UI", COL_DIM); sx += 22;
    draw_sparkline(sx, 18, 30, 14, g_ui_hist, COL_GREEN); sx += 36;
    fill_rect(sx, 16, 1, 22, COL_BORDER); sx += 8;
    draw_text(sx, 19, "MEM", COL_TEXT); sx += 34;
    {
        int pct = (int)st->mem_pct; if (pct > 100) pct = 100;
        fill_rect(sx, 22, 30, 8, COL_HILITE);
        fill_rect(sx, 22, 30 * pct / 100, 8, COL_ACCENT);
        sx += 36;
    }
    uitoa(st->mem_pct, buf); draw_text(sx, 19, buf, COL_TEXT);
    draw_text(sx + str_len(buf) * 8, 19, "%", COL_TEXT); sx += 30;
    fill_rect(sx, 16, 1, 22, COL_BORDER); sx += 8;
    {
        uint32_t s = st->uptime_seconds;
        char t[9];
        utoa2((s / 3600) % 24, &t[0]); t[2] = ':';
        utoa2((s / 60) % 60, &t[3]);   t[5] = ':';
        utoa2(s % 60, &t[6]);          t[8] = 0;
        draw_text(sx, 19, t, COL_TEXT);
        sx += 8 * 8 + 10;
    }
    fill_rect(sx, 16, 1, 22, COL_BORDER);
    blit_power(g_w - 36, 17);

    /* ---- open dropdown ---- */
    if (g_menu_open >= 0 && g_menu_open < g_menu_n) {
        int rx, ry, rw, rh, ti, row = 0, k;
        dropdown_rect(st, g_menu_open, &rx, &ry, &rw, &rh, 0);
        fill_rect(rx, ry, rw, rh, COL_DROP);
        fill_rect(rx, ry, rw, 1, COL_BORDER);
        fill_rect(rx, ry + rh - 1, rw, 1, COL_BORDER);
        fill_rect(rx, ry, 1, rh, COL_BORDER);
        fill_rect(rx + rw - 1, ry, 1, rh, COL_BORDER);
        ti = title_index(st, g_menu_open);
        for (k = ti + 1; k < (int)st->menu_count && !(st->menu[k].flags & MB_TITLE); ++k, ++row) {
            int iy = ry + 5 + row * ITEM_H;
            if (st->menu[k].flags & MB_DIVIDER) {
                fill_rect(rx + 8, iy + ITEM_H / 2, rw - 16, 1, COL_BORDER);
                continue;
            }
            draw_text(rx + 14, iy + 5, st->menu[k].label,
                      (st->menu[k].flags & MB_DANGER) ? COL_RED : COL_TEXT);
            if (st->menu[k].shortcut[0])
                draw_text(rx + rw - 14 - text_w(st->menu[k].shortcut), iy + 5,
                          st->menu[k].shortcut, COL_DIM);
        }
    } else if (g_power_menu_open) {
        int rw = 160;
        int rh = 2 * ITEM_H + 10;
        int rx = g_w - rw - 6;
        int ry = BAR_H;
        fill_rect(rx, ry, rw, rh, COL_DROP);
        fill_rect(rx, ry, rw, 1, COL_BORDER);
        fill_rect(rx, ry + rh - 1, rw, 1, COL_BORDER);
        fill_rect(rx, ry, 1, rh, COL_BORDER);
        fill_rect(rx + rw - 1, ry, 1, rh, COL_BORDER);

        int iy0 = ry + 5;
        draw_text(rx + 14, iy0 + 5, "Reboot System", COL_TEXT);

        int iy1 = ry + 5 + ITEM_H;
        draw_text(rx + 14, iy1 + 5, "Shutdown System", COL_RED);
    }
}

/* ---- input ---- */
static void handle_click(const struct desktop_status *st, int mx, int my) {
    int i;
    if (g_power_menu_open) {
        int rw = 160;
        int rh = 2 * ITEM_H + 10;
        int rx = g_w - rw - 6;
        int ry = BAR_H;
        if (mx >= rx && mx < rx + rw && my >= ry && my < ry + rh) {
            int row = (my - ry - 5) / ITEM_H;
            if (row == 0) {
                sc1(SYS_REBOOT, 0);
            } else if (row == 1) {
                sc1(SYS_SHUTDOWN, 0);
            }
            g_power_menu_open = 0;
            return;
        }
    }

    g_power_menu_open = 0;

    if (g_menu_open >= 0) {       /* a dropdown is showing */
        int rx, ry, rw, rh, rows;
        dropdown_rect(st, g_menu_open, &rx, &ry, &rw, &rh, &rows);
        if (mx >= rx && mx < rx + rw && my >= ry && my < ry + rh) {
            int row = (my - ry - 5) / ITEM_H, ti = title_index(st, g_menu_open), c = 0, k;
            for (k = ti + 1; k < (int)st->menu_count && !(st->menu[k].flags & MB_TITLE); ++k, ++c) {
                if (c == row) {
                    if (!(st->menu[k].flags & MB_DIVIDER))
                        sc1(SYS_MENU_DISPATCH, (uint64_t)st->menu[k].action_id);
                    break;
                }
            }
            g_menu_open = -1;
            return;
        }
    }

    if (my < BAR_H && mx >= g_w - 44) {
        g_power_menu_open = 1;
        g_menu_open = -1;
        return;
    }

    if (my < BAR_H && mx >= 4 && mx < 116) {        /* logo / brand → System Info */
        sc1(SYS_PROCESS_SPAWN, (uint64_t)(size_t)"/bin/sysinfo");
        g_menu_open = -1;
        return;
    }
    for (i = 0; i < g_menu_n; ++i) {                /* a top-level title? */
        if (my < BAR_H && mx >= g_menu_x[i] - 8 && mx < g_menu_x[i] + g_menu_w[i] + 8) {
            g_menu_open = (g_menu_open == i) ? -1 : i;
            return;
        }
    }
    g_menu_open = -1;
}

/* Present only the bar strip, growing to include a dropdown while one is open
 * (and once more to erase it when it closes). */
static void present(void) {
    /* Start at WIN_H so the very first present pushes the whole buffer — that
     * stamps the transparent key into the rows below the bar (otherwise they
     * stay black and cover the desktop). Afterwards only the bar strip is
     * presented, growing to include a dropdown while one is open. */
    static int prev_h = WIN_H;
    int h = (g_menu_open >= 0 || g_power_menu_open) ? WIN_H : BAR_H;
    int ph = h > prev_h ? h : prev_h;
    prev_h = h;
    sc6(SYS_WINDOW_PRESENT_RECT, (uint64_t)g_win, (uint64_t)(size_t)g_canvas,
        (uint64_t)g_w, (uint64_t)WIN_H,
        0u, (((uint64_t)(uint32_t)g_w) << 16) | (uint32_t)ph);
}

int main() {
    uint32_t mode = (uint32_t)sc2(SYS_DISPLAY_MODE, 0, 0);
    static uint32_t canvas_storage[1920 * WIN_H];
    struct win_options opt;
    struct desktop_status st;
    uint32_t sig_prev = 0;

    g_w = (int)((mode >> 16) & 0xffffu);
    if (g_w <= 0 || g_w > 1920) g_w = 1024;
    g_canvas = canvas_storage;

    load_logo();
    load_power();

    opt.title = "Top Bar";
    opt.width = g_w; opt.height = WIN_H;
    opt.flags = WIN_FRAMELESS | WIN_NO_DOCK | WIN_POSITIONED | WIN_ALWAYS_ON_TOP | WIN_TRANSLUCENT | WIN_NO_SHADOW;
    opt.x = 0; opt.y = 0;
    g_win = (int)sc1(SYS_WINDOW_CREATE_EX, (uint64_t)(size_t)&opt);
    if (g_win < 0) return 1;

    for (;;) {
        struct win_event ev;
        int redraw = 0;
        uint32_t sig;

        while ((int)sc2(SYS_EVENT_POLL, (uint64_t)g_win, (uint64_t)(size_t)&ev) == 1) {
            if (ev.type == EV_MOUSE_DOWN) {
                sc1(SYS_DESKTOP_STATUS, (uint64_t)(size_t)&st);
                layout_menus(&st);
                handle_click(&st, ev.x, ev.y);
                redraw = 1;
            } else if (ev.type == EV_MOUSE_MOVE) {
                int hover = (ev.x >= 6 && ev.x < 50 && ev.y >= 5 && ev.y < 49);
                if (hover != g_logo_hover) {
                    g_logo_hover = hover;
                }
            } else if (ev.type == EV_KEY && ev.key == 0x1b) {
                if (g_menu_open >= 0) { g_menu_open = -1; redraw = 1; }
                if (g_power_menu_open) { g_power_menu_open = 0; redraw = 1; }
            } else if (ev.type == EV_CLOSE) {
                return 0;
            }
        }

        int anim_changed = 0;
        if (g_logo_hover) {
            if (g_logo_hover_val < 10) {
                g_logo_hover_val++;
                anim_changed = 1;
            }
        } else {
            if (g_logo_hover_val > 0) {
                g_logo_hover_val--;
                anim_changed = 1;
            }
        }
        if (anim_changed) {
            redraw = 1;
        }

        sc1(SYS_DESKTOP_STATUS, (uint64_t)(size_t)&st);
        sample_history(&st);

        /* Only repaint when something visible changed (clock ticks, load moves,
         * focus/menu changes) so we don't blit the bar every frame. */
        sig = st.uptime_seconds ^ (st.cpu_pct << 8) ^ (st.ui_pct << 14)
            ^ (st.mem_pct << 20) ^ ((uint32_t)(g_menu_open + 1) << 26)
            ^ ((uint32_t)g_power_menu_open << 30)
            ^ (uint32_t)st.app_label[0] ^ st.menu_count;
        if (sig != sig_prev) { redraw = 1; sig_prev = sig; }

        if (redraw) {
            render(&st);
            present();
        }
        if (g_logo_hover_val > 0 && g_logo_hover_val < 10) {
            nap(2);
        } else {
            nap(6);
        }
    }
}
