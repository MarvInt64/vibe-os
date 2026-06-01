/* libvexui — retained-mode implementation. Syscalls + rendering hidden here. */
#include "vexui.h"
/* Keep in sync with MENUBAR_H in vexui.h */
#define MENUBAR_H 22

typedef unsigned long size_t;
typedef long ssize_t;
typedef unsigned long uint64_t;
typedef unsigned int uint32_t;
typedef int int32_t;
typedef unsigned short uint16_t;
typedef unsigned char uint8_t;

#include "vexui_font.h"   /* glyph_for_char() -> const uint8_t[16] */

#define SYS_READ 0
#define SYS_WRITE 1
#define SYS_YIELD 3
#define SYS_EXIT 4
#define SYS_OPEN 7
#define SYS_CLOSE 8
#define SYS_WINDOW_CREATE 17
#define SYS_WINDOW_PRESENT 18
#define SYS_EVENT_POLL 19
#define SYS_PROCESS_SNAPSHOT 28
#define SYS_PROCESS_KILL 29
#define SYS_WINDOW_SET_MENU 30
#define SYS_WINDOW_CREATE_EX 35

struct winsys_event { uint32_t type; int32_t x; int32_t y; uint32_t buttons; uint32_t key; };
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
static inline ssize_t sc4(uint64_t n, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    ssize_t r; __asm__ volatile("mov %5,%%r10; int $0x80" : "=a"(r) : "a"(n),"D"(a0),"S"(a1),"d"(a2),"r"(a3) : "rcx","r8","r9","r10","r11","memory"); return r;
}
static void do_yield(void){ __asm__ volatile("int $0x80"::"a"((uint64_t)SYS_YIELD):"rcx","r11","memory"); }

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
       W_TABS
};

static const vui_theme g_default_theme = {
    0x00101a29u, 0x00182536u, 0x00233146u, 0x0032455du,
    0x006f86a6u, 0x00e8f0f8u, 0x00aebbd0u, 0x006eb6ffu,
    0x0076e0b5u, 0x00f4c36bu, 0x00ff7f8fu, 6u, 10u
};
static vui_theme g_theme = {
    0x00101a29u, 0x00182536u, 0x00233146u, 0x0032455du,
    0x006f86a6u, 0x00e8f0f8u, 0x00aebbd0u, 0x006eb6ffu,
    0x0076e0b5u, 0x00f4c36bu, 0x00ff7f8fu, 6u, 10u
};

struct vui_widget {
    int type;
    int x, y, w, h;
    char text[80];
    vui_u32 color;
    int value, max;
    vui_callback on_click;
    void *user;

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

    /* runtime / rendering state */
    uint8_t hover;
    uint8_t pressed;
    uint8_t visible;
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
    struct vui_widget widgets[VUI_MAX_WIDGETS];
    int widget_count;
    uint8_t dirty;
    vui_tick_callback on_tick;
    vui_resize_callback on_resize;
    vui_context_callback on_context_menu;
    vui_menu_callback menu_cbs[WINSYS_MAX_MENU_ITEMS];
    char menu_labels[WINSYS_MAX_MENU_ITEMS][WINSYS_MENU_LABEL_MAX];
    int menu_count;
};

static struct vui_window g_win;
static uint32_t g_canvas[VUI_MAX_W * VUI_MAX_H];

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
}

