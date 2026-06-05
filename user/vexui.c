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

/* libvexui — retained-mode implementation. Syscalls + rendering hidden here. */
#include "vexui.h"
#include "svg.h"
/* Keep in sync with MENUBAR_H in vexui.h */
#define MENUBAR_H 22

typedef unsigned long size_t;
typedef long ssize_t;
typedef unsigned long uint64_t;
typedef unsigned int uint32_t;
typedef int int32_t;
typedef unsigned short uint16_t;
typedef unsigned char uint8_t;

/* Text is rasterized by the kernel font atlas via SYS_TEXT_DRAW (see below);
 * the old 8x16 bitmap font (vexui_font.h) is no longer used. */

#define SYS_READ 0
#define SYS_WRITE 1
#define SYS_IOCTL 2
#define SYS_YIELD 3
#define SYS_EXIT 4
#define SYS_PROCESS_SPAWN 5
#define SYS_WAITPID 6
#define SYS_OPEN 7
#define SYS_CLOSE 8
#define SYS_STAT 9
#define SYS_READDIR 10
#define SYS_CHDIR 11
#define SYS_GETCWD 12
#define SYS_UNLINK 13
#define SYS_CREAT 14
#define SYS_GETARG 15
#define SYS_WRITE_FILE 16
#define SYS_WINDOW_CREATE 17
#define SYS_WINDOW_PRESENT 18
#define SYS_EVENT_POLL 19
#define SYS_TIMER_SLEEP 20
#define SYS_GETPID 53
#define SYS_PROCESS_SNAPSHOT 28
#define SYS_PROCESS_KILL 29
#define SYS_WINDOW_SET_MENU 30
#define SYS_WINDOW_CREATE_EX 35
#define SYS_WINDOW_SET_MENUBAR 37
#define SYS_WINDOW_PRESENT_RECT 38
#define SYS_TEXT_DRAW 40
#define SYS_TEXT_METRICS 41

struct winsys_event { uint32_t type; int32_t x; int32_t y; uint32_t buttons; uint32_t key; };
struct vos_menubar_item { char label[28]; char shortcut[16]; vui_u32 flags; vui_u32 action_id; };
#define EV_MOUSE_MOVE 1
#define EV_MOUSE_DOWN 2
#define EV_MOUSE_UP 3
#define EV_KEY 4
#define EV_CLOSE 5
#define EV_CONTEXT_MENU 6
#define EV_MENU_ACTION 7
#define EV_SCROLL 8
#define EV_RESIZE 9

#define WINSYS_MENU_LABEL_MAX 24
#define WINSYS_MAX_MENU_ITEMS 6
struct winsys_menu_item { char label[WINSYS_MENU_LABEL_MAX]; uint32_t action_id; };
struct winsys_window_options {
    const char *title;
    int32_t width;
    int32_t height;
    uint32_t flags;
    int32_t x;
    int32_t y;
    int32_t shadow_inset_top;
};

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
static void do_yield(void){ __asm__ volatile("int $0x80"::"a"((uint64_t)SYS_YIELD):"rcx","r11","memory"); }
/* Sleep for `ticks` scheduler ticks — paces the UI loop so it doesn't busy-spin
 * (which starved the rest of the desktop and made periodic refreshers present
 * far faster than intended). */
static void nap(uint64_t ticks){ sc1(SYS_TIMER_SLEEP, ticks); }

/* Print a message to the controlling terminal (stdout). */
static void emit(const char *s) {
    int n = 0; while (s[n]) n++;
    sc3(SYS_WRITE, 1, (uint64_t)(size_t)s, (uint64_t)n);
}

/* ---- widget model ---- */
enum { W_PANEL, W_LABEL, W_BUTTON, W_BAR, W_VBOX, W_HBOX,
       W_MENUBAR,  /* horizontal bar at the top of the window           */
       W_MENU,     /* one menu title inside a menubar (e.g. "File")     */
       W_MENUITEM, /* one entry inside a dropdown; also separators       */
       W_CARD,
       W_TILE,
       W_INPUT,
       W_BADGE,
       W_CANVAS,
       W_TABS,
       W_SPARK,
       W_PILL,
       W_PILLBTN,
       W_METRIC,
       W_ICON,     /* bare SVG icon/logo, no chrome (transparent bg)     */
       W_LISTITEM, /* sidebar row: left icon + label, hover + selected   */
       W_SLIDER    /* interactive horizontal slider (track + handle)     */
};

static const vui_theme g_default_theme = {
    0x0015273cu, 0x001b3048u, 0x00233850u, 0x0039506au,
    0x005a7da3u, 0x00eaf2fau, 0x00b7c7d8u, 0x004da3ffu,
    0x0063d9a5u, 0x00e6b65cu, 0x00e36c7au,
    0x000c1b2au, 0x00d6e3efu, 0x007f93a8u, 5u, 10u
};
static vui_theme g_theme = {
    0x0015273cu, 0x001b3048u, 0x00233850u, 0x0039506au,
    0x005a7da3u, 0x00eaf2fau, 0x00b7c7d8u, 0x004da3ffu,
    0x0063d9a5u, 0x00e6b65cu, 0x00e36c7au,
    0x000c1b2au, 0x00d6e3efu, 0x007f93a8u, 5u, 10u
};

struct vui_widget {
    int type;
    int x, y, w, h;
    char text[80];
    /* W_METRIC: title (top) + sub-label (under the big value, which is `text`).
     * hist is a ring buffer of recent samples (0..100) for the live chart. */
    char mtitle[24];
    char msub[28];
    unsigned char hist[32];
    unsigned char hist_n;
    unsigned char hist_head;
    vui_u32 color;
    int value, max;
    vui_callback on_click;
    /* W_INPUT: fired only when Enter/Return is pressed (form submit), as opposed
     * to on_click which fires on focus and on every keystroke. */
    vui_callback on_submit;
    void *user;
    int icon_slot;   /* index into the shared SVG icon pool; -1 = none */

    /* anchor / resize system (used by root widgets and vui_set_anchor) */
    uint8_t anchors;
    int margin_l, margin_t, margin_r, margin_b;

    /* container layout (W_VBOX / W_HBOX) */
    int parent_idx; /* index in w->widgets of the parent box; -1 = root        */
    int gap;        /* space between children on the main axis (pixels)         */
    int padding;    /* inner padding on all four sides (pixels)                 */

    /* child layout flags (set on the child widget, read by parent box) */
    uint8_t fill;      /* stretch across the cross-axis to fill the container   */
    uint8_t expand;    /* grow on the main-axis to consume leftover space        */

    /* W_MENUITEM: when non-zero this entry renders as a horizontal divider line
     * rather than a clickable text item; on_click is ignored for separators.   */
    uint8_t separator;

    /* Tooltip text shown as a bubble above the widget on hover. Empty = none. */
    char tooltip[48];

    /* runtime / rendering state */
    uint8_t hover;
    uint8_t pressed;
    uint8_t visible;
    uint8_t running;
};

#define VUI_MAX_WIDGETS 128
#define VUI_MAX_W 900
#define VUI_MAX_H 640

struct vui_window {
    int id, width, height, open;
    int mouse_x, mouse_y, mouse_down;
    vui_u32 clear_color;
    /* Index of the currently open W_MENU widget in widgets[]; -1 = none. */
    int active_menu_idx;
    /* Index of the focused W_INPUT receiving keyboard input; -1 = none. */
    int active_input;
    /* Index of the W_SLIDER currently being dragged; -1 = none. */
    int active_slider;
    int fb_stride;   /* row stride when using bound framebuffer, 0 = fallback */
    int fb_bound;    /* 1 if g_canvas points at a kernel-bound framebuffer  */

    struct vui_widget widgets[VUI_MAX_WIDGETS];
    int widget_count;
    uint8_t dirty;
    vui_tick_callback on_tick;
    vui_resize_callback on_resize;
    vui_context_callback on_context_menu;
    vui_key_callback on_key;
    vui_scroll_callback on_scroll;
    vui_mouse_callback on_mouse_move;
    vui_mouse_callback on_mouse_click;
    vui_mouse_callback on_mouse_release;
    /* Tooltip: index of the hovered widget (-1=none) and a tick counter.
     * The bubble appears after TOOLTIP_DELAY ticks of continuous hover. */
    int  tooltip_widget;
    int  tooltip_ticks;
    vui_menu_callback menu_cbs[WINSYS_MAX_MENU_ITEMS];
    char menu_labels[WINSYS_MAX_MENU_ITEMS][WINSYS_MENU_LABEL_MAX];
    int menu_count;
};

static struct vui_window g_win;
static uint32_t g_canvas_fb[VUI_MAX_W * VUI_MAX_H];
static uint32_t *g_canvas = g_canvas_fb;  /* may be redirected to bound FB */

/* Damage tracking for partial presents: the union of widget rects that changed
 * since the last paint. g_dmg_full forces a whole-window present (used for
 * structural changes: resize, menus, layout). */
static int g_dmg_full = 1;
static int g_dmg_x0, g_dmg_y0, g_dmg_x1, g_dmg_y1;
static void dmg_reset(void){ g_dmg_full = 0; g_dmg_x0 = g_dmg_y0 = 1<<29; g_dmg_x1 = g_dmg_y1 = -(1<<29); }
static void dmg_full(void){ g_dmg_full = 1; }
static void dmg_add(int x, int y, int w, int h){
    if (x < g_dmg_x0) g_dmg_x0 = x;
    if (y < g_dmg_y0) g_dmg_y0 = y;
    if (x + w > g_dmg_x1) g_dmg_x1 = x + w;
    if (y + h > g_dmg_y1) g_dmg_y1 = y + h;
}

const vui_theme *vui_theme_default(void) { return &g_default_theme; }
void vui_set_theme(const vui_theme *theme) {
    if (!theme) return;
    g_theme = *theme;
    g_win.dirty = 1;
}