int vui_load_theme(const char *path) {
    char buf[1024];
    int fd, n, i = 0;
    vui_theme next = g_theme;
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

static uint32_t mix(uint32_t a, uint32_t b, unsigned step, unsigned total) {
    uint32_t ar = (a >> 16) & 0xffu, ag = (a >> 8) & 0xffu, ab = a & 0xffu;
    uint32_t br = (b >> 16) & 0xffu, bg = (b >> 8) & 0xffu, bb = b & 0xffu;
    return ((((ar * (total - step)) + (br * step)) / total) << 16) |
           ((((ag * (total - step)) + (bg * step)) / total) << 8) |
           (((ab * (total - step)) + (bb * step)) / total);
}

/* ---- canvas drawing ---- */
static void rect(struct vui_window *w, int x, int y, int wid, int hgt, vui_u32 c) {
    int iy, ix;
    for (iy=y; iy<y+hgt; ++iy) { if(iy<0||iy>=w->height) continue;
        for (ix=x; ix<x+wid; ++ix) { if(ix<0||ix>=w->width) continue; g_canvas[iy*w->width+ix]=c; } }
}
static void put(struct vui_window *w,int x,int y,vui_u32 c){ if(x<0||y<0||x>=w->width||y>=w->height)return; g_canvas[y*w->width+x]=c; }
static void line_h(struct vui_window *w, int x, int y, int wid, vui_u32 c) { rect(w, x, y, wid, 1, c); }
static void line_v(struct vui_window *w, int x, int y, int hgt, vui_u32 c) { rect(w, x, y, 1, hgt, c); }
static void glass_box(struct vui_window *w, int x, int y, int wid, int hgt, vui_u32 fill, int strong) {
    vui_u32 top = strong ? g_theme.border_hi : mix(g_theme.border_hi, fill, 1u, 2u);
    vui_u32 edge = strong ? g_theme.border_hi : g_theme.border;
    vui_u32 bot = mix(0x00000000u, fill, 1u, 3u);
    if (wid <= 0 || hgt <= 0) return;
    rect(w, x, y, wid, hgt, fill);
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
static void glyph(struct vui_window *w,int x,int y,char ch,vui_u32 c){
    const uint8_t *g=glyph_for_char(ch); int r,col;
    for(r=0;r<16;++r){ uint8_t b=g[r]; for(col=0;col<8;++col) if((b>>(7-col))&1u) put(w,x+col,y+r,c); }
}
static void text(struct vui_window *w,int x,int y,const char *s,vui_u32 c){
    int cx=x; while(s&&*s){ if(*s=='\n'){cx=x;y+=18;} else {glyph(w,cx,y,*s,c);cx+=8;} ++s; }
}
static void line_diag(struct vui_window *w,int x,int y,int len,int dx,int dy,vui_u32 c){
    int i; for(i=0;i<len;++i){ put(w,x+i*dx,y+i*dy,c); put(w,x+i*dx+1,y+i*dy,c); }
}
/* Small monochrome line-art icons for dock tiles (id from vui_set_value). */
static void tile_icon(struct vui_window *w,int cx,int cy,int id,vui_u32 c){
    switch(id){
    case 1: { /* globe / web */
        int s=9;
        line_h(w, cx-s+2, cy-s,   2*s-3, c);
        line_h(w, cx-s+2, cy+s,   2*s-3, c);
        line_v(w, cx-s,   cy-s+2, 2*s-3, c);
        line_v(w, cx+s,   cy-s+2, 2*s-3, c);
        line_h(w, cx-s, cy, 2*s+1, c);   /* equator */
        line_v(w, cx, cy-s, 2*s+1, c);   /* meridian */
        break; }
    case 2: { /* monitor with an activity line (task manager) */
        int s = 9;
        line_h(w, cx-s,   cy-s+1, 2*s,     c);   /* screen top    */
        line_h(w, cx-s,   cy+s-3, 2*s,     c);   /* screen bottom */
        line_v(w, cx-s,   cy-s+1, 2*s-4,   c);   /* screen left   */
        line_v(w, cx+s-1, cy-s+1, 2*s-4,   c);   /* screen right  */
        line_v(w, cx,     cy+s-3, 3,       c);   /* stand neck    */
        line_h(w, cx-4,   cy+s,   9,       c);   /* stand base    */
        line_diag(w, cx-6, cy+2, 4, 1, -1, c);   /* chart line    */
        line_diag(w, cx-2, cy-1, 3, 1,  1, c);
        line_diag(w, cx+1, cy+1, 5, 1, -1, c);
        break; }
    case 3: { /* apps grid 2x2 */
        int q=6;
        rect(w, cx-q-1, cy-q-1, q, q, c);
        rect(w, cx+1,   cy-q-1, q, q, c);
        rect(w, cx-q-1, cy+1,   q, q, c);
        rect(w, cx+1,   cy+1,   q, q, c);
        break; }
    case 4: { /* code </> */
        line_diag(w, cx-3, cy, 6, -1, -1, c);
        line_diag(w, cx-3, cy, 6, -1,  1, c);
        line_diag(w, cx+4, cy, 6,  1, -1, c);
        line_diag(w, cx+4, cy, 6,  1,  1, c);
        break; }
    case 5: { /* folder */
        line_h(w, cx-9, cy-5, 8, c);
        line_v(w, cx-1, cy-7, 3, c);
        line_h(w, cx-9, cy+6, 19, c);
        line_v(w, cx-9, cy-5, 11, c);
        line_v(w, cx+9, cy-3, 9, c);
        line_h(w, cx-1, cy-7, 11, c);
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
    wd->color = VUI_TEXT; wd->value=0; wd->max=100; wd->on_click=0; wd->user=0;
    wd->anchors = VUI_ANCHOR_LEFT | VUI_ANCHOR_TOP;
    wd->margin_l=wd->margin_t=wd->margin_r=wd->margin_b=0;
    wd->parent_idx=-1; wd->gap=4; wd->padding=0;
    wd->fill=0; wd->expand=0; wd->separator=0;
    wd->hover=0; wd->pressed=0; wd->visible=1;
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
    wd->x=x; wd->y=y; scopy(wd->text, t, sizeof(wd->text)); wd->w=slen(wd->text)*8; wd->h=16; wd->color=VUI_TEXT;
    init_margins(wd); w->dirty=1; return wd;
}
vui_widget *vui_button(vui_window *w, int x, int y, const char *t) {
    vui_widget *wd = new_widget(w, W_BUTTON);
    if (!wd) return 0;
    wd->x=x; wd->y=y; scopy(wd->text, t, sizeof(wd->text));
    wd->w = slen(wd->text)*8 + 26; wd->h = 26; wd->color = VUI_ACCENT;
    init_margins(wd); w->dirty=1; return wd;
}
vui_widget *vui_tile_button(vui_window *w, int x, int y, const char *t) {
    vui_widget *wd = new_widget(w, W_TILE);
    if (!wd) return 0;
    wd->x=x; wd->y=y; wd->w=52; wd->h=52; wd->color=VUI_ACCENT;
    scopy(wd->text, t?t:"", sizeof(wd->text));
    init_margins(wd); w->dirty=1; return wd;
}
vui_widget *vui_input(vui_window *w, int x, int y, int width, const char *placeholder) {
    vui_widget *wd = new_widget(w, W_INPUT);
    if (!wd) return 0;
    wd->x=x; wd->y=y; wd->w=width; wd->h=26; scopy(wd->text, placeholder?placeholder:"", sizeof(wd->text));
    init_margins(wd); w->dirty=1; return wd;
}
vui_widget *vui_badge(vui_window *w, int x, int y, const char *t) {
    vui_widget *wd = new_widget(w, W_BADGE);
    if (!wd) return 0;
    wd->x=x; wd->y=y; scopy(wd->text, t?t:"", sizeof(wd->text));
    wd->w=slen(wd->text)*8+18; wd->h=20; wd->color=VUI_ACCENT;
    init_margins(wd); w->dirty=1; return wd;
}
vui_widget *vui_tabs(vui_window *w, int x, int y, int width, const char *labels, int active) {
    vui_widget *wd = new_widget(w, W_TABS);
    if (!wd) return 0;
    wd->x=x; wd->y=y; wd->w=width; wd->h=28; scopy(wd->text, labels?labels:"", sizeof(wd->text));
    wd->value=active; init_margins(wd); w->dirty=1; return wd;
}
vui_widget *vui_bar(vui_window *w, int x, int y, int width, int height, int max) {
    vui_widget *wd = new_widget(w, W_BAR);
    if (!wd) return 0;
    wd->x=x; wd->y=y; wd->w=width; wd->h=height; wd->max=max>0?max:1; wd->color=VUI_ACCENT;
    init_margins(wd); w->dirty=1; return wd;
}

void vui_on_click(vui_widget *b, vui_callback cb){ if(b) b->on_click=cb; }
void vui_set_text(vui_widget *wd, const char *t){
    if(!wd)return;
    scopy(wd->text,t,sizeof(wd->text));
    if(wd->type==W_BUTTON && !(wd->anchors & VUI_ANCHOR_RIGHT)) wd->w=slen(wd->text)*8+24;
    if(wd->type==W_LABEL) wd->w=slen(wd->text)*8;
    if(wd->type==W_BADGE) wd->w=slen(wd->text)*8+18;
    g_win.dirty=1;
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
void vui_set_value(vui_widget *wd, int v){ if(!wd)return; wd->value=v; g_win.dirty=1; }
void vui_set_color(vui_widget *wd, vui_u32 c){ if(!wd)return; wd->color=c; g_win.dirty=1; }
void vui_set_user(vui_widget *wd, void *u){ if(wd) wd->user=u; }
void *vui_get_user(vui_widget *wd){ return wd?wd->user:0; }
void vui_set_visible(vui_widget *wd, int visible){ if(!wd)return; wd->visible=(uint8_t)(visible?1:0); g_win.dirty=1; }
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
void vui_on_tick(vui_window *w, vui_tick_callback cb){ if(w) w->on_tick=cb; }
void vui_on_resize(vui_window *w, vui_resize_callback cb){ if(w) w->on_resize=cb; }
void vui_on_context_menu(vui_window *w, vui_context_callback cb){ if(w) w->on_context_menu=cb; }
void vui_request_repaint(vui_window *w){ if(w) w->dirty=1; }

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
        int tw = slen(it->text) * 8 + 24; /* 12 px left padding + text + 12 px right */
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
        m->w = slen(m->text) * 8 + 16;
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
    wd->w = slen(wd->text) * 8 + 16;
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

    /* Pass A: measure */
    int n_children = 0, n_expand = 0, fixed_sum = 0;
    for (i = 0; i < w->widget_count; ++i) {
        if (w->widgets[i].parent_idx != box_idx) continue;
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

    /* Pass B: place */
    int cursor = (horiz ? box->x : box->y) + pad;
    for (i = 0; i < w->widget_count; ++i) {
        struct vui_widget *ch = &w->widgets[i];
        if (ch->parent_idx != box_idx) continue;

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
        if (wd->color != VUI_TEXT) glass_box(w, wd->x, wd->y, wd->w, wd->h, wd->color, 0);
        return;
    }
    /* Menu items are drawn as a dropdown overlay in repaint(), not here. */
    if (wd->type == W_MENUITEM) return;
    switch (wd->type) {
    case W_MENUBAR:
        /* Thin bar background + bottom separator line. */
        rect(w, wd->x, wd->y, wd->w, wd->h, VUI_PANEL);
        rect(w, wd->x, wd->y + wd->h - 1, wd->w, 1, VUI_BORDER);
        break;
    case W_MENU: {
        /* Menu title: highlight on hover or when this menu is open. */
        int is_active = ((int)(wd - w->widgets) == w->active_menu_idx);
        if (is_active || wd->hover) {
            vui_u32 hi = mix(VUI_ACCENT, VUI_PANEL, 1u, 5u);
            rect(w, wd->x + 1, wd->y + 2, wd->w - 2, wd->h - 4, hi);
        }
        vui_u32 fg = is_active ? VUI_TEXT : (wd->hover ? VUI_TEXT : VUI_TEXT_DIM);
        text(w, wd->x + 8, wd->y + 3, wd->text, fg);
        break; }
    case W_PANEL:
        glass_box(w, wd->x, wd->y, wd->w, wd->h, g_theme.surface, 0);
        if (wd->text[0]) {
            rect(w, wd->x + 1, wd->y + 1, wd->w - 2, 22, g_theme.surface_hi);
            rect(w, wd->x + 1, wd->y+23, wd->w - 2, 1, g_theme.border);
            rect(w, wd->x+8, wd->y+6, 3, 12, g_theme.accent);
            text(w, wd->x+16, wd->y+4, wd->text, g_theme.text);
        }
        break;
    case W_CARD:
        glass_box(w, wd->x, wd->y, wd->w, wd->h,
                  wd->color != VUI_TEXT ? wd->color : g_theme.surface, 1);
        if (wd->text[0]) text(w, wd->x + 12, wd->y + 9, wd->text, g_theme.text_dim);
        break;
    case W_LABEL:
        text(w, wd->x, wd->y, wd->text, wd->color);
        break;
    case W_BUTTON: {
        uint32_t accent = wd->color ? wd->color : VUI_ACCENT;
        uint32_t fill = wd->pressed ? mix(accent, 0x00161d2bu, 3u, 5u) :
                        (wd->hover ? mix(accent, 0x001f2942u, 1u, 4u) : 0x002a354du);
        uint32_t top = wd->pressed ? mix(fill, 0x00000000u, 1u, 4u) : mix(fill, 0x00ffffffu, 1u, 5u);
        uint32_t border = wd->hover || wd->pressed ? mix(accent, 0x00ffffffu, 1u, 4u) : 0x003e4d69u;
        int tx = wd->x + (wd->w - (slen(wd->text) * 8)) / 2;
        int ty = wd->y + (wd->h - 16) / 2;

        rect(w, wd->x + 2, wd->y + wd->h, wd->w - 4, 1, 0x000d1320u);
        rect(w, wd->x, wd->y, wd->w, wd->h, fill);
        rect(w, wd->x, wd->y, wd->w, 1, top);
        rect(w, wd->x, wd->y+wd->h-1, wd->w, 1, 0x00101826u);
        rect(w, wd->x, wd->y, 1, wd->h, border);
        rect(w, wd->x+wd->w-1, wd->y, 1, wd->h, border);
        rect(w, wd->x + 3, wd->y + 4, 3, wd->h - 8, accent);
        if (wd->hover && !wd->pressed) rect(w, wd->x + 7, wd->y + 3, wd->w - 10, 1, mix(accent, 0x00ffffffu, 1u, 3u));
        text(w, tx, ty, wd->text, VUI_TEXT);
        break; }
    case W_TILE: {
        uint32_t accent = wd->color ? wd->color : g_theme.accent;
        int cx = wd->x + wd->w / 2;
        int cy = wd->y + wd->h / 2;
        uint32_t ic;
        /* No permanent tile box — icons sit directly on the flat dock bar
         * (matches the reference). Only hover/press shows a subtle highlight. */
        if (wd->hover || wd->pressed) {
            uint32_t hl = wd->pressed ? mix(accent, g_theme.surface, 1u, 2u)
                                      : mix(accent, g_theme.surface, 1u, 4u);
            glass_box(w, wd->x + 3, wd->y + 3, wd->w - 6, wd->h - 6, hl, 0);
        }
        /* Monochrome line-art icon: light, brighter on hover. */
        ic = (wd->hover || wd->pressed) ? g_theme.text : g_theme.text_dim;
        if (wd->value > 0) {
            tile_icon(w, cx, cy, wd->value, ic);
        } else {
            int glyph_w = slen(wd->text) * 8;
            int tx = wd->x + (wd->w - glyph_w) / 2;
            int ty = wd->y + (wd->h - 16) / 2;
            if (tx < wd->x + 6) tx = wd->x + 6;
            text(w, tx, ty, wd->text, ic);
        }
        /* Faint vertical divider sitting in the gap to the right of the tile. */
        rect(w, wd->x + wd->w + 11, wd->y + 7, 1, wd->h - 14,
             mix(g_theme.border, g_theme.surface, 1u, 2u));
        break; }
    case W_INPUT: {
        int tx = wd->x + 28;
        glass_box(w, wd->x, wd->y, wd->w, wd->h, 0x00111b2au, wd->hover);
        /* small magnifier */
        line_h(w, wd->x + 12, wd->y + 8, 5, g_theme.text_dim);
        line_h(w, wd->x + 12, wd->y + 13, 5, g_theme.text_dim);
        line_v(w, wd->x + 11, wd->y + 9, 4, g_theme.text_dim);
        line_v(w, wd->x + 17, wd->y + 9, 4, g_theme.text_dim);
        put(w, wd->x + 18, wd->y + 14, g_theme.text_dim);
        put(w, wd->x + 19, wd->y + 15, g_theme.text_dim);
        text(w, tx, wd->y + 5, wd->text, g_theme.text_dim);
        break; }
    case W_BADGE: {
        uint32_t fill = mix(wd->color ? wd->color : g_theme.accent, g_theme.surface, 1u, 4u);
        glass_box(w, wd->x, wd->y, wd->w, wd->h, fill, 1);
        text(w, wd->x + 9, wd->y + 2, wd->text, g_theme.text);
        break; }
    case W_TABS: {
        int seg = 0, seg_start = 0, i = 0;
        int count = 1, per;
        while (wd->text[i]) { if (wd->text[i] == '|') count++; i++; }
        per = count > 0 ? wd->w / count : wd->w;
        glass_box(w, wd->x, wd->y, wd->w, wd->h, 0x00131d2cu, 0);
        for (seg = 0; seg < count; ++seg) {
            char label[24];
            int li = 0;
            int sx = wd->x + seg * per;
            int sw = (seg == count - 1) ? (wd->w - seg * per) : per;
            while (wd->text[seg_start] == '|') seg_start++;
            while (wd->text[seg_start] && wd->text[seg_start] != '|' && li + 1 < (int)sizeof(label))
                label[li++] = wd->text[seg_start++];
            label[li] = 0;
            if (seg == wd->value) {
                rect(w, sx + 2, wd->y + 2, sw - 4, wd->h - 4, g_theme.surface_hi);
                line_h(w, sx + 8, wd->y + wd->h - 2, sw - 16, g_theme.accent);
            } else if (seg > 0) {
                line_v(w, sx, wd->y + 6, wd->h - 12, g_theme.border);
            }
            text(w, sx + (sw - slen(label) * 8) / 2, wd->y + 6, label,
                 seg == wd->value ? g_theme.text : g_theme.text_dim);
        }
        break; }
    case W_BAR: {
        int fillw; int v=wd->value; if(v<0)v=0; if(v>wd->max)v=wd->max;
        fillw=(wd->w-2)*v/wd->max;
        glass_box(w, wd->x, wd->y, wd->w, wd->h, 0x00141b28u, 0);
        rect(w, wd->x+1, wd->y+1, fillw, wd->h-2, wd->color);
        break; }
    }
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

    /* Drop shadow */
    rect(w, dx + 2, dy + 2, dw, dh, 0x00080e18u);

    /* Panel background + border */
    rect(w, dx, dy, dw, dh, VUI_PANEL);
    rect(w, dx,        dy,        dw, 1,  VUI_BORDER);
    rect(w, dx,        dy + dh-1, dw, 1,  VUI_BORDER);
    rect(w, dx,        dy,        1,  dh, VUI_BORDER);
    rect(w, dx + dw-1, dy,        1,  dh, VUI_BORDER);

    /* Items — positions already set by layout_menu_dropdown(). */
    int i;
    for (i = 0; i < w->widget_count; ++i) {
        struct vui_widget *it = &w->widgets[i];
        if (it->type != W_MENUITEM || it->parent_idx != menu_idx) continue;
        if (it->separator) {
            rect(w, it->x + 6, it->y, it->w - 12, 1, VUI_BORDER);
        } else {
            if (it->hover)
                rect(w, it->x + 1, it->y + 1, it->w - 2, it->h - 2,
                     mix(VUI_ACCENT, VUI_PANEL, 1u, 4u));
            text(w, it->x + 12, it->y + 3, it->text, VUI_TEXT);
        }
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
    sc4(SYS_WINDOW_PRESENT, (uint64_t)w->id, (uint64_t)(size_t)g_canvas, (uint64_t)w->width, (uint64_t)w->height);
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
vui_window *vui_window_open_ex(const char *title, int width, int height,
                               vui_u32 flags, int x, int y) {
    static int theme_loaded = 0;
    int id;
    struct winsys_window_options options;
    if (!theme_loaded) {
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
    g_win.mouse_x=-1; g_win.mouse_y=-1; g_win.mouse_down=0;
    g_win.clear_color=g_theme.bg;
    g_win.widget_count=0; g_win.dirty=1; g_win.on_tick=0; g_win.on_resize=0; g_win.on_context_menu=0;
    g_win.menu_count=0; g_win.active_menu_idx=-1;
    return &g_win;
}

vui_window *vui_window_open(const char *title, int width, int height) {
    return vui_window_open_ex(title, width, height, 0, 0, 0);
}

void vui_quit(vui_window *w){ if(w) w->open=0; }

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
            if (ev.type == EV_MOUSE_MOVE) { w->mouse_x=ev.x; w->mouse_y=ev.y; }
            else if (ev.type == EV_MOUSE_DOWN) { w->mouse_x=ev.x; w->mouse_y=ev.y; w->mouse_down=1; click_x=ev.x; click_y=ev.y; }
            else if (ev.type == EV_MOUSE_UP) { w->mouse_down=0; }
            else if (ev.type == EV_CLOSE) { w->open=0; }
            else if (ev.type == EV_CONTEXT_MENU) { w->mouse_x=ev.x; w->mouse_y=ev.y; if(w->on_context_menu) w->on_context_menu(w, ev.x, ev.y); }
            else if (ev.type == EV_MENU_ACTION) { if (ev.key < (uint32_t)w->menu_count && w->menu_cbs[ev.key]) w->menu_cbs[ev.key](w); }
            else if (ev.type == EV_RESIZE) {
                int nw=ev.x, nh=ev.y;
                if(nw<1)nw=1; if(nh<1)nh=1;
                if(nw>VUI_MAX_W)nw=VUI_MAX_W; if(nh>VUI_MAX_H)nh=VUI_MAX_H;
                w->width=nw; w->height=nh; w->dirty=1;
                if(w->on_resize) w->on_resize(w,nw,nh);
            }
        }

        /* ---- Menu hover ------------------------------------------------- */
        /* Update hover for W_MENU titles (always, so they glow on mouse-over). */
        for (i = 0; i < w->widget_count; ++i) {
            struct vui_widget *m = &w->widgets[i];
            if (m->type != W_MENU) continue;
            uint8_t hov = (uint8_t)inside(m, w->mouse_x, w->mouse_y);
            if (hov != m->hover) { m->hover = hov; w->dirty = 1; }
        }
        /* Update hover for W_MENUITEM entries when their dropdown is open. */
        if (w->active_menu_idx >= 0) {
            for (i = 0; i < w->widget_count; ++i) {
                struct vui_widget *it = &w->widgets[i];
                if (it->type != W_MENUITEM || it->parent_idx != w->active_menu_idx
                    || it->separator) continue;
                uint8_t hov = (uint8_t)inside(it, w->mouse_x, w->mouse_y);
                if (hov != it->hover) { it->hover = hov; w->dirty = 1; }
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
        for (i = 0; i < w->widget_count; ++i) {
            struct vui_widget *wd = &w->widgets[i];
            if (wd->type != W_BUTTON && wd->type != W_TILE && wd->type != W_TABS && wd->type != W_INPUT) continue;
            uint8_t hov = (uint8_t)(w->active_menu_idx < 0 &&
                                    inside(wd, w->mouse_x, w->mouse_y));
            uint8_t prs = (uint8_t)(hov && w->mouse_down);
            if (hov != wd->hover || prs != wd->pressed) { wd->hover=hov; wd->pressed=prs; w->dirty=1; }
            if (click_x >= 0 && inside(wd, click_x, click_y)) {
                if (wd->type == W_TABS) {
                    int n = tab_count(wd->text);
                    if (n > 0) {
                        int rel = click_x - wd->x;
                        int tab = rel / (wd->w / n ? wd->w / n : 1);
                        if (tab < 0) tab = 0;
                        if (tab >= n) tab = n - 1;
                        if (tab != wd->value) { wd->value = tab; w->dirty = 1; }
                    }
                }
                if (wd->on_click) wd->on_click(wd);
            }
        }

        if (w->dirty) { repaint(w); w->dirty = 0; }
        do_yield();
    }
    sc1(SYS_EXIT, 0);
    for(;;){}
}