static int streqn(const char *a, int an, const char *b) {
    int i;
    for (i = 0; i < an; ++i) if (!b[i] || a[i] != b[i]) return 0;
    return b[an] == 0;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parse_hex_color(const char *s, vui_u32 *out) {
    int i, v = 0;
    if (*s == '#') ++s;
    for (i = 0; i < 6; ++i) {
        int h = hexval(s[i]);
        if (h < 0) return 0;
        v = (v << 4) | h;
    }
    *out = (vui_u32)v;
    return 1;
}

static void theme_set_key(vui_theme *t, const char *key, int key_len, vui_u32 color) {
    if (streqn(key, key_len, "bg")) t->bg = color;
    else if (streqn(key, key_len, "surface")) t->surface = color;
    else if (streqn(key, key_len, "surface_hi")) t->surface_hi = color;
    else if (streqn(key, key_len, "border")) t->border = color;
    else if (streqn(key, key_len, "border_hi")) t->border_hi = color;
    else if (streqn(key, key_len, "text")) t->text = color;
    else if (streqn(key, key_len, "text_dim")) t->text_dim = color;
    else if (streqn(key, key_len, "accent")) t->accent = color;
    else if (streqn(key, key_len, "ok")) t->ok = color;
    else if (streqn(key, key_len, "warn")) t->warn = color;
    else if (streqn(key, key_len, "danger")) t->danger = color;
    else if (streqn(key, key_len, "menu_bg")) t->menu_bg = color;
    else if (streqn(key, key_len, "menu_item")) t->menu_item = color;
    else if (streqn(key, key_len, "menu_muted")) t->menu_muted = color;
}

int vui_load_theme(const char *path) {
    char buf[1024];
    int fd, n, i = 0;
    vui_theme next = g_theme;
    return -1;   /* Keep the built-in theme until file-backed themes are stable. */
    if (!path) return -1;
    fd = (int)sc1(SYS_OPEN, (uint64_t)(size_t)path);
    if (fd < 0) return -1;
    n = (int)sc3(SYS_READ, (uint64_t)fd, (uint64_t)(size_t)buf, (uint64_t)(sizeof(buf) - 1));
    sc1(SYS_CLOSE, (uint64_t)fd);
    if (n <= 0) return -1;
    buf[n] = 0;
    while (i < n) {
        int key = i, key_len, val;
        vui_u32 color;
        while (i < n && (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\n' || buf[i] == '\r')) ++i;
        if (i >= n) break;
        if (buf[i] == '#') { while (i < n && buf[i] != '\n') ++i; continue; }
        key = i;
        while (i < n && buf[i] != '=' && buf[i] != '\n' && buf[i] != '\r') ++i;
        key_len = i - key;
        while (key_len > 0 && (buf[key + key_len - 1] == ' ' || buf[key + key_len - 1] == '\t')) --key_len;
        if (i >= n || buf[i] != '=') { while (i < n && buf[i] != '\n') ++i; continue; }
        ++i;
        while (i < n && (buf[i] == ' ' || buf[i] == '\t')) ++i;
        val = i;
        if (parse_hex_color(&buf[val], &color)) theme_set_key(&next, &buf[key], key_len, color);
        while (i < n && buf[i] != '\n') ++i;
    }
    vui_set_theme(&next);
    return 0;
}

/* ---- small helpers ---- */
static int slen(const char *s){ int n=0; while(s&&s[n]) n++; return n; }
static void scopy(char *dst, const char *src, int cap){ int i=0; for(;src&&src[i]&&i<cap-1;i++) dst[i]=src[i]; dst[i]=0; }
static int sequal(const char *a, const char *b){ int i=0; if(!a||!b)return a==b; for(;a[i]&&a[i]==b[i];++i){} return a[i]==b[i]; }

static uint32_t mix(uint32_t a, uint32_t b, unsigned step, unsigned total) {
    uint32_t ar = (a >> 16) & 0xffu, ag = (a >> 8) & 0xffu, ab = a & 0xffu;
    uint32_t br = (b >> 16) & 0xffu, bg = (b >> 8) & 0xffu, bb = b & 0xffu;
    return ((((ar * (total - step)) + (br * step)) / total) << 16) |
           ((((ag * (total - step)) + (bg * step)) / total) << 8) |
           (((ab * (total - step)) + (bb * step)) / total);
}

static vui_u32 argb(vui_u32 color, unsigned alpha) {
    if (alpha > 255u) alpha = 255u;
    return ((vui_u32)alpha << 24) | (color & 0x00ffffffu);
}

static vui_u32 glass(vui_u32 color, unsigned alpha) {
    return argb(color, alpha);
}

/* ---- canvas drawing ---- */
/* Final, hard bound on every g_canvas access: returns the pixel index, or -1 if
 * it would land outside the backing array. Defends against a corrupted
 * w->width/height (which would otherwise turn y*w->width+x into a wild offset
 * and fault the renderer); with this guard a bad geometry skips pixels instead
 * of reading/writing out of bounds. */
static long canvas_index(struct vui_window *w, int x, int y) {
    long i;
    if (x < 0 || y < 0 || x >= w->width || y >= w->height) return -1;
    i = (long)y * (long)w->width + (long)x;
    if (i < 0 || i >= (long)((long)VUI_MAX_W * (long)VUI_MAX_H)) return -1;
    return i;
}
static void rect(struct vui_window *w, int x, int y, int wid, int hgt, vui_u32 c) {
    int iy, ix;
    for (iy=y; iy<y+hgt; ++iy) { if(iy<0||iy>=w->height) continue;
        for (ix=x; ix<x+wid; ++ix) { long i=canvas_index(w,ix,iy); if(i<0) continue; g_canvas[i]=c; } }
}
static void put(struct vui_window *w,int x,int y,vui_u32 c){ long i=canvas_index(w,x,y); if(i<0)return; g_canvas[i]=c; }
static void line_h(struct vui_window *w, int x, int y, int wid, vui_u32 c) { rect(w, x, y, wid, 1, c); }
static void line_v(struct vui_window *w, int x, int y, int hgt, vui_u32 c) { rect(w, x, y, 1, hgt, c); }
static int isqrt_i(int v){ int r=0; while((r+1)*(r+1)<=v) ++r; return r; }
/* Filled rounded rectangle (pill). Corner pixels outside the radius are left at
 * the window's clear color so a frameless transparent window shows the desktop
 * through them. Draws a 1px border in `border`. */
/* Solid rounded-rect fill (no border). Corner pixels outside the radius are
 * left untouched (caller's background shows through there). */
static void rrect_fill(struct vui_window *w,int x,int y,int wid,int hgt,int r,vui_u32 c){
    int iy;
    if (wid<=0||hgt<=0) return;
    if (r<0) r=0;
    if (r>hgt/2) r=hgt/2;
    if (r>wid/2) r=wid/2;
    for (iy=0; iy<hgt; ++iy){
        int dy=0, inset=0, py=y+iy;
        if (iy<r) dy=r-iy; else if (iy>=hgt-r) dy=iy-(hgt-r)+1;
        if (dy>0){ int ch=isqrt_i(r*r-dy*dy); inset=r-ch; }
        rect(w, x+inset, py, wid-2*inset, 1, c);
    }
}
/* Alpha-blend a colour over the existing canvas pixel (a = 0..255). */
static void blend_put(struct vui_window *w,int x,int y,vui_u32 c,int a){
    long i;
    if (a<=0) return;
    i = canvas_index(w, x, y);
    if (i < 0) return;
    if (a>=255){ g_canvas[i]=c; return; }
    {
        vui_u32 *d=&g_canvas[i];
        /* Only keep the per-pixel alpha (so the compositor can blend the window
         * over the desktop) when the window is actually translucent, i.e. its
         * clear colour is the transparent sentinel. For an OPAQUE window the
         * compositor ignores the alpha byte, so writing argb(c,a) for an
         * anti-aliased edge pixel shows full-opaque colour — no AA, leaving
         * icon/shape edges jagged on the solid background. There we must bake
         * the AA in by blending the colour over the solid background. */
        if (*d == w->clear_color && w->clear_color == VUI_COLOR_TRANSPARENT) {
            *d = argb(c, (unsigned)a);
            return;
        }
        vui_u32 src_rb = c & 0x00ff00ffu;
        vui_u32 dest_rb = *d & 0x00ff00ffu;
        vui_u32 rb = ((src_rb * (vui_u32)a + dest_rb * (256u - (vui_u32)a)) >> 8) & 0x00ff00ffu;

        vui_u32 src_g = c & 0x0000ff00u;
        vui_u32 dest_g = *d & 0x0000ff00u;
        vui_u32 g = ((src_g * (vui_u32)a + dest_g * (256u - (vui_u32)a)) >> 8) & 0x0000ff00u;

        *d = rb | g;
    }
}
/* Is a fixed-point sub-pixel inside the rounded rect? Coordinates are scaled
 * by 4, so 2x2 samples can use offsets 1 and 3 without floating point. */
static int rr_in4(int px4,int py4,int x,int y,int wid,int hgt,int r){
    int x4 = x * 4, y4 = y * 4, x24 = (x + wid) * 4, y24 = (y + hgt) * 4;
    int xr4 = (x + r) * 4, yr4 = (y + r) * 4;
    int xwr4 = (x + wid - r) * 4, yhr4 = (y + hgt - r) * 4;
    int cx4, cy4, dx4, dy4;
    long dist2, rr4;

    if (px4 < x4 || py4 < y4 || px4 > x24 || py4 > y24) return 0;
    if (r <= 0) return 1;
    if (px4 >= xr4 && px4 <= xwr4) return 1;
    if (py4 >= yr4 && py4 <= yhr4) return 1;
    cx4 = (px4 < xr4) ? xr4 : xwr4;
    cy4 = (py4 < yr4) ? yr4 : yhr4;
    dx4 = px4 - cx4;
    dy4 = py4 - cy4;
    dist2 = (long)dx4 * (long)dx4 + (long)dy4 * (long)dy4;
    rr4 = (long)(r * 4) * (long)(r * 4);
    return dist2 <= rr4;
}
/* Coverage 0..4 of a pixel, via 2x2 supersampling (corner anti-aliasing). */
static int rr_cov(int px,int py,int x,int y,int wid,int hgt,int r){
    static const int o[2]={1,3};
    int i,j,c=0;
    for(i=0;i<2;++i) for(j=0;j<2;++j)
        c += rr_in4(px * 4 + o[j], py * 4 + o[i], x, y, wid, hgt, r);
    return c;
}
/* Anti-aliased rounded-rect fill: full coverage in the interior/straight edges
 * (crisp), blended coverage only on the corner arcs (smooth, not "krisselig"). */
static void rrect_fill_aa(struct vui_window *w,int x,int y,int wid,int hgt,int r,vui_u32 c){
    int iy,ix;
    if (wid<=0||hgt<=0) return;
    if (r<0) r=0; if (r>hgt/2) r=hgt/2; if (r>wid/2) r=wid/2;
    if (r <= 0) { rrect_fill(w,x,y,wid,hgt,r,c); return; }
    for (iy=0; iy<hgt; ++iy){
        int py=y+iy;
        int corner = (iy<r) || (iy>=hgt-r);
        if (!corner){ rect(w, x, py, wid, 1, c); continue; }   /* straight rows: solid */
        for (ix=0; ix<wid; ++ix){
            int cov=rr_cov(x+ix,py,x,y,wid,hgt,r);
            if (cov==4) put(w, x+ix, py, c);
            else if (cov>0) blend_put(w, x+ix, py, c, cov*255/4);
        }
    }
}
/* Rounded rect with an anti-aliased 1px outline (straight sides stay crisp). */
static void fill_round_rect(struct vui_window *w,int x,int y,int wid,int hgt,int r,vui_u32 fill,vui_u32 border){
    if (border == fill){ rrect_fill_aa(w,x,y,wid,hgt,r,fill); return; }
    rrect_fill_aa(w, x, y, wid, hgt, r, border);
    rrect_fill_aa(w, x+1, y+1, wid-2, hgt-2, r>1?r-1:0, fill);
}
static void glass_box(struct vui_window *w, int x, int y, int wid, int hgt, vui_u32 fill, int strong) {
    vui_u32 body = glass(fill, strong ? 242u : 224u);
    vui_u32 top = glass(strong ? mix(g_theme.border_hi, 0x00ffffffu, 1u, 3u)
                               : mix(g_theme.border_hi, fill, 1u, 2u), strong ? 210u : 170u);
    vui_u32 edge = glass(strong ? g_theme.border_hi : g_theme.border, strong ? 190u : 150u);
    vui_u32 bot = glass(mix(0x00000000u, fill, 1u, 3u), 120u);
    if (wid <= 0 || hgt <= 0) return;
    fill_round_rect(w, x, y, wid, hgt, g_theme.radius + (strong ? 2 : 0), body, edge);
    line_h(w, x + 1, y, wid - 2, top);
    line_h(w, x + 1, y + hgt - 1, wid - 2, bot);
    line_v(w, x, y + 1, hgt - 2, edge);
    line_v(w, x + wid - 1, y + 1, hgt - 2, edge);
    if (g_theme.radius > 1 && wid > 5 && hgt > 5) {
        put(w, x, y, w->clear_color);
        put(w, x + wid - 1, y, w->clear_color);
        put(w, x, y + hgt - 1, w->clear_color);
        put(w, x + wid - 1, y + hgt - 1, w->clear_color);
    }
}
/* Text now routes through the kernel's anti-aliased TrueType atlas (SYS_TEXT_DRAW)
 * instead of the old 8x16 bitmap font: the kernel rasterizes straight into our
 * g_canvas buffer (our address space is active during the syscall). Newlines and
 * clipping are handled kernel-side. scale 1..3 picks the atlas size. */
static void draw_text_sys(struct vui_window *w,int x,int y,const char *s,vui_u32 c,int sc){
    if(!s||!*s) return;
    if(sc<1) sc=1; else if(sc>3) sc=3;
    sc6(SYS_TEXT_DRAW,
        (uint64_t)(size_t)g_canvas,
        (uint64_t)(size_t)s,
        (((uint64_t)(uint32_t)w->width)<<16)|(uint32_t)w->height,
        (((uint64_t)(uint16_t)x)<<16)|(uint16_t)y,
        (uint64_t)c,
        (uint64_t)sc);
}
static void text(struct vui_window *w,int x,int y,const char *s,vui_u32 c){
    draw_text_sys(w,x,y,s,c,1);
}
static void text_scaled(struct vui_window *w,int x,int y,const char *s,vui_u32 c,int sc){
    draw_text_sys(w,x,y,s,c,sc);
}
/* Proportional pixel width of a string at the given atlas scale, queried from
 * the kernel so widget layout matches the rasterized glyphs. */
static int text_px(const char *s,int sc){
    if(!s||!*s) return 0;
    if(sc<1) sc=1; else if(sc>3) sc=3;
    return (int)sc2(SYS_TEXT_METRICS,(uint64_t)(size_t)s,(uint64_t)sc);
}
/* 2px-thick line (Bresenham-ish) for line sparklines. */
static void draw_line(struct vui_window *w,int x0,int y0,int x1,int y1,vui_u32 c){
    int dx=x1-x0, dy=y1-y0;
    int adx=dx<0?-dx:dx, ady=dy<0?-dy:dy;
    int steps=adx>ady?adx:ady, i; if(steps<1)steps=1;
    for(i=0;i<=steps;++i){ int x=x0+dx*i/steps, y=y0+dy*i/steps; put(w,x,y,c); put(w,x,y+1,c); }
}
/* Filled-area line chart in box (x,y,wd,ht) from `n` samples (0..100, oldest
 * first): shaded area fading from an accent tint at the line to the surface,
 * with a bright line on top. */
static void area_chart(struct vui_window *w,int x,int y,int wd,int ht,
                       const unsigned char *samp,int n,vui_u32 line){
    vui_u32 ftop = mix(line, g_theme.surface, 1u, 2u);
    vui_u32 fbot = mix(line, g_theme.surface, 1u, 8u);
    int i,px=-1,py=0;
    if(n<2) return;
    for(i=0;i<n;++i){
        int cx=x+i*(wd-1)/(n-1);
        int v=samp[i]; if(v>100)v=100;
        int cy=y+(ht-1)-v*(ht-2)/100;          /* samples are 0..100 */
        int span=(y+ht)-cy; if(span<1)span=1;
        int seg; for(seg=cy; seg<y+ht; ++seg){
            vui_u32 c=mix(ftop, fbot, (vui_u32)(seg-cy), (vui_u32)span);
            put(w,cx,seg,c); if(cx+1<x+wd) put(w,cx+1,seg,c);
        }
        if(px>=0) draw_line(w,px,py,cx,cy,line);
        px=cx; py=cy;
    }
}
static void line_diag(struct vui_window *w,int x,int y,int len,int dx,int dy,vui_u32 c){
    int i; for(i=0;i<len;++i){ put(w,x+i*dx,y+i*dy,c); put(w,x+i*dx+1,y+i*dy,c); }
}

/* Render an SVG icon/logo via libsvg into a scratch RGBA buffer, then blit it
 * onto the window canvas at (x,y) with per-pixel alpha. `color` serves as the
 * SVG `currentColor`. The raster work lives in lib/svg so other tools can
 * reuse it. */
#define VUI_ICON_SLOT_SZ 4096
struct svg_cache_entry {
    char svg_content[VUI_ICON_SLOT_SZ];
    int size;
    vui_u32 color;
    unsigned int buffer[SVG_MAX_DIM * SVG_MAX_DIM];
    int valid;
};
/* One slot per distinct (svg, size, colour). Sized so a whole app's icon set
 * (e.g. the audioplayer's 8 transport/header icons) stays resident — otherwise
 * the cache thrashes and every repaint re-rasterises (and re-supersamples)
 * every icon, which is expensive. */
#define SVG_CACHE_SLOTS 16
static struct svg_cache_entry g_svg_cache[SVG_CACHE_SLOTS];
static int g_svg_cache_next = 0;

static void draw_svg_icon(struct vui_window *w, int x, int y, int size,
                          const char *svg, vui_u32 color) {
    int px, py;
    if (!svg || size <= 0) return;
    if (size > SVG_MAX_DIM) size = SVG_MAX_DIM;

    unsigned int *cached_buf = 0;
    int i;
    for (i = 0; i < SVG_CACHE_SLOTS; ++i) {
        if (g_svg_cache[i].valid && g_svg_cache[i].size == size && g_svg_cache[i].color == color) {
            if (sequal(g_svg_cache[i].svg_content, svg)) {
                cached_buf = g_svg_cache[i].buffer;
                break;
            }
        }
    }

    if (!cached_buf) {
        int slot = g_svg_cache_next;
        g_svg_cache_next = (g_svg_cache_next + 1) % SVG_CACHE_SLOTS;

        int len = 0;
        while (svg[len] && len < VUI_ICON_SLOT_SZ - 1) {
            g_svg_cache[slot].svg_content[len] = svg[len];
            ++len;
        }
        g_svg_cache[slot].svg_content[len] = '\0';

        g_svg_cache[slot].size = size;
        g_svg_cache[slot].color = color;

        /* Render once at the display size into the cache (the svg rasteriser has
         * its own 1px analytic AA, so a direct render stays crisp). Cached in
         * RAM and just blitted on every subsequent repaint. */
        svg_render_rgba(svg, g_svg_cache[slot].buffer, size, (unsigned int)color);
        g_svg_cache[slot].valid = 1;

        cached_buf = g_svg_cache[slot].buffer;
    }

    for (py = 0; py < size; ++py) {
        for (px = 0; px < size; ++px) {
            unsigned int p = cached_buf[py * size + px];
            int a = (int)((p >> 24) & 255u);
            if (a > 0) blend_put(w, x + px, y + py, p & 0xFFFFFFu, a);
        }
    }
}

/* Large monochrome outline icons (2px stroke) for the dock, matching the
 * reference: thin line-art glyphs that sit directly on the dock bar. */
static void tile_icon(struct vui_window *w,int cx,int cy,int id,vui_u32 c){
    switch(id){
    case 1: { /* globe / web */
        int s=14;
        rect(w, cx-s+3, cy-s,   2*s-6, 2, c);   /* top    */
        rect(w, cx-s+3, cy+s-1, 2*s-6, 2, c);   /* bottom */
        rect(w, cx-s,   cy-s+3, 2, 2*s-6, c);   /* left   */
        rect(w, cx+s-1, cy-s+3, 2, 2*s-6, c);   /* right  */
        rect(w, cx-s,   cy-1,   2*s, 2, c);     /* equator  */
        rect(w, cx-1,   cy-s,   2, 2*s, c);     /* meridian */
        break; }
    case 2: { /* monitor with an activity chart */
        int s=14;
        rect(w, cx-s,   cy-s+1, 2*s, 2, c);     /* screen top    */
        rect(w, cx-s,   cy+s-6, 2*s, 2, c);     /* screen bottom */
        rect(w, cx-s,   cy-s+1, 2, 2*s-7, c);   /* screen left   */
        rect(w, cx+s-2, cy-s+1, 2, 2*s-7, c);   /* screen right  */
        rect(w, cx-2,   cy+s-5, 4, 4, c);       /* stand neck    */
        rect(w, cx-8,   cy+s,   16, 2, c);      /* stand base    */
        line_diag(w, cx-9, cy+4, 6, 1, -1, c);  /* chart zigzag  */
        line_diag(w, cx-3, cy-2, 4, 1,  1, c);
        line_diag(w, cx+1, cy+2, 7, 1, -1, c);
        break; }
    case 3: { /* apps grid 2x2 */
        int q=10;
        rect(w, cx-q-2, cy-q-2, q, q, c);
        rect(w, cx+2,   cy-q-2, q, q, c);
        rect(w, cx-q-2, cy+2,   q, q, c);
        rect(w, cx+2,   cy+2,   q, q, c);
        break; }
    case 4: { /* code  <>  */
        line_diag(w, cx-9, cy, 9, 1, -1, c);    /* "<" upper arm */
        line_diag(w, cx-9, cy, 9, 1,  1, c);    /* "<" lower arm */
        line_diag(w, cx+10, cy, 9, -1, -1, c);  /* ">" upper arm */
        line_diag(w, cx+10, cy, 9, -1,  1, c);  /* ">" lower arm */
        break; }
    case 5: { /* folder */
        int s=14;
        rect(w, cx-s,   cy-6, 2, 16, c);        /* left  */
        rect(w, cx+s,   cy-2, 2, 12, c);        /* right */
        rect(w, cx-s,   cy+9, 2*s+2, 2, c);     /* bottom */
        rect(w, cx-s,   cy-6, 9, 2, c);         /* back-top */
        rect(w, cx-s+8, cy-9, 2, 4, c);
        rect(w, cx-s+8, cy-9, s+2, 2, c);       /* front-top */
        break; }
    default: break;
    }
}

/* ---- widget creation ---- */
static struct vui_widget *new_widget(struct vui_window *w, int type) {
    struct vui_widget *wd;
    if (w->widget_count >= VUI_MAX_WIDGETS) return 0;
    wd = &w->widgets[w->widget_count++];
    wd->type = type; wd->x=wd->y=wd->w=wd->h=0; wd->text[0]=0;
    wd->color = VUI_TEXT; wd->value=0; wd->max=100; wd->on_click=0; wd->on_submit=0; wd->user=0; wd->icon_slot=-1;
    wd->anchors = VUI_ANCHOR_LEFT | VUI_ANCHOR_TOP;
    wd->margin_l=wd->margin_t=wd->margin_r=wd->margin_b=0;
    wd->parent_idx=-1; wd->gap=4; wd->padding=0;
    wd->fill=0; wd->expand=0; wd->separator=0;
    wd->hover=0; wd->pressed=0; wd->visible=1; wd->running=0;
    return wd;
}

/* Helper: snapshot current position into margins so layout_widget keeps it. */
static void init_margins(struct vui_widget *wd) {
    wd->margin_l = wd->x; wd->margin_t = wd->y;
    wd->margin_r = g_win.width  - (wd->x + wd->w);
    wd->margin_b = g_win.height - (wd->y + wd->h);
}

vui_widget *vui_panel(vui_window *w, int x, int y, int width, int height, const char *title) {
    vui_widget *wd = new_widget(w, W_PANEL);
    if (!wd) return 0;
    wd->x=x; wd->y=y; wd->w=width; wd->h=height; scopy(wd->text, title?title:"", sizeof(wd->text));
    init_margins(wd); w->dirty=1; return wd;
}
vui_widget *vui_card(vui_window *w, int x, int y, int width, int height, const char *title) {
    vui_widget *wd = new_widget(w, W_CARD);
    if (!wd) return 0;
    wd->x=x; wd->y=y; wd->w=width; wd->h=height; scopy(wd->text, title?title:"", sizeof(wd->text));
    init_margins(wd); w->dirty=1; return wd;
}
vui_widget *vui_label(vui_window *w, int x, int y, const char *t) {
    vui_widget *wd = new_widget(w, W_LABEL);
    if (!wd) return 0;
    wd->x=x; wd->y=y; scopy(wd->text, t, sizeof(wd->text)); wd->w=text_px(wd->text,1); wd->h=16; wd->color=VUI_TEXT;
    init_margins(wd); w->dirty=1; return wd;
}
vui_widget *vui_button(vui_window *w, int x, int y, const char *t) {
    vui_widget *wd = new_widget(w, W_BUTTON);
    if (!wd) return 0;
    wd->x=x; wd->y=y; scopy(wd->text, t, sizeof(wd->text));
    wd->w = text_px(wd->text,1) + 26; wd->h = 26; wd->color = VUI_ACCENT;
    init_margins(wd); w->dirty=1; return wd;
}
vui_widget *vui_tile_button(vui_window *w, int x, int y, const char *t) {
    vui_widget *wd = new_widget(w, W_TILE);
    if (!wd) return 0;
    wd->x=x; wd->y=y; wd->w=58; wd->h=58; wd->color=VUI_ACCENT;
    scopy(wd->text, t?t:"", sizeof(wd->text));
    init_margins(wd); w->dirty=1; return wd;
}
vui_widget *vui_image(vui_window *w, int x, int y, int size) {
    vui_widget *wd = new_widget(w, W_ICON);
    if (!wd) return 0;
    wd->x=x; wd->y=y; wd->w=size; wd->h=size; wd->color=VUI_ACCENT;
    init_margins(wd); w->dirty=1; return wd;
}
vui_widget *vui_listitem(vui_window *w, int x, int y, int width, int height, const char *label) {
    vui_widget *wd = new_widget(w, W_LISTITEM);
    if (!wd) return 0;
    wd->x=x; wd->y=y; wd->w=width; wd->h=height;
    scopy(wd->text, label?label:"", sizeof(wd->text));
    wd->color = g_theme.accent;        /* selection/indicator accent */
    init_margins(wd); w->dirty=1; return wd;
}
vui_widget *vui_input(vui_window *w, int x, int y, int width, const char *placeholder) {
    vui_widget *wd = new_widget(w, W_INPUT);
    if (!wd) return 0;
    wd->x=x; wd->y=y; wd->w=width; wd->h=34;
    scopy(wd->mtitle, placeholder?placeholder:"", sizeof(wd->mtitle));  /* placeholder */
    wd->text[0]=0;                                                      /* typed value */
    init_margins(wd); w->dirty=1; return wd;
}
vui_widget *vui_badge(vui_window *w, int x, int y, const char *t) {
    vui_widget *wd = new_widget(w, W_BADGE);
    if (!wd) return 0;
    wd->x=x; wd->y=y; scopy(wd->text, t?t:"", sizeof(wd->text));
    wd->w=text_px(wd->text,1)+18; wd->h=20; wd->color=VUI_ACCENT;
    init_margins(wd); w->dirty=1; return wd;
}
vui_widget *vui_canvas(vui_window *w, int x, int y, int width, int height, vui_u32 *pixels) {
    return vui_canvas_ex(w, x, y, width, height, pixels, width);
}
vui_widget *vui_canvas_ex(vui_window *w, int x, int y, int width, int height, vui_u32 *pixels, int stride) {
    vui_widget *wd = new_widget(w, W_CANVAS);
    if (!wd) return 0;
    wd->x=x; wd->y=y; wd->w=width; wd->h=height;
    wd->user = (void*)pixels;
    wd->value = stride;
    init_margins(wd); w->dirty=1; return wd;
}
vui_widget *vui_tabs(vui_window *w, int x, int y, int width, const char *labels, int active) {
    vui_widget *wd = new_widget(w, W_TABS);
    if (!wd) return 0;
    wd->x=x; wd->y=y; wd->w=width; wd->h=34; scopy(wd->text, labels?labels:"", sizeof(wd->text));
    wd->value=active; init_margins(wd); w->dirty=1; return wd;
}
vui_widget *vui_pill_button(vui_window *w, int x, int y, const char *t) {
    vui_widget *wd = new_widget(w, W_PILLBTN);
    if (!wd) return 0;
    wd->x=x; wd->y=y; scopy(wd->text, t?t:"", sizeof(wd->text));
    wd->w = text_px(wd->text,1) + 28; wd->h = 32; wd->color = 0;
    init_margins(wd); w->dirty=1; return wd;
}
vui_widget *vui_pill(vui_window *w, int x, int y, int width, int height) {
    vui_widget *wd = new_widget(w, W_PILL);
    if (!wd) return 0;
    wd->x=x; wd->y=y; wd->w=width; wd->h=height; wd->color=g_theme.surface;
    init_margins(wd); w->dirty=1; return wd;
}
/* Metric card: a self-contained card with a title, a big value, a sub-label and
 * a chart/progress indicator — drawn as ONE widget. `mode`: 0 = area chart,
 * 1 = progress bar (value via vui_set_value, 0..100). */
vui_widget *vui_metric(vui_window *w, int x, int y, int width, int height,
                       const char *title, int mode) {
    vui_widget *wd = new_widget(w, W_METRIC);
    if (!wd) return 0;
    wd->x=x; wd->y=y; wd->w=width; wd->h=height;
    scopy(wd->mtitle, title?title:"", sizeof(wd->mtitle));
    wd->text[0]=0; wd->msub[0]=0; wd->color=g_theme.accent;
    wd->max = mode;           /* reuse max as the chart mode */
    init_margins(wd); w->dirty=1; return wd;
}
/* Set a metric's big value + sub-label (e.g. "12%", "2.1 GHz"). */
void vui_set_metric(vui_widget *wd, const char *value, const char *sub) {
    if (!wd) return;
    if (sequal(wd->text, value?value:"") && sequal(wd->msub, sub?sub:"")) return;
    scopy(wd->text, value?value:"", sizeof(wd->text));
    scopy(wd->msub,  sub?sub:"",    sizeof(wd->msub));
    dmg_add(wd->x, wd->y, wd->w, wd->h);
    g_win.dirty=1;
}
/* Append a live sample (0..100) to a metric's history chart. */
void vui_metric_push(vui_widget *wd, int sample) {
    unsigned cap;
    if (!wd) return;
    if (sample < 0) sample = 0; if (sample > 100) sample = 100;
    cap = (unsigned)sizeof(wd->hist);
    wd->hist[wd->hist_head] = (unsigned char)sample;
    wd->hist_head = (unsigned char)((wd->hist_head + 1) % cap);
    if (wd->hist_n < cap) wd->hist_n++;
    dmg_add(wd->x, wd->y, wd->w, wd->h);
    g_win.dirty = 1;
}
vui_widget *vui_sparkline(vui_window *w, int x, int y, int width, int height) {
    vui_widget *wd = new_widget(w, W_SPARK);
    if (!wd) return 0;
    wd->x=x; wd->y=y; wd->w=width; wd->h=height; wd->color=VUI_ACCENT;
    init_margins(wd); w->dirty=1; return wd;
}
vui_widget *vui_bar(vui_window *w, int x, int y, int width, int height, int max) {
    vui_widget *wd = new_widget(w, W_BAR);
    if (!wd) return 0;
    wd->x=x; wd->y=y; wd->w=width; wd->h=height; wd->max=max>0?max:1; wd->color=VUI_ACCENT;
    init_margins(wd); w->dirty=1; return wd;
}

/* Interactive horizontal slider: a thin track with a draggable handle. value
 * runs 0..max; on_click fires whenever a drag/click changes it. The hit area
 * is the full widget height, so make it tall enough (~16px) to grab easily. */
vui_widget *vui_slider(vui_window *w, int x, int y, int width, int height, int max) {
    vui_widget *wd = new_widget(w, W_SLIDER);
    if (!wd) return 0;
    wd->x=x; wd->y=y; wd->w=width; wd->h=height; wd->max=max>0?max:1;
    wd->value=0; wd->color=VUI_ACCENT;
    init_margins(wd); w->dirty=1; return wd;
}

void vui_on_click(vui_widget *b, vui_callback cb){ if(b) b->on_click=cb; }
void vui_on_submit(vui_widget *b, vui_callback cb){ if(b) b->on_submit=cb; }
void vui_set_text(vui_widget *wd, const char *t){
    if(!wd)return;
    /* Dirty-on-change: skip the repaint entirely when the text is unchanged, so
     * periodic refreshers (e.g. the task manager) don't re-present every tick. */
    if(sequal(wd->text, t?t:"")) return;
    dmg_add(wd->x, wd->y, wd->w, wd->h);   /* old extent */
    scopy(wd->text,t,sizeof(wd->text));
    /* Only auto-size widgets that own their geometry (root widgets). Box
     * children keep the width their container assigned (e.g. fixed table
     * columns), so changing the text must not reflow the column. */
    if(wd->parent_idx == -1){
        if(wd->type==W_BUTTON && !(wd->anchors & VUI_ANCHOR_RIGHT)) wd->w=text_px(wd->text,1)+24;
        if(wd->type==W_LABEL) wd->w=text_px(wd->text, wd->value>1?wd->value:1);
        if(wd->type==W_BADGE) wd->w=text_px(wd->text,1)+18;
    }
    dmg_add(wd->x, wd->y, wd->w, wd->h);   /* new extent */
    g_win.dirty=1;
}
/* Integer text scale for a label (1 = normal 8x16, 2 = double, …). */
void vui_set_text_scale(vui_widget *wd, int scale){
    if(!wd || wd->type!=W_LABEL) return;
    if(scale<1) scale=1;
    if(wd->value==scale) return;
    wd->value=scale;
    wd->w=text_px(wd->text,scale); wd->h=16*scale;
    dmg_add(wd->x,wd->y,wd->w,wd->h); g_win.dirty=1;
}
void vui_set_int(vui_widget *wd, int v){
    char b[16]; int i=0; unsigned n; char t[12]; int k=0;
    if(!wd) return;
    if(v<0){ b[i++]='-'; n=(unsigned)(-v); } else n=(unsigned)v;
    if(n==0) t[k++]='0'; else while(n){ t[k++]=(char)('0'+n%10); n/=10; }
    while(k) b[i++]=t[--k];
    b[i]=0;
    vui_set_text(wd,b);
}
int vui_get_int(vui_widget *wd){ return wd ? wd->value : 0; }
void vui_set_value(vui_widget *wd, int v){ if(!wd||wd->value==v)return; wd->value=v; dmg_add(wd->x,wd->y,wd->w,wd->h); g_win.dirty=1; }
int  vui_get_value(vui_widget *wd){ return wd ? wd->value : 0; }
void vui_set_color(vui_widget *wd, vui_u32 c){ if(!wd||wd->color==c)return; wd->color=c; dmg_add(wd->x,wd->y,wd->w,wd->h); g_win.dirty=1; }
/* Shared SVG icon storage. Widgets reference a slot by index instead of each
 * carrying a large inline buffer (logos can be a couple of KB). */
#define VUI_ICON_SLOTS   32   /* enough for icon-rich apps (file browser sidebar) */
static char g_icon_store[VUI_ICON_SLOTS][VUI_ICON_SLOT_SZ];
static unsigned char g_icon_used[VUI_ICON_SLOTS];

/* The widget's icon slot, allocating one on first use (-1 if the pool is full). */
static int icon_slot_for(struct vui_widget *wd){
    int i;
    if(wd->icon_slot >= 0) return wd->icon_slot;
    for(i = 0; i < VUI_ICON_SLOTS; ++i)
        if(!g_icon_used[i]){ g_icon_used[i] = 1; wd->icon_slot = i; return i; }
    return -1;
}

/* The widget's SVG text, or NULL if it has none. */
static const char *widget_icon(struct vui_widget *wd){
    if(wd->icon_slot < 0 || !g_icon_store[wd->icon_slot][0]) return 0;
    return g_icon_store[wd->icon_slot];
}

void vui_set_icon_svg(vui_widget *wd, const char *svg){
    int s;
    if(!wd) return;
    s = icon_slot_for(wd);
    if(s < 0) return;
    scopy(g_icon_store[s], svg ? svg : "", VUI_ICON_SLOT_SZ);
    dmg_add(wd->x,wd->y,wd->w,wd->h);
    g_win.dirty=1;
}
int vui_set_icon_svg_path(vui_widget *wd, const char *path){
    int fd, n, s;
    if(!wd || !path) return -1;
    s = icon_slot_for(wd);
    if(s < 0) return -1;
    fd = (int)sc1(SYS_OPEN, (uint64_t)(size_t)path);
    if(fd < 0) { 
        g_icon_store[s][0] = 0; 
        /* Log error to serial for debugging */
        const char *err = "vexui: failed to open SVG icon: ";
        sc3(SYS_WRITE, 1, (uint64_t)(size_t)err, (uint64_t)slen(err));
        sc3(SYS_WRITE, 1, (uint64_t)(size_t)path, (uint64_t)slen(path));
        sc3(SYS_WRITE, 1, (uint64_t)(size_t)"\n", 1);
        return fd; 
    }
    n = (int)sc3(SYS_READ, (uint64_t)fd, (uint64_t)(size_t)g_icon_store[s],
                 (uint64_t)(VUI_ICON_SLOT_SZ - 1));
    sc1(SYS_CLOSE, (uint64_t)fd);
    if(n <= 0) { 
        g_icon_store[s][0] = 0; 
        const char *err = "vexui: failed to read SVG icon: ";
        sc3(SYS_WRITE, 1, (uint64_t)(size_t)err, (uint64_t)slen(err));
        sc3(SYS_WRITE, 1, (uint64_t)(size_t)path, (uint64_t)slen(path));
        sc3(SYS_WRITE, 1, (uint64_t)(size_t)"\n", 1);
        return -1; 
    }
    if(n >= VUI_ICON_SLOT_SZ) n = VUI_ICON_SLOT_SZ - 1;
    g_icon_store[s][n] = 0;
    dmg_add(wd->x,wd->y,wd->w,wd->h);
    g_win.dirty=1;
    return 0;
}
void vui_set_user(vui_widget *wd, void *u){ if(wd) wd->user=u; }
void *vui_get_user(vui_widget *wd){ return wd?wd->user:0; }
const char *vui_input_text(vui_widget *wd){ return wd ? wd->text : ""; }
void vui_set_visible(vui_widget *wd, int visible){ uint8_t v=(uint8_t)(visible?1:0); if(!wd||wd->visible==v)return; wd->visible=v; dmg_add(wd->x,wd->y,wd->w,wd->h); g_win.dirty=1; }
void vui_set_running(vui_widget *wd, int running){ uint8_t r=(uint8_t)(running?1:0); if(!wd||wd->running==r)return; wd->running=r; dmg_add(wd->x,wd->y,wd->w,wd->h); g_win.dirty=1; }
void vui_set_bounds(vui_widget *wd, int x, int y, int width, int height){
    if(!wd)return;
    wd->x=x; wd->y=y; wd->w=width; wd->h=height;
    wd->margin_l=wd->x; wd->margin_t=wd->y;
    wd->margin_r=g_win.width-(wd->x+wd->w);
    wd->margin_b=g_win.height-(wd->y+wd->h);
    g_win.dirty=1;
}
/* Set only the size without changing the position or margins.
 * Useful when a widget's initial size must differ from its text-derived default
 * but the position will be controlled by a box container. */
void vui_set_size(vui_widget *wd, int width, int height) {
    if (!wd) return;
    if (width  > 0) wd->w = width;
    if (height > 0) wd->h = height;
    wd->margin_r = g_win.width  - (wd->x + wd->w);
    wd->margin_b = g_win.height - (wd->y + wd->h);
    g_win.dirty = 1;
}
void vui_set_button_width(vui_widget *wd, int width){
    if(!wd||wd->type!=W_BUTTON)return;
    if(width<24)width=24;
    wd->w=width;
    wd->margin_r=g_win.width-(wd->x+wd->w);
    wd->margin_b=g_win.height-(wd->y+wd->h);
    g_win.dirty=1;
}
void vui_set_anchor(vui_widget *wd, int anchors){
    if(!wd)return;
    if(!anchors) anchors = VUI_ANCHOR_LEFT | VUI_ANCHOR_TOP;
    wd->anchors=(uint8_t)anchors;
    wd->margin_l=wd->x; wd->margin_t=wd->y;
    wd->margin_r=g_win.width-(wd->x+wd->w);
    wd->margin_b=g_win.height-(wd->y+wd->h);
    g_win.dirty=1;
}
void vui_set_clear_color(vui_window *w, vui_u32 color){ if(w){ w->clear_color=color; w->dirty=1; } }
int vui_window_width(vui_window *w){ return w?w->width:0; }
int vui_window_height(vui_window *w){ return w?w->height:0; }
/* Return the current framebuffer pointer (may be a kernel-bound direct
 * mapping after BIND_FB, or the static fallback buffer).  Canvas-based
 * apps can draw directly into this buffer for zero-copy rendering. */
uint32_t *vui_canvas_ptr(vui_window *w) {
    (void)w;
    return g_canvas;
}

void vui_on_tick(vui_window *w, vui_tick_callback cb){ if(w) w->on_tick=cb; }
void vui_on_resize(vui_window *w, vui_resize_callback cb){ if(w) w->on_resize=cb; }
void vui_on_context_menu(vui_window *w, vui_context_callback cb){ if(w) w->on_context_menu=cb; }
void vui_on_key(vui_window *w, vui_key_callback cb){ if(w) w->on_key=cb; }
void vui_set_tooltip(vui_widget *wd, const char *tip){
    if (!wd) return;
    if (!tip) { wd->tooltip[0] = 0; return; }
    scopy(wd->tooltip, tip, sizeof(wd->tooltip));
}
void vui_on_scroll(vui_window *w, vui_scroll_callback cb){ if(w) w->on_scroll=cb; }
void vui_on_mouse_move(vui_window *w, vui_mouse_callback cb){ if(w) w->on_mouse_move=cb; }
void vui_on_mouse_click(vui_window *w, vui_mouse_callback cb){ if(w) w->on_mouse_click=cb; }
void vui_on_mouse_release(vui_window *w, vui_mouse_callback cb){ if(w) w->on_mouse_release=cb; }
void vui_request_repaint(vui_window *w){ if(w) w->dirty=1; }

/* Update only one canvas widget's region on screen: copy its pixels into the
 * window framebuffer and present just that rectangle (SYS_WINDOW_PRESENT_RECT).
 * Lets an app animate one area (e.g. a visualiser) every frame without a full
 * repaint of the static chrome around it — the chrome stays in the framebuffer
 * from its last full repaint and is left untouched. */
void vui_canvas_flush(vui_window *w, vui_widget *canvas) {
    if (!w || !canvas || canvas->type != W_CANVAS) return;
    uint32_t *pixels = (uint32_t *)canvas->user;
    int stride = canvas->value;
    if (!pixels || stride <= 0) return;
    int x0 = canvas->x, y0 = canvas->y, cw = canvas->w, ch = canvas->h;

    /* When the framebuffer is kernel-bound, the app writes directly into
     * content_storage (no intermediate g_canvas).  The copy loop below is
     * skipped and we just mark the dirty rectangle. */
    if (w->fb_bound) {
        if (x0 < 0) { cw += x0; x0 = 0; }
        if (y0 < 0) { ch += y0; y0 = 0; }
        if (x0 + cw > w->width)  cw = w->width  - x0;
        if (y0 + ch > w->height) ch = w->height - y0;
        if (cw > 0 && ch > 0)
            sc3(66, (uint64_t)w->id, (uint64_t)(((x0 & 0xffff) << 16) | (y0 & 0xffff)), (uint64_t)(((cw & 0xffff) << 16) | (ch & 0xffff)));
        return;
    }

    /* Fallback: copy canvas pixels into the local g_canvas, then present. */
    for (int iy = 0; iy < ch; ++iy) {
        int py = y0 + iy;
        if (py < 0 || py >= w->height) continue;
        for (int ix = 0; ix < cw; ++ix) {
            long di = canvas_index(w, x0 + ix, py);
            if (di < 0) continue;
            g_canvas[di] = pixels[iy * stride + ix];
        }
    }
    if (x0 < 0) { cw += x0; x0 = 0; }
    if (y0 < 0) { ch += y0; y0 = 0; }
    if (x0 + cw > w->width)  cw = w->width  - x0;
    if (y0 + ch > w->height) ch = w->height - y0;
    if (cw <= 0 || ch <= 0) return;
    sc6(SYS_WINDOW_PRESENT_RECT, (uint64_t)w->id, (uint64_t)(size_t)g_canvas,
        (uint64_t)w->width, (uint64_t)w->height,
        ((uint64_t)x0 << 16) | (uint64_t)y0,
        ((uint64_t)cw << 16) | (uint64_t)ch);
}

/* =========================================================================
 * Box containers — vui_vbox / vui_hbox
 *
 * Containers are invisible widgets that control the position and size of
 * their children.  Apps create a container, then call vui_box_add() to
 * assign child widgets to it.  On every repaint the layout engine runs a
 * second pass that computes each child's final geometry.
 *
 * Main axis   — the axis along which children are stacked
 *               (vertical for VBox, horizontal for HBox)
 * Cross axis  — the perpendicular axis
 *
 * Child sizing flags:
 *   vui_set_expand(child)  — child grows to fill leftover main-axis space
 *                            (multiple expand children share the space evenly)
 *   vui_set_fill(child)    — child stretches to the full cross-axis size
 *                            of the container's inner area
 * ========================================================================= */

/* Helper: find the widget index for a given pointer. */
static int widget_index(vui_window *w, vui_widget *wd) {
    int i;
    for (i = 0; i < w->widget_count; ++i)
        if (&w->widgets[i] == wd) return i;
    return -1;
}

vui_widget *vui_vbox(vui_window *w, int x, int y, int width, int height) {
    vui_widget *wd = new_widget(w, W_VBOX);
    if (!wd) return 0;
    wd->x=x; wd->y=y; wd->w=width; wd->h=height;
    init_margins(wd);
    w->dirty=1; return wd;
}

vui_widget *vui_hbox(vui_window *w, int x, int y, int width, int height) {
    vui_widget *wd = new_widget(w, W_HBOX);
    if (!wd) return 0;
    wd->x=x; wd->y=y; wd->w=width; wd->h=height;
    init_margins(wd);
    w->dirty=1; return wd;
}

/* Assign child to container.  child must already exist in the same window. */
void vui_box_add(vui_widget *container, vui_widget *child) {
    int idx;
    if (!container || !child) return;
    idx = widget_index(&g_win, container);
    if (idx < 0) return;
    child->parent_idx = idx;
    g_win.dirty = 1;
}

/* Spacing between children on the main axis. */
void vui_set_gap(vui_widget *wd, int gap)         { if (wd) { wd->gap     = gap;     g_win.dirty=1; } }

/* Inner padding on all four sides of the container. */
void vui_set_padding(vui_widget *wd, int padding)  { if (wd) { wd->padding = padding; g_win.dirty=1; } }

/* Child flag: stretch to fill the container's full cross-axis width/height. */
void vui_set_fill(vui_widget *wd)                  { if (wd) { wd->fill    = 1;       g_win.dirty=1; } }

/* Child flag: grow on the main axis to consume leftover space. */
void vui_set_expand(vui_widget *wd)                { if (wd) { wd->expand  = 1;       g_win.dirty=1; } }

/* =========================================================================
 * In-window menu bar  (W_MENUBAR / W_MENU / W_MENUITEM)
 *
 * Usage pattern:
 *
 *   vui_widget *mb   = vui_menubar(win);
 *   vui_widget *file = vui_menu(win, mb, "File");
 *   vui_on_click(vui_menuitem(win, file, "New"),  on_new_cb);
 *   vui_on_click(vui_menuitem(win, file, "Open"), on_open_cb);
 *   vui_menu_separator(win, file);
 *   vui_on_click(vui_menuitem(win, file, "Quit"), on_quit_cb);
 *
 * The menu bar always stretches to the full window width.
 * Reserve the top MENUBAR_H pixels of content for it.
 * ========================================================================= */

/* ---- helpers (used by layout, draw and event loop) ---------------------- */

/* Minimum dropdown panel width, derived from the widest non-separator item. */
static int dropdown_width(struct vui_window *w, int menu_idx) {
    int max_w = 80, i;
    for (i = 0; i < w->widget_count; ++i) {
        struct vui_widget *it = &w->widgets[i];
        if (it->type != W_MENUITEM || it->parent_idx != menu_idx || it->separator) continue;
        int tw = text_px(it->text, 1) + 24; /* 12 px left padding + text + 12 px right */
        if (tw > max_w) max_w = tw;
    }
    return max_w;
}

/* Total pixel height of the dropdown panel for menu_idx. */
static int dropdown_height(struct vui_window *w, int menu_idx) {
    int h = 8, i; /* 4 px top + 4 px bottom padding */
    for (i = 0; i < w->widget_count; ++i) {
        struct vui_widget *it = &w->widgets[i];
        if (it->type != W_MENUITEM || it->parent_idx != menu_idx) continue;
        h += it->separator ? 9  /* 4 px gap + 1 px line + 4 px gap */
                           : 22 /* item row height                  */;
    }
    return h;
}

/* ---- layout ------------------------------------------------------------- */

/* Position W_MENU titles inside their W_MENUBAR (called every repaint). */
static void layout_menubar(struct vui_window *w, int bar_idx) {
    struct vui_widget *bar = &w->widgets[bar_idx];
    int x = bar->x + 4, i; /* small left inset */

    /* Always stretch to full window width. */
    bar->w = w->width;
    bar->margin_r = 0;

    for (i = 0; i < w->widget_count; ++i) {
        struct vui_widget *m = &w->widgets[i];
        if (m->type != W_MENU || m->parent_idx != bar_idx) continue;
        m->x = x;
        m->y = bar->y;
        m->w = text_px(m->text, 1) + 16;
        m->h = bar->h;
        m->margin_l = m->x;
        x += m->w;
    }
}

/* Set x/y/w/h on W_MENUITEM widgets for the currently open menu so that
 * inside() works for hit-testing without separate coordinate math. */
static void layout_menu_dropdown(struct vui_window *w) {
    int menu_idx = w->active_menu_idx;
    if (menu_idx < 0) return;

    struct vui_widget *menu = &w->widgets[menu_idx];
    int dw     = dropdown_width(w, menu_idx);
    int item_y = menu->y + menu->h + 4; /* 4 px top padding inside panel */
    int dx     = menu->x;
    int i;

    /* Clamp to window right edge so the panel never overflows. */
    if (dx + dw > w->width) dx = w->width - dw;
    if (dx < 0) dx = 0;

    for (i = 0; i < w->widget_count; ++i) {
        struct vui_widget *it = &w->widgets[i];
        if (it->type != W_MENUITEM || it->parent_idx != menu_idx) continue;

        it->x = dx;
        it->w = dw;
        if (it->separator) {
            it->y = item_y + 4;
            it->h = 1;
            item_y += 9;
        } else {
            it->y = item_y;
            it->h = 22;
            item_y += 22;
        }
    }
}

/* ---- public API --------------------------------------------------------- */

/* Create the menu bar.  It sits at y=0 and is MENUBAR_H px tall.
 * Other widgets should start at y >= MENUBAR_H. */
/* Register all W_MENU widgets as the window-server menu bar so the top bar can
 * display the focused app's menus.  Call once after building the menu bar. */
void vui_sync_menubar(vui_window *w) {
#define VUI_MAX_MENUBAR_ITEMS 64
    struct vos_menubar_item items[VUI_MAX_MENUBAR_ITEMS];
    int count = 0, i, j;
    if (!w) return;
    for (i = 0; i < w->widget_count && count < VUI_MAX_MENUBAR_ITEMS; ++i) {
        struct vui_widget *wd = &w->widgets[i];
        if (wd->type != W_MENU) continue;
        
        /* Add top-level menu title */
        scopy(items[count].label, wd->text, sizeof(items[count].label));
        items[count].shortcut[0] = 0;
        items[count].flags = 1u; /* VOS_MB_TITLE = 1u */
        items[count].action_id = (vui_u32)(i + 1000);
        ++count;
        
        /* Find W_MENUITEMs belonging to this menu title */
        for (j = 0; j < w->widget_count && count < VUI_MAX_MENUBAR_ITEMS; ++j) {
            struct vui_widget *it = &w->widgets[j];
            if (it->type != W_MENUITEM || it->parent_idx != i) continue;
            scopy(items[count].label, it->text, sizeof(items[count].label));
            items[count].shortcut[0] = 0;
            items[count].flags = it->separator ? 2u : 0; /* VOS_MB_DIVIDER = 2u */
            items[count].action_id = (vui_u32)(j + 1000);
            ++count;
        }
    }
    sc3(SYS_WINDOW_SET_MENUBAR, (uint64_t)w->id,
        (uint64_t)(size_t)items, (uint64_t)count);

    /* Hide the local in-window menubar & menus so they are not drawn inside the window */
    for (i = 0; i < w->widget_count; ++i) {
        struct vui_widget *wd = &w->widgets[i];
        if (wd->type == W_MENUBAR || wd->type == W_MENU || wd->type == W_MENUITEM) {
            wd->visible = 0;
        }
    }
#undef VUI_MAX_MENUBAR_ITEMS
}

vui_widget *vui_menubar(vui_window *w) {
    vui_widget *wd = new_widget(w, W_MENUBAR);
    if (!wd) return 0;
    wd->x = 0; wd->y = 0;
    wd->w = w->width; wd->h = MENUBAR_H;
    /* Anchor left+right so it stretches when the window is resized. */
    wd->anchors = VUI_ANCHOR_LEFT | VUI_ANCHOR_TOP | VUI_ANCHOR_RIGHT;
    wd->margin_l = 0; wd->margin_r = 0; wd->margin_t = 0;
    w->dirty = 1; return wd;
}

/* Add a dropdown menu to a menu bar.  The returned widget accepts items via
 * vui_menuitem() / vui_menu_separator(). */
vui_widget *vui_menu(vui_window *w, vui_widget *menubar, const char *title) {
    vui_widget *wd = new_widget(w, W_MENU);
    if (!wd) return 0;
    scopy(wd->text, title ? title : "", sizeof(wd->text));
    /* Position is computed by layout_menubar; set a placeholder size. */
    wd->x = 0; wd->y = 0;
    wd->w = text_px(wd->text, 1) + 16;
    wd->h = MENUBAR_H;
    wd->parent_idx = widget_index(w, menubar);
    w->dirty = 1; return wd;
}

/* Add a clickable item to a menu.  Register the action with vui_on_click(). */
vui_widget *vui_menuitem(vui_window *w, vui_widget *menu, const char *label) {
    vui_widget *wd = new_widget(w, W_MENUITEM);
    if (!wd) return 0;
    scopy(wd->text, label ? label : "", sizeof(wd->text));
    wd->parent_idx = widget_index(w, menu);
    w->dirty = 1; return wd;
}

/* Add a horizontal separator line to a menu (no callback). */
vui_widget *vui_menu_separator(vui_window *w, vui_widget *menu) {
    vui_widget *wd = new_widget(w, W_MENUITEM);
    if (!wd) return 0;
    wd->separator  = 1;
    wd->parent_idx = widget_index(w, menu);
    w->dirty = 1; return wd;
}

/* ---- dock context menu (existing API, kept with a clearer name) --------- */
void vui_add_dock_item(vui_window *w, const char *label, vui_menu_callback cb) {
    struct winsys_menu_item items[WINSYS_MAX_MENU_ITEMS];
    int i;
    if (!w || w->menu_count >= WINSYS_MAX_MENU_ITEMS) {
        return;
    }
    w->menu_cbs[w->menu_count] = cb;
    scopy(w->menu_labels[w->menu_count], label, WINSYS_MENU_LABEL_MAX);
    w->menu_count++;
    /* The kernel replaces the whole menu table on each call, so resend every
     * entry from our app-side store. Each entry's action_id is its index, so
     * the kernel hands it straight back via EV_MENU_ACTION. */
    for (i = 0; i < w->menu_count; ++i) {
        scopy(items[i].label, w->menu_labels[i], WINSYS_MENU_LABEL_MAX);
        items[i].action_id = (uint32_t)i;
    }
    sc3(SYS_WINDOW_SET_MENU, (uint64_t)w->id, (uint64_t)(size_t)items, (uint64_t)w->menu_count);
}

int vui_process_snapshot(unsigned int slot, vui_process_info *out) {
    return (int)sc2(SYS_PROCESS_SNAPSHOT, (uint64_t)slot, (uint64_t)(size_t)out);
}

int vui_process_kill(unsigned int pid) {
    return (int)sc1(SYS_PROCESS_KILL, (uint64_t)pid);
}

static void layout_widget(struct vui_window *w, struct vui_widget *wd) {
    int right = w->width - wd->margin_r;
    int bottom = w->height - wd->margin_b;

    if ((wd->anchors & VUI_ANCHOR_LEFT) && (wd->anchors & VUI_ANCHOR_RIGHT)) {
        wd->x = wd->margin_l;
        wd->w = right - wd->x;
        if (wd->w < 1) wd->w = 1;
    } else if (wd->anchors & VUI_ANCHOR_RIGHT) {
        wd->x = right - wd->w;
    } else {
        wd->x = wd->margin_l;
    }

    if ((wd->anchors & VUI_ANCHOR_TOP) && (wd->anchors & VUI_ANCHOR_BOTTOM)) {
        wd->y = wd->margin_t;
        wd->h = bottom - wd->y;
        if (wd->h < 1) wd->h = 1;
    } else if (wd->anchors & VUI_ANCHOR_BOTTOM) {
        wd->y = bottom - wd->h;
    } else {
        wd->y = wd->margin_t;
    }
}

/* ---- Box layout pass --------------------------------------------------- */

/* Compute the position and size of all children belonging to box[box_idx].
 *
 * Algorithm (same for VBox and HBox, just on different axes):
 *
 *  1. Walk children in creation order to count them and sum fixed sizes.
 *  2. Divide remaining main-axis space equally among expand children.
 *  3. Walk children again in order to place each one.
 *
 * Cross-axis: fill children stretch to the container's inner width/height;
 * non-fill children keep their current size and are left-/top-aligned.
 */
static void layout_box(struct vui_window *w, int box_idx) {
    struct vui_widget *box = &w->widgets[box_idx];
    int horiz = (box->type == W_HBOX);
    int pad   = box->padding;
    int gap   = box->gap;
    int i;

    /* Pass A: measure (hidden children take no space) */
    int n_children = 0, n_expand = 0, fixed_sum = 0;
    for (i = 0; i < w->widget_count; ++i) {
        if (w->widgets[i].parent_idx != box_idx) continue;
        if (!w->widgets[i].visible) continue;    /* hidden = no slot */
        n_children++;
        if (w->widgets[i].expand)
            n_expand++;
        else
            fixed_sum += horiz ? w->widgets[i].w : w->widgets[i].h;
    }
    if (n_children == 0) return;

    int total_gaps   = (n_children - 1) * gap;
    int main_inner   = (horiz ? box->w : box->h) - 2 * pad;
    int cross_inner  = (horiz ? box->h : box->w) - 2 * pad;
    if (main_inner  < 0) main_inner  = 0;
    if (cross_inner < 0) cross_inner = 0;

    int free_space   = main_inner - total_gaps - fixed_sum;
    if (free_space   < 0) free_space = 0;
    int expand_share = n_expand > 0 ? free_space / n_expand : 0;

    /* Pass B: place (skip hidden children — they contribute neither space nor gaps) */
    int cursor = (horiz ? box->x : box->y) + pad;
    for (i = 0; i < w->widget_count; ++i) {
        struct vui_widget *ch = &w->widgets[i];
        if (ch->parent_idx != box_idx) continue;
        if (!ch->visible) continue;

        int main_sz  = ch->expand ? expand_share
                                  : (horiz ? ch->w : ch->h);
        int cross_sz = ch->fill   ? cross_inner
                                  : (horiz ? ch->h : ch->w);
        if (main_sz  < 0) main_sz  = 0;
        if (cross_sz < 0) cross_sz = 0;

        if (horiz) {
            ch->x = cursor;
            ch->y = box->y + pad;
            ch->w = main_sz;
            ch->h = cross_sz;
        } else {
            ch->x = box->x + pad;
            ch->y = cursor;
            ch->w = cross_sz;
            ch->h = main_sz;
        }

        /* Sync margins so that a subsequent anchor pass does not overwrite. */
        ch->margin_l = ch->x;
        ch->margin_t = ch->y;
        ch->margin_r = g_win.width  - (ch->x + ch->w);
        ch->margin_b = g_win.height - (ch->y + ch->h);

        cursor += main_sz + gap;
    }
}

/* ---- Two-phase layout --------------------------------------------------- */

static void layout_widgets(struct vui_window *w) {
    int i;

    /* Phase 1: anchor-based resize for root widgets (no container parent).
     * This repositions and resizes everything attached to window edges. */
    for (i = 0; i < w->widget_count; ++i) {
        if (w->widgets[i].parent_idx == -1)
            layout_widget(w, &w->widgets[i]);
    }

    /* Phase 2: box container layout (outer before inner by index order). */
    for (i = 0; i < w->widget_count; ++i) {
        int t = w->widgets[i].type;
        if (t == W_VBOX || t == W_HBOX)
            layout_box(w, i);
    }

    /* Phase 3: menu bar — stretch titles across the bar. */
    for (i = 0; i < w->widget_count; ++i) {
        if (w->widgets[i].type == W_MENUBAR)
            layout_menubar(w, i);
    }

    /* Phase 4: dropdown positions — only needed when a menu is open.
     * This lets inside() work for hit-testing without duplicating math. */
    layout_menu_dropdown(w);
}

/* ---- rendering one widget ---- */
static void draw_widget(struct vui_window *w, struct vui_widget *wd) {
    if (!wd->visible) return;
    /* Containers are layout-only; they render nothing. */
    if (wd->type == W_VBOX || wd->type == W_HBOX) {
        /* Flat fill, no glass border — keeps table rows clean (no cell outlines). */
        if (wd->color != VUI_TEXT) rect(w, wd->x, wd->y, wd->w, wd->h, wd->color);
        return;
    }
    /* Menu items are drawn as a dropdown overlay in repaint(), not here. */
    if (wd->type == W_MENUITEM) return;
    switch (wd->type) {
    case W_MENUBAR:
        /* Thin glass menu bar with a quiet bottom separator. */
        rect(w, wd->x, wd->y, wd->w, wd->h, glass(g_theme.surface, 220u));
        rect(w, wd->x, wd->y, wd->w, 1, glass(0x00ffffffu, 28u));
        rect(w, wd->x, wd->y + wd->h - 1, wd->w, 1, glass(g_theme.border_hi, 145u));
        break;
    case W_MENU: {
        /* Menu title: highlight on hover or when this menu is open. */
        int is_active = ((int)(wd - w->widgets) == w->active_menu_idx);
        if (is_active || wd->hover) {
            vui_u32 hi = glass(mix(g_theme.accent, g_theme.surface, 1u, 5u), 180u);
            fill_round_rect(w, wd->x + 2, wd->y + 3, wd->w - 4, wd->h - 6, 5, hi, hi);
        }
        vui_u32 fg = is_active ? g_theme.text : (wd->hover ? g_theme.text : g_theme.text_dim);
        text(w, wd->x + 8, wd->y + 3, wd->text, fg);
        break; }
    case W_PANEL:
        glass_box(w, wd->x, wd->y, wd->w, wd->h, g_theme.surface, 0);
        if (wd->text[0]) {
            rect(w, wd->x + 1, wd->y + 1, wd->w - 2, 22, glass(g_theme.surface_hi, 170u));
            rect(w, wd->x + 1, wd->y+23, wd->w - 2, 1, glass(g_theme.border_hi, 120u));
            fill_round_rect(w, wd->x+8, wd->y+6, 3, 12, 2, g_theme.accent, g_theme.accent);
            text(w, wd->x+16, wd->y+4, wd->text, g_theme.text);
        }
        break;
    case W_CARD:
        glass_box(w, wd->x, wd->y, wd->w, wd->h,
                  wd->color != VUI_TEXT ? wd->color : g_theme.surface, 1);
        if (wd->text[0]) text(w, wd->x + 12, wd->y + 9, wd->text, g_theme.text_dim);
        break;
    case W_LABEL:
        /* wd->value carries an optional integer text scale (1 = normal). */
        if (wd->value > 1) text_scaled(w, wd->x, wd->y, wd->text, wd->color, wd->value);
        else text(w, wd->x, wd->y, wd->text, wd->color);
        break;
    case W_CANVAS: {
        /* Raw pixel buffer blit. pixels = (uint32_t*)wd->user, with its OWN row
         * stride in wd->value (the source buffer may be wider than the widget).
         * The destination g_canvas is packed at stride w->width — the same
         * convention canvas_index() uses for every other draw — so we MUST index
         * it with w->width here, not VUI_MAX_W, or the blitted region shears
         * relative to the rest of the window. */
        uint32_t *pixels = (uint32_t *)wd->user;
        int stride = wd->value;
        if (pixels && stride > 0) {
            for (int iy = 0; iy < wd->h; ++iy) {
                int py = wd->y + iy;
                if (py < 0 || py >= w->height) continue;
                for (int ix = 0; ix < wd->w; ++ix) {
                    int px = wd->x + ix;
                    long di = canvas_index(w, px, py);
                    if (di < 0) continue;
                    g_canvas[di] = pixels[iy * stride + ix];
                }
            }
        }
        break; }
    case W_BUTTON: {
        /* Restrained rounded button (themed): subtle surface fill, thin border
         * that turns accent on hover/press; no gradient or accent bar. */
        uint32_t accent = wd->color ? wd->color : g_theme.accent;
        uint32_t fill = wd->pressed ? glass(mix(g_theme.surface, accent, 1u, 3u), 240u)
                       : (wd->hover ? glass(mix(g_theme.surface, accent, 1u, 5u), 230u)
                                    : glass(g_theme.surface_hi, 215u));
        /* Faint accent outline even at rest (subtle blue for normal buttons, a
         * restrained red for danger-coloured buttons like KILL). */
        uint32_t bd = (wd->hover || wd->pressed) ? mix(g_theme.surface, accent, 1u, 2u)
                                                 : mix(g_theme.border, accent, 1u, 3u);
        uint32_t tcol = (wd->hover || wd->pressed) ? g_theme.text
                                                   : mix(g_theme.text_dim, accent, 1u, 2u);
        fill_round_rect(w, wd->x, wd->y, wd->w, wd->h, 4, fill, bd);
        /* Optional leading icon (e.g. a "+" on "New Folder"): icon + label are
         * centred together as one group, with the icon to the left of the text. */
        const char *bisvg = widget_icon(wd);
        int tw = text_px(wd->text, 1);
        int ty = wd->y + (wd->h - 16) / 2;
        if (bisvg) {
            int isz = 16, gap = 5;
            int gx = wd->x + (wd->w - (isz + gap + tw)) / 2;
            draw_svg_icon(w, gx, wd->y + (wd->h - isz) / 2, isz, bisvg, tcol);
            text(w, gx + isz + gap, ty, wd->text, tcol);
        } else {
            text(w, wd->x + (wd->w - tw) / 2, ty, wd->text, tcol);
        }
        break; }
    case W_PILLBTN: {
        /* Fully-rounded pill button (tags: Docs/Releases/Status …). Faint glass
         * fill at rest; accent-tinted on hover. */
        uint32_t accent = wd->color ? wd->color : g_theme.accent;
        uint32_t fill = (wd->hover || wd->pressed) ? glass(mix(g_theme.surface, accent, 1u, 5u), 230u)
                                                    : glass(mix(g_theme.surface, 0x00ffffffu, 1u, 16u), 190u);
        uint32_t bd = (wd->hover || wd->pressed) ? mix(g_theme.surface, accent, 1u, 2u)
                                                  : mix(g_theme.surface, 0x00cfe2f5u, 1u, 5u);
        int tx = wd->x + (wd->w - text_px(wd->text, 1)) / 2;
        int ty = wd->y + (wd->h - 16) / 2;
        fill_round_rect(w, wd->x, wd->y, wd->w, wd->h, 4, fill, bd);
        rect(w, wd->x + 2, wd->y + 1, wd->w - 4, 1, glass(0x00ffffffu, 30u));
        text(w, tx, ty, wd->text, (wd->hover || wd->pressed) ? g_theme.text : g_theme.text_dim);
        break; }
    case W_TILE: {
        /* Dock icon tile with enough raster budget for crisp line-art. The
         * interior stays mostly transparent so the glass bar remains visible. */
        uint32_t accent = wd->color ? wd->color : g_theme.accent;
        int cx = wd->x + wd->w / 2;
        int cy = wd->y + wd->h / 2 - 3;
        int active = (wd->hover || wd->pressed);
        int pad = 6;
        int tx = wd->x + pad, ty = wd->y + pad;
        int tw = wd->w - 2 * pad, th = wd->h - 2 * pad;
        uint32_t bd = active ? mix(g_theme.surface, accent, 1u, 1u)
                             : mix(g_theme.surface, 0x00cfe2f5u, 1u, 3u);
        uint32_t fill = active ? mix(g_theme.surface, accent, 1u, 7u)
                               : argb(mix(g_theme.surface, 0x00ffffffu, 1u, 18u), 150u);
        uint32_t ic = active ? mix(g_theme.text, accent, 1u, 4u) : mix(g_theme.text_dim, 0x00ffffffu, 1u, 6u);
        fill_round_rect(w, tx, ty, tw, th, 6, fill, bd);
        const char *isvg = widget_icon(wd);
        if (isvg) {
            int icon_size = 32;   /* renderer keeps a built-in edge margin */
            draw_svg_icon(w, cx - icon_size / 2, cy - icon_size / 2, icon_size, isvg, accent);
        } else if (wd->value > 0) {
            tile_icon(w, cx, cy, wd->value, ic);
        } else {
            int glyph_w = text_px(wd->text, 1);
            int gx = wd->x + (wd->w - glyph_w) / 2;
            int gy = wd->y + (wd->h - 16) / 2 - 2;
            if (gx < wd->x + 6) gx = wd->x + 6;
            text(w, gx, gy, wd->text, ic);
        }
        rect(w, wd->x + wd->w + 10, wd->y + 10, 1, wd->h - 20,
             argb(mix(g_theme.surface, 0x00cfe2f5u, 1u, 4u), 130u));
        if (active) fill_round_rect(w, cx - 9, ty + th + 4, 18, 3, 2, accent, accent);
        else if (wd->running) fill_round_rect(w, cx - 3, ty + th + 4, 6, 3, 1, accent, accent);
        break; }
    case W_ICON: {
        /* Bare SVG icon/logo: no background, no border — just the artwork,
         * sized to the widget and centered (e.g. the topbar logo). When the
         * icon is interactive (has a click handler), it doubles as a flat icon
         * button: a subtle rounded wash appears on hover/press, but the icon
         * itself stays chrome-free to match toolbars in the mockup. */
        uint32_t accent = wd->color ? wd->color : g_theme.accent;
        int size = wd->w < wd->h ? wd->w : wd->h;
        if (wd->on_click && (wd->hover || wd->pressed)) {
            uint32_t wash = glass(mix(g_theme.surface, accent, 1u, 6u),
                                  wd->pressed ? 220u : 170u);
            fill_round_rect(w, wd->x, wd->y, wd->w, wd->h, 6, wash, wash);
        }
        const char *isvg = widget_icon(wd);
        if (isvg)
            draw_svg_icon(w, wd->x + (wd->w - size) / 2, wd->y + (wd->h - size) / 2,
                          size, isvg, accent);
        break; }
    case W_LISTITEM: {
        /* Flat sidebar row: left-aligned SVG icon + label. Three visual states:
         * selected (wd->running) draws a tinted surface, a thin border and an
         * accent bar on the left edge; hover draws a faint wash; rest is bare. */
        uint32_t accent = wd->color ? wd->color : g_theme.accent;
        int selected = wd->running;
        int active = wd->hover || wd->pressed;
        if (selected) {
            fill_round_rect(w, wd->x, wd->y, wd->w, wd->h, 5,
                            glass(mix(g_theme.surface, accent, 1u, 6u), 235u),
                            mix(g_theme.border, accent, 1u, 2u));
            rect(w, wd->x, wd->y + (wd->h - 18) / 2, 3, 18, accent);   /* selector */
        } else if (active) {
            uint32_t wash = glass(mix(g_theme.surface, accent, 1u, 12u), 190u);
            fill_round_rect(w, wd->x, wd->y, wd->w, wd->h, 5, wash, wash);
        }
        uint32_t fg = (selected || active) ? g_theme.text : g_theme.text_dim;
        const char *isvg = widget_icon(wd);
        int icon_size = 18;
        int iy = wd->y + (wd->h - icon_size) / 2;
        if (isvg) draw_svg_icon(w, wd->x + 12, iy, icon_size, isvg, fg);
        text(w, wd->x + 38, wd->y + (wd->h - 16) / 2, wd->text, fg);
        break; }
    case W_INPUT: {
        /* Rounded dark input field (all text/search fields). Subtle border that
         * turns to an accent focus ring on hover; dimmed placeholder text. */
        int tx = wd->x + 28;
        int cy = wd->y + (wd->h - 16) / 2;       /* vertically-centred content */
        int focused = ((int)(wd - w->widgets) == w->active_input);
        uint32_t ibg = glass(mix(g_theme.bg, 0x00000000u, 1u, 3u), 220u);
        uint32_t bd  = (focused || wd->hover) ? mix(g_theme.surface, g_theme.accent, 1u, 2u)
                                              : glass(g_theme.border, 165u);
        fill_round_rect(w, wd->x, wd->y, wd->w, wd->h, g_theme.radius + 1, ibg, bd);
        // rect(w, wd->x + g_theme.radius, wd->y + 1, wd->w - 2 * g_theme.radius, 1, glass(0x00ffffffu, 18u)); no top white border
        /* Magnifier icon. */
        const char *isvg = widget_icon(wd);
        if (isvg) {
            int icon_size = 16;
            draw_svg_icon(w, wd->x + 10, wd->y + (wd->h - icon_size) / 2, icon_size, isvg, 0x00ffffffu);
        }
        /* Typed value (bright) or placeholder (dim); blinking-less caret. */
        if (wd->text[0]) {
            text(w, tx, cy, wd->text, g_theme.text);
            if (focused) rect(w, tx + text_px(wd->text,1), cy, 1, 16, g_theme.accent);
        } else {
            text(w, tx, cy, wd->mtitle, g_theme.text_dim);
            if (focused) rect(w, tx, cy, 1, 16, g_theme.accent);
        }
        break; }
    case W_BADGE: {
        /* Status pill: faint colour wash + matching border and text. The variant
         * colour comes from wd->color (ok = secure, danger = danger, else neutral). */
        uint32_t c = wd->color ? wd->color : g_theme.text_dim;
        uint32_t fill = glass(mix(g_theme.surface, c, 1u, 10u), 205u);
        uint32_t bd   = mix(g_theme.surface, c, 1u, 4u);
        fill_round_rect(w, wd->x, wd->y, wd->w, wd->h, g_theme.radius, fill, bd);
        text(w, wd->x + 9, wd->y + 2, wd->text, mix(c, g_theme.text, 1u, 2u));
        break; }
    case W_TABS: {
        int seg = 0, seg_start = 0, i = 0;
        int count = 1, per;
        while (wd->text[i]) { if (wd->text[i] == '|') count++; i++; }
        per = count > 0 ? wd->w / count : wd->w;
        /* Flat tab row (themed): a baseline divider, muted labels, and a rounded
         * accent underline beneath the active tab — no filled segment block. */
        fill_round_rect(w, wd->x, wd->y, wd->w, wd->h, g_theme.radius, glass(g_theme.surface, 120u), glass(g_theme.border, 90u));
        for (seg = 0; seg < count; ++seg) {
            char label[24];
            int li = 0;
            int sx = wd->x + seg * per;
            int sw = (seg == count - 1) ? (wd->w - seg * per) : per;
            int active = (seg == wd->value);
            while (wd->text[seg_start] == '|') seg_start++;
            while (wd->text[seg_start] && wd->text[seg_start] != '|' && li + 1 < (int)sizeof(label))
                label[li++] = wd->text[seg_start++];
            label[li] = 0;
            if (active) {
                fill_round_rect(w, sx + 6, wd->y + 4, sw - 12, wd->h - 8, 4,
                                glass(mix(g_theme.surface, g_theme.accent, 1u, 6u), 180u),
                                mix(g_theme.surface, g_theme.accent, 1u, 3u));
                fill_round_rect(w, sx + 14, wd->y + wd->h - 6, sw - 28, 2, 1,
                                g_theme.accent, g_theme.accent);
            }
            text(w, sx + (sw - text_px(label, 1)) / 2, wd->y + (wd->h - 16) / 2, label,
                 active ? g_theme.text : g_theme.text_dim);
        }
        break; }
    case W_PILL: {
        /* Glassmorphism dock: layered semi-transparent bands create a soft
         * vertical gradient (lighter top → darker bottom). Each band is a
         * rectangle drawn inside the rounded area, so corners stay clean. */
        uint32_t fill = wd->color ? wd->color : g_theme.surface;

        /* Mix lighter top / darker bottom tones from the base colour. */
        uint32_t top_c = mix(fill, 0x00ffffffu, 2u, 10u);   /* lighter  */
        uint32_t mid_c = fill;                                /* base     */
        uint32_t bot_c = mix(fill, 0x00000000u, 3u, 10u);   /* darker   */

        int r = 14;
        if (r > wd->h / 2) r = wd->h / 2;
        int hh = wd->h;  /* total pill height */

        /* Base: darkest tone, fills the rounded shape. */
        fill_round_rect(w, wd->x, wd->y, wd->w, hh, r, argb(bot_c, 230u),
                        mix(fill, 0x00aaccffu, 1u, 6u));

        /* Three horizontal bands, each lighter toward the top.  The bands
         * stay strictly inside the non-rounded rectangle so they never
         * bleed into corner pixels. */
        int inner_x = wd->x + r;
        int inner_w = wd->w - 2 * r;

        int split1 = wd->y + r + 1;                       /* top of flat area   */
        int split2 = wd->y + hh * 2 / 5;                  /* upper-middle        */
        int split3 = wd->y + hh * 3 / 5;                  /* lower-middle        */
        int split4 = wd->y + hh - r - 1;                  /* bottom of flat area */

        /* Band 1 (top): lightest, soft alpha so it reads as a highlight. */
        rect(w, inner_x, split1, inner_w, split2 - split1, argb(top_c, 90u));
        /* Band 2 (mid-top): medium-light. */
        rect(w, inner_x, split2, inner_w, split3 - split2, argb(mid_c, 140u));
        /* Band 3 (mid-bot): medium-dark. */
        rect(w, inner_x, split3, inner_w, split4 - split3, argb(bot_c, 75u));

        /* Glass edge: a brighter line at the very top of the rounded shape. */
        rect(w, wd->x + r - 2, wd->y + 2, wd->w - 2 * (r - 2), 1,
             argb(0x00ffffffu, 80u));
        rect(w, wd->x + r + 2, wd->y + 3, wd->w - 2 * (r + 2), 1,
             argb(0x00ffffffu, 40u));
        break; }
    case W_METRIC: {
        /* Self-contained metric card: title + big value + sub-label + a chart
         * (max==0) or progress bar (max==1, value 0..100). One widget, one draw
         * pass — robust against the layout quirks of stacked sub-widgets. */
        uint32_t accent = wd->color ? wd->color : g_theme.accent;
        uint32_t bd   = glass(mix(g_theme.surface, 0x00cfe2f5u, 1u, 8u), 165u);
        int ix = wd->x + wd->w / 2;             /* indicator left edge */
        int iw = wd->w - (ix - wd->x) - 14;
        fill_round_rect(w, wd->x, wd->y, wd->w, wd->h, g_theme.radius + 2, glass(g_theme.surface, 224u), bd);
        // rect(w, wd->x + g_theme.radius, wd->y + 1, wd->w - 2 * g_theme.radius, 1, glass(0x00ffffffu, 24u));
        text(w, wd->x + 14, wd->y + 10, wd->mtitle, g_theme.text_dim);
        text_scaled(w, wd->x + 14, wd->y + 28, wd->text, accent, 2);
        text(w, wd->x + 14, wd->y + wd->h - 18, wd->msub, g_theme.text_dim);
        if (wd->max == 1) {                     /* progress bar */
            int pct = wd->value; if (pct < 0) pct = 0; if (pct > 100) pct = 100;
            int by = wd->y + 34, bh = 10, fillw = iw * pct / 100;
            fill_round_rect(w, ix, by, iw, bh, bh/2, glass(mix(g_theme.surface, 0x00ffffffu, 1u, 10u), 170u), bd);
            if (fillw >= bh) {
                fill_round_rect(w, ix, by, fillw, bh, bh/2, accent, accent);
                fill_round_rect(w, ix + fillw - bh, by, bh, bh, bh/2,
                                mix(accent, 0x00ffffffu, 1u, 3u), mix(accent, 0x00ffffffu, 1u, 3u));
            }
        } else if (wd->max == 0 && wd->hist_n >= 2) {   /* live area chart */
            unsigned char ord[32]; int n = wd->hist_n, k;
            int start = (wd->hist_head + (int)sizeof(wd->hist) - n) % (int)sizeof(wd->hist);
            for (k = 0; k < n; ++k) ord[k] = wd->hist[(start + k) % (int)sizeof(wd->hist)];
            area_chart(w, ix, wd->y + 28, iw, 28, ord, n, accent);
        }                                        /* max==2: value only (no chart) */
        break; }
    case W_SPARK: {
        /* Smooth line graph (per the reference): a polyline across the widget. */
        static const unsigned char pat[20] = {
            3,5,4,6,5,7,6,8,7,9,7,6,5,7,8,6,5,7,6,8
        };
        uint32_t accent = wd->color ? wd->color : g_theme.accent;
        int n = 16, i, px = -1, py = 0;
        for (i = 0; i < n; ++i) {
            int v = pat[i];                                 /* 0..10 */
            int x = wd->x + i * (wd->w - 1) / (n - 1);
            int y = wd->y + (wd->h - 2) - v * (wd->h - 3) / 10;
            if (px >= 0) draw_line(w, px, py, x, y, accent);
            px = x; py = y;
        }
        break; }
    case W_BAR: {
        /* Rounded progress bar (themed): faint track + accent fill (lightened
         * toward the right for a subtle gradient). */
        int v=wd->value; if(v<0)v=0; if(v>wd->max)v=wd->max;
        int r = wd->h/2;
        uint32_t accent = wd->color ? wd->color : g_theme.accent;
        uint32_t track = glass(mix(g_theme.surface, 0x00ffffffu, 1u, 8u), 170u);
        int fillw = wd->w * v / wd->max;
        fill_round_rect(w, wd->x, wd->y, wd->w, wd->h, r, track, track);
        if (fillw >= 2*r)
            fill_round_rect(w, wd->x, wd->y, fillw, wd->h, r,
                            mix(accent, g_theme.surface, 1u, 4u), accent);
        break; }
    case W_SLIDER: {
        /* Thin 4px track centred in the widget box, accent-filled up to the
         * value, with a round handle. The tall box is just the grab area. */
        int v = wd->value; if (v < 0) v = 0; if (v > wd->max) v = wd->max;
        uint32_t accent = wd->color ? wd->color : g_theme.accent;
        uint32_t track  = glass(mix(g_theme.surface, 0x00ffffffu, 1u, 6u), 180u);
        int th = 4, ty = wd->y + (wd->h - th) / 2;
        int fillw = wd->w * v / wd->max;
        int hx = wd->x + fillw, hy = wd->y + wd->h / 2;
        int hr = (wd->h / 2) - 1; if (hr < 5) hr = 5;
        fill_round_rect(w, wd->x, ty, wd->w, th, th / 2, track, track);
        if (fillw >= th)
            fill_round_rect(w, wd->x, ty, fillw, th, th / 2, accent, accent);
        /* handle: filled rounded square (= disc) + faint glow when hovered */
        if (wd->hover || wd->pressed)
            fill_round_rect(w, hx - hr - 2, hy - hr - 2, (hr + 2) * 2, (hr + 2) * 2,
                            hr + 2, glass(accent, 60u), glass(accent, 60u));
        fill_round_rect(w, hx - hr, hy - hr, hr * 2, hr * 2, hr,
                        mix(accent, 0x00ffffffu, 1u, 3u), accent);
        break; }
    }
}

#define TOOLTIP_DELAY   6   /* loop iterations of hover before bubble appears (~0.4 s) */
#define TOOLTIP_PAD     6   /* horizontal padding inside the bubble */
#define TOOLTIP_VPAD    4   /* vertical padding inside the bubble */

/* Draw a tooltip bubble above the hovered widget when enough hover time has
 * accumulated.  Rendered last so it always appears on top of everything. */
static void draw_tooltip(struct vui_window *w) {
    struct vui_widget *wd;
    int tx, ty, tw, th, bx, by;

    if (w->tooltip_widget < 0 || w->tooltip_widget >= w->widget_count) return;
    if (w->tooltip_ticks < TOOLTIP_DELAY) return;

    wd = &w->widgets[w->tooltip_widget];
    if (!wd->tooltip[0] || !wd->visible || !wd->hover) return;

    tw = text_px(wd->tooltip, 1) + TOOLTIP_PAD * 2;
    th = 16 + TOOLTIP_VPAD * 2;

    /* Centre above the widget; clamp to window edges. */
    bx = wd->x + (wd->w - tw) / 2;
    if (bx < 2) bx = 2;
    if (bx + tw > w->width - 2) bx = w->width - 2 - tw;
    by = wd->y - th - 6;
    if (by < 2) by = wd->y + wd->h + 6;   /* flip below if no room above */

    /* Bubble background: themed surface with border. */
    fill_round_rect(w, bx, by, tw, th, 4, g_theme.surface, g_theme.border);

    tx = bx + TOOLTIP_PAD;
    ty = by + TOOLTIP_VPAD;
    text(w, tx, ty, wd->tooltip, g_theme.text);
}

/* Draw the open dropdown panel + all its items on top of everything else. */
static void draw_dropdown(struct vui_window *w) {
    int menu_idx = w->active_menu_idx;
    if (menu_idx < 0) return;

    struct vui_widget *menu = &w->widgets[menu_idx];
    int dw = dropdown_width(w,  menu_idx);
    int dh = dropdown_height(w, menu_idx);
    int dx = menu->x;
    int dy = menu->y + menu->h;

    /* Clamp to window right edge. */
    if (dx + dw > w->width) dx = w->width - dw;

    /* Dark glass panel matching the top-bar dropdowns (themed via menu_*). */
    int i;
    rect(w, dx + 3, dy + 4, dw, dh, mix(0x00000000u, g_theme.menu_bg, 1u, 2u)); /* soft shadow */
    rect(w, dx, dy, dw, dh, g_theme.menu_bg);
    rect(w, dx,        dy,        dw, 1,  g_theme.border);
    rect(w, dx,        dy + dh-1, dw, 1,  g_theme.border);
    rect(w, dx,        dy,        1,  dh, g_theme.border);
    rect(w, dx + dw-1, dy,        1,  dh, g_theme.border);

    /* Items — positions already set by layout_menu_dropdown(). */
    for (i = 0; i < w->widget_count; ++i) {
        struct vui_widget *it = &w->widgets[i];
        if (it->type != W_MENUITEM || it->parent_idx != menu_idx) continue;
        if (it->separator) {
            rect(w, it->x + 8, it->y, it->w - 16, 1, mix(g_theme.border, g_theme.menu_bg, 1u, 2u));
        } else {
            if (it->hover)
                rect(w, it->x + 4, it->y + 1, it->w - 8, it->h - 2,
                     mix(VUI_ACCENT, g_theme.menu_bg, 1u, 6u));
            text(w, it->x + 12, it->y + 3, it->text,
                 it->hover ? g_theme.text : g_theme.menu_item);
        }
    }
}

/* Map a mouse x to a slider value and fire on_click if it changed. */
static void slider_set_from_x(struct vui_window *w, struct vui_widget *wd, int mx) {
    int rel = mx - wd->x;
    if (rel < 0) rel = 0;
    if (rel > wd->w) rel = wd->w;
    int v = (wd->w > 0) ? (wd->max * rel + wd->w / 2) / wd->w : 0;
    if (v != wd->value) {
        wd->value = v;
        w->dirty = 1;
        dmg_add(wd->x - 8, wd->y - 8, wd->w + 16, wd->h + 16);
        if (wd->on_click) wd->on_click(wd);
    }
}

static void repaint(struct vui_window *w) {
    int i;
    layout_widgets(w);
    rect(w, 0, 0, w->width, w->height, w->clear_color);
    /* Pass 1: all regular widgets (menu items excluded — drawn in pass 2). */
    for (i = 0; i < w->widget_count; ++i) draw_widget(w, &w->widgets[i]);
    /* Pass 2: dropdown overlay so it always appears on top. */
    draw_dropdown(w);
    /* Pass 3: tooltip bubble — rendered last, above everything. */
    draw_tooltip(w);
    /* When the framebuffer is kernel-bound, flushing is a zero-copy dirty-rect
     * mark; otherwise fall back to the traditional whole-window present. */
    if (w->fb_bound) {
        sc3(66, (uint64_t)w->id, 0, (uint64_t)(((w->width & 0xffff) << 16) | (w->height & 0xffff)));
    } else {
        sc6(SYS_WINDOW_PRESENT, (uint64_t)w->id, (uint64_t)(size_t)g_canvas,
            (uint64_t)w->width, (uint64_t)w->height, 0, 0);
    }
    dmg_reset();
}

static int inside(struct vui_widget *wd, int x, int y) {
    if (!wd->visible) return 0;
    return x>=wd->x && x<wd->x+wd->w && y>=wd->y && y<wd->y+wd->h;
}

static int tab_count(const char *labels) {
    int n = 1, i = 0;
    if (!labels || !labels[0]) return 0;
    while (labels[i]) { if (labels[i] == '|') ++n; ++i; }
    return n;
}

/* ---- public lifecycle ---- */
vui_window *vui_window_open_inset(const char *title, int width, int height,
                                  vui_u32 flags, int x, int y,
                                  int shadow_inset_top) {
    static int theme_loaded = 0;
    int id;
    struct winsys_window_options options;
    if (!theme_loaded) {
        /* Try to load a per-user theme.  Start with root's home (the common
         * case on VibeOS today), then fall back to /home/user.  The theme
         * file is optional — desktop_theme_load handles missing files. */
        vui_load_theme("/root/.config/vibeos.theme");
        vui_load_theme("/home/user/.config/vibeos.theme");
        theme_loaded = 1;
    }
    if (width > VUI_MAX_W) width = VUI_MAX_W;
    if (height > VUI_MAX_H) height = VUI_MAX_H;
    options.title = title;
    options.width = width;
    options.height = height;
    options.flags = flags;
    options.x = x;
    options.y = y;
    options.shadow_inset_top = shadow_inset_top;
    id = (int)sc1(SYS_WINDOW_CREATE_EX, (uint64_t)(size_t)&options);
    if (id < 0) {
        /* No window server: it is the toolkit's job (not the app's) to report
         * this and terminate cleanly. We never return here, so app code does
         * not need to null-check the result. */
        emit("vexui: no window server running.\n");
        emit("       start the desktop with 'gui', then launch this app from its terminal.\n");
        sc1(SYS_EXIT, 1);
        for (;;) { do_yield(); }   /* unreachable safety net */
    }
    g_win.id=id; g_win.width=width; g_win.height=height; g_win.open=1;
    /* Attempt to bind the window content storage directly into our address
     * space.  On success g_canvas points straight at the compositor
     * back-buffer — every pixel we write lands there with zero intermediate
     * copies.  Falls back to the static g_canvas_fb when the kernel does
     * not support BIND_FB (older VibeOS or non-user-app windows). */
        if (0) {
        int fb_stride = 0, fb_w = 0, fb_h = 0;
        long fb_addr, fb_dx, fb_r10, fb_r8;
        __asm__ volatile(
            "int \$0x80"
            : "=a"(fb_addr), "=d"(fb_dx), "=r"(fb_r10), "=r"(fb_r8)
            : "a"(65), "D"((long)id)
            : "rcx", "r11", "memory"
        );
        void *fb = (void *)(unsigned long)fb_addr;
        if (fb && fb_dx > 0) { fb_stride = (int)fb_dx; fb_w = (int)fb_r10; fb_h = (int)fb_r8; }
        else { fb_stride = 0; fb_w = 0; fb_h = 0; }
        if (fb && fb_stride > 0 && fb_w > 0 && fb_h > 0) {
            g_canvas = (uint32_t *)fb;
            g_win.fb_stride = fb_stride;
            g_win.fb_bound  = 1;
        }
    }

    g_win.mouse_x=-1; g_win.mouse_y=-1; g_win.mouse_down=0;
    g_win.clear_color=g_theme.bg;
    g_win.widget_count=0; g_win.dirty=1; g_win.on_tick=0; g_win.on_resize=0; g_win.on_context_menu=0; g_win.on_key=0; g_win.on_scroll=0; g_win.on_mouse_move=0; g_win.on_mouse_click=0; g_win.on_mouse_release=0;
    g_win.menu_count=0; g_win.active_menu_idx=-1; g_win.active_input=-1; g_win.active_slider=-1;
    g_win.tooltip_widget=-1; g_win.tooltip_ticks=0;
    return &g_win;
}

vui_window *vui_window_open_ex(const char *title, int width, int height,
                               vui_u32 flags, int x, int y) {
    return vui_window_open_inset(title, width, height, flags, x, y, 0);
}

vui_window *vui_window_open(const char *title, int width, int height) {
    return vui_window_open_ex(title, width, height, 0, 0, 0);
}

void vui_quit(vui_window *w){ if(w) w->open=0; }
void vui_clear_focus(vui_window *w) { if (w) { w->active_input = -1; w->dirty = 1; } }
int vui_window_id(vui_window *w){ return w ? w->id : -1; }

int vui_file_dialog(const char *title, const char *initial_path, char *out_path, int out_cap, int save_mode) {
    emit("vui_file_dialog: entered\n");
    (void)title;
    char arg[256];
    char res_file[64];
    int pid = (int)sc1(SYS_GETPID, 0);
    
    /* Create a unique result file name: /tmp/fdr.PID */
    scopy(res_file, "/tmp/fdr.", 64);
    int p = pid, k = 9;
    if (p == 0) res_file[k++] = '0';
    else {
        char t[16]; int ti = 0;
        while (p > 0) { t[ti++] = (char)('0' + (p % 10)); p /= 10; }
        while (ti > 0) res_file[k++] = t[--ti];
    }
    res_file[k] = '\0';

    /* Build argument: "MODE;PATH;RESULT_FILE" */
    int ai = 0;
    if (save_mode) { scopy(arg, "SAVE;", 256); ai = 5; }
    else { scopy(arg, "OPEN;", 256); ai = 5; }
    
    scopy(arg + ai, initial_path ? initial_path : "/", 256 - ai);
    ai = slen(arg);
    arg[ai++] = ';';
    scopy(arg + ai, res_file, 256 - ai);

    emit("vui_file_dialog: spawning /bin/filebrowser with arg: ");
    emit(arg);
    emit("\n");

    /* Remove old result if it exists */
    sc1(SYS_UNLINK, (uint64_t)(size_t)res_file);

    /* Spawn the dialog app */
    int child = (int)sc2(SYS_PROCESS_SPAWN, (uint64_t)(size_t)"/bin/filebrowser", (uint64_t)(size_t)arg);
    if (child <= 0) {
        emit("vui_file_dialog: spawn failed\n");
        return 0;
    }

    emit("vui_file_dialog: spawned child pid=");
    /* Simple integer print */
    { char b[16]; int i=0, v=child; if(v==0) b[i++]='0'; else { while(v>0){ b[i++]='0'+(v%10); v/=10; } }
      while(i>0){ char c=b[--i]; sc3(SYS_WRITE, 1, (uint64_t)(size_t)&c, 1); } }
    emit("\n");

    /* Wait for it to finish */
    int status = 0;
    while (sc2(SYS_WAITPID, (uint64_t)child, (uint64_t)(size_t)&status) == 0) {
        do_yield();
    }

    emit("vui_file_dialog: child finished, status=");
    { char b[16]; int i=0, v=status; if(v==0) b[i++]='0'; else { if(v<0){ emit("-"); v=-v; } while(v>0){ b[i++]='0'+(v%10); v/=10; } }
      while(i>0){ char c=b[--i]; sc3(SYS_WRITE, 1, (uint64_t)(size_t)&c, 1); } }
    emit("\n");

    /* Read the result back */
    int fd = (int)sc1(SYS_OPEN, (uint64_t)(size_t)res_file);
    if (fd < 0) {
        emit("vui_file_dialog: could not open result file\n");
        return 0;
    }
    
    int n = (int)sc3(SYS_READ, (uint64_t)fd, (uint64_t)(size_t)out_path, (uint64_t)(out_cap - 1));
    sc1(SYS_CLOSE, (uint64_t)fd);
    
    if (n > 0) {
        out_path[n] = '\0';
        emit("vui_file_dialog: success, path=");
        emit(out_path);
        emit("\n");
        /* Cleanup */
        sc1(SYS_UNLINK, (uint64_t)(size_t)res_file);
        return 1;
    }

    emit("vui_file_dialog: no data read from result file\n");
    return 0;
}

void __attribute__((noreturn)) vui_run(vui_window *w) {
    /* initial paint */
    repaint(w);
    w->dirty = 0;

    while (w->open) {
        struct winsys_event ev;
        int i;
        int click_x = -1, click_y = -1;

        if (w->on_tick) {
            w->on_tick(w);
        }

        while ((int)sc2(SYS_EVENT_POLL, (uint64_t)w->id, (uint64_t)(size_t)&ev) == 1) {
            if (ev.type == EV_MOUSE_MOVE) {
                w->mouse_x=ev.x; w->mouse_y=ev.y;
                if (w->on_mouse_move) w->on_mouse_move(w, ev.x, ev.y, 0);
            }
            else if (ev.type == EV_MOUSE_DOWN) {
                w->mouse_x=ev.x; w->mouse_y=ev.y; w->mouse_down=1; click_x=ev.x; click_y=ev.y;
                if (w->on_mouse_click) w->on_mouse_click(w, ev.x, ev.y, ev.buttons);
            }
            else if (ev.type == EV_MOUSE_UP) {
                w->mouse_down=0;
                w->active_slider = -1;          /* release any slider drag */
                if (w->on_mouse_release) w->on_mouse_release(w, w->mouse_x, w->mouse_y, 0);
            }
            else if (ev.type == EV_CLOSE) { w->open=0; }
            else if (ev.type == EV_CONTEXT_MENU) { w->mouse_x=ev.x; w->mouse_y=ev.y; if(w->on_context_menu) w->on_context_menu(w, ev.x, ev.y); }
            else if (ev.type == EV_MENU_ACTION) {
                if (ev.key >= 1000) {
                    int w_idx = (int)(ev.key - 1000);
                    if (w_idx < w->widget_count) {
                        struct vui_widget *wd = &w->widgets[w_idx];
                        if (wd->type == W_MENUITEM && wd->on_click) {
                            wd->on_click(wd);
                        }
                    }
                } else if (ev.key < (uint32_t)w->menu_count && w->menu_cbs[ev.key]) {
                    w->menu_cbs[ev.key](w);
                }
            }
            else if (ev.type == EV_SCROLL) {
                /* Wheel notch delta arrives in ev.y (positive = away/up). */
                if (w->on_scroll) w->on_scroll(w, ev.y);
            }
            else if (ev.type == EV_KEY && w->active_input < 0) {
                /* No focused input: hand the raw keystroke to the app, if it
                 * registered a key callback (e.g. a terminal or game). */
                if (w->on_key) w->on_key(w, ev.key);
            }
            else if (ev.type == EV_KEY && w->active_input >= 0) {
                /* Edit the focused input. */
                struct vui_widget *in = &w->widgets[w->active_input];
                unsigned k = ev.key;
                int n = slen(in->text);
                int submit = 0;
                if (k == 0x08) { if (n > 0) in->text[n-1] = 0; }          /* backspace */
                else if (k == 0x1b) { w->active_input = -1; }             /* escape: cancel */
                else if (k == '\n' || k == '\r') { w->active_input = -1; submit = 1; }
                else if (k >= 0x20 && k < 0x7f && n + 1 < (int)sizeof(in->text)) {
                    in->text[n] = (char)k; in->text[n+1] = 0;
                }
                if (in->on_click) in->on_click(in);     /* per-keystroke notify (e.g. re-filter) */
                if (submit && in->on_submit) in->on_submit(in);  /* Enter: form submit */
                w->dirty = 1; dmg_add(in->x, in->y, in->w, in->h);
            }
            else if (ev.type == EV_RESIZE) {
                int nw=ev.x, nh=ev.y;
                if(nw<1)nw=1; if(nh<1)nh=1;
                if(nw>VUI_MAX_W)nw=VUI_MAX_W; if(nh>VUI_MAX_H)nh=VUI_MAX_H;
                w->width=nw; w->height=nh; w->dirty=1; dmg_full();
                if(w->on_resize) w->on_resize(w,nw,nh);
            }
        }

        /* ---- Menu hover ------------------------------------------------- */
        /* Update hover for W_MENU titles (always, so they glow on mouse-over). */
        for (i = 0; i < w->widget_count; ++i) {
            struct vui_widget *m = &w->widgets[i];
            if (m->type != W_MENU) continue;
            uint8_t hov = (uint8_t)inside(m, w->mouse_x, w->mouse_y);
            if (hov != m->hover) { m->hover = hov; w->dirty = 1; dmg_full(); }
        }
        /* Update hover for W_MENUITEM entries when their dropdown is open. */
        if (w->active_menu_idx >= 0) {
            for (i = 0; i < w->widget_count; ++i) {
                struct vui_widget *it = &w->widgets[i];
                if (it->type != W_MENUITEM || it->parent_idx != w->active_menu_idx
                    || it->separator) continue;
                uint8_t hov = (uint8_t)inside(it, w->mouse_x, w->mouse_y);
                if (hov != it->hover) { it->hover = hov; w->dirty = 1; dmg_full(); }
            }
        }

        /* ---- Menu click dispatch ---------------------------------------- */
        if (click_x >= 0) {
            if (w->active_menu_idx >= 0) {
                /* A dropdown is open: check items first, then dismiss. */
                int hit = 0;
                for (i = 0; i < w->widget_count; ++i) {
                    struct vui_widget *it = &w->widgets[i];
                    if (it->type != W_MENUITEM) continue;
                    if (it->parent_idx != w->active_menu_idx) continue;
                    if (it->separator || !inside(it, click_x, click_y)) continue;
                    if (it->on_click) it->on_click(it);
                    w->active_menu_idx = -1;
                    w->dirty = 1;
                    hit = 1;
                    break;
                }
                if (!hit) {
                    /* Click outside items: switch to another menu title or close. */
                    int on_title = 0;
                    for (i = 0; i < w->widget_count; ++i) {
                        struct vui_widget *m = &w->widgets[i];
                        if (m->type != W_MENU || !inside(m, click_x, click_y)) continue;
                        w->active_menu_idx = (int)(m - w->widgets);
                        w->dirty = 1;
                        on_title = 1;
                        break;
                    }
                    if (!on_title) { w->active_menu_idx = -1; w->dirty = 1; }
                }
                click_x = -1; /* consume — do not fall through to button dispatch */
            } else {
                /* No dropdown: check if a menu title was clicked. */
                for (i = 0; i < w->widget_count; ++i) {
                    struct vui_widget *m = &w->widgets[i];
                    if (m->type != W_MENU || !inside(m, click_x, click_y)) continue;
                    w->active_menu_idx = (int)(m - w->widgets);
                    w->dirty = 1;
                    click_x = -1; /* consume */
                    break;
                }
            }
        }

        /* ---- Widget hover / click (only when no menu is consuming input) -- */
        int input_clicked = 0;
        for (i = 0; i < w->widget_count; ++i) {
            struct vui_widget *wd = &w->widgets[i];
            if (wd->type != W_BUTTON && wd->type != W_PILLBTN && wd->type != W_TILE && wd->type != W_TABS && wd->type != W_INPUT && wd->type != W_LISTITEM && wd->type != W_ICON && wd->type != W_SLIDER) continue;
            uint8_t hov = (uint8_t)(w->active_menu_idx < 0 &&
                                    inside(wd, w->mouse_x, w->mouse_y));
            uint8_t prs = (uint8_t)(hov && w->mouse_down);
            if (hov != wd->hover || prs != wd->pressed) { wd->hover=hov; wd->pressed=prs; w->dirty=1; dmg_add(wd->x, wd->y, wd->w + 24, wd->h + 8); }
            /* Slider drag: capture on press, follow mouse_x until release even
             * if it leaves the track vertically. */
            if (wd->type == W_SLIDER) {
                if (click_x >= 0 && inside(wd, click_x, click_y))
                    w->active_slider = i;
                if (w->active_slider == i && w->mouse_down)
                    slider_set_from_x(w, wd, w->mouse_x);
            }
            /* Tooltip: accumulate hover ticks; reset when leaving the widget. */
            if (wd->tooltip[0]) {
                int idx = (int)(wd - w->widgets);
                if (hov) {
                    if (w->tooltip_widget == idx) {
                        if (w->tooltip_ticks < TOOLTIP_DELAY) {
                            ++w->tooltip_ticks;
                            if (w->tooltip_ticks == TOOLTIP_DELAY) { w->dirty = 1; dmg_full(); }
                        }
                    } else {
                        w->tooltip_widget = idx;
                        w->tooltip_ticks  = 1;
                    }
                } else if (w->tooltip_widget == idx) {
                    if (w->tooltip_ticks >= TOOLTIP_DELAY) { w->dirty = 1; dmg_full(); }
                    w->tooltip_widget = -1;
                    w->tooltip_ticks  = 0;
                }
            }
            if (click_x >= 0 && inside(wd, click_x, click_y)) {
                if (wd->type == W_INPUT) {        /* focus this input for typing */
                    w->active_input = (int)(wd - w->widgets); input_clicked = 1; w->dirty = 1;
                }
                if (wd->type == W_TABS) {
                    int n = tab_count(wd->text);
                    if (n > 0) {
                        int rel = click_x - wd->x;
                        int tab = rel / (wd->w / n ? wd->w / n : 1);
                        if (tab < 0) tab = 0;
                        if (tab >= n) tab = n - 1;
                        if (tab != wd->value) { wd->value = tab; w->dirty = 1; dmg_add(wd->x, wd->y, wd->w, wd->h); }
                    }
                }
                if (wd->on_click) wd->on_click(wd);
            }
        }
        /* If the tooltip widget is no longer hovered (mouse moved to a widget
         * without a tooltip, or off all widgets), dismiss the bubble. */
        if (w->tooltip_widget >= 0) {
            struct vui_widget *tw = &w->widgets[w->tooltip_widget];
            if (!tw->hover) {
                if (w->tooltip_ticks >= TOOLTIP_DELAY) { w->dirty = 1; dmg_full(); }
                w->tooltip_widget = -1;
                w->tooltip_ticks  = 0;
            }
        }

        /* Click outside any input drops keyboard focus. */
        if (click_x >= 0 && !input_clicked && w->active_input >= 0) {
            w->active_input = -1; w->dirty = 1;
        }

        /* While a dropdown overlay is open (or was, the frame it closes), force
         * a full present so the overlay is drawn/erased cleanly. */
        {
            static int prev_menu = -1;
            if (w->active_menu_idx >= 0 || prev_menu >= 0) dmg_full();
            prev_menu = w->active_menu_idx;
        }
        if (w->dirty) { repaint(w); w->dirty = 0; }
        /* Pace idle UI apps near 16 Hz. Input is still sampled by the kernel;
         * this loop only needs to refresh app state and repaint dirty widgets. */
        nap(6);
    }
    sc1(SYS_EXIT, 0);
    for(;;){}
}
