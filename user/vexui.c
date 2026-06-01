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

#define SYS_WRITE 1
#define SYS_YIELD 3
#define SYS_EXIT 4
#define SYS_WINDOW_CREATE 17
#define SYS_WINDOW_PRESENT 18
#define SYS_EVENT_POLL 19
#define SYS_PROCESS_SNAPSHOT 28
#define SYS_PROCESS_KILL 29
#define SYS_WINDOW_SET_MENU 30

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
       W_MENUITEM  /* one entry inside a dropdown; also separators       */
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

#define VUI_MAX_WIDGETS 72
#define VUI_MAX_W 900
#define VUI_MAX_H 640

struct vui_window {
    int id, width, height, open;
    int mouse_x, mouse_y, mouse_down;
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
static void glyph(struct vui_window *w,int x,int y,char ch,vui_u32 c){
    const uint8_t *g=glyph_for_char(ch); int r,col;
    for(r=0;r<16;++r){ uint8_t b=g[r]; for(col=0;col<8;++col) if((b>>(7-col))&1u) put(w,x+col,y+r,c); }
}
static void text(struct vui_window *w,int x,int y,const char *s,vui_u32 c){
    int cx=x; while(s&&*s){ if(*s=='\n'){cx=x;y+=18;} else {glyph(w,cx,y,*s,c);cx+=8;} ++s; }
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
    if (wd->type == W_VBOX || wd->type == W_HBOX) return;
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
        rect(w, wd->x, wd->y, wd->w, wd->h, VUI_PANEL);
        rect(w, wd->x, wd->y, wd->w, 1, VUI_BORDER);
        rect(w, wd->x, wd->y+wd->h-1, wd->w, 1, VUI_BORDER);
        rect(w, wd->x, wd->y, 1, wd->h, VUI_BORDER);
        rect(w, wd->x+wd->w-1, wd->y, 1, wd->h, VUI_BORDER);
        if (wd->text[0]) {
            rect(w, wd->x, wd->y, wd->w, 22, 0x00263354u);
            rect(w, wd->x, wd->y+22, wd->w, 1, VUI_BORDER);
            rect(w, wd->x+8, wd->y+6, 3, 12, VUI_ACCENT);
            text(w, wd->x+16, wd->y+4, wd->text, VUI_TEXT);
        }
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
    case W_BAR: {
        int fillw; int v=wd->value; if(v<0)v=0; if(v>wd->max)v=wd->max;
        fillw=(wd->w-2)*v/wd->max;
        rect(w, wd->x, wd->y, wd->w, wd->h, 0x00141b28u);
        rect(w, wd->x, wd->y, wd->w, 1, VUI_BORDER);
        rect(w, wd->x, wd->y+wd->h-1, wd->w, 1, VUI_BORDER);
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
    rect(w, 0, 0, w->width, w->height, VUI_BG);
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

/* ---- public lifecycle ---- */
vui_window *vui_window_open(const char *title, int width, int height) {
    int id;
    if (width > VUI_MAX_W) width = VUI_MAX_W;
    if (height > VUI_MAX_H) height = VUI_MAX_H;
    id = (int)sc3(SYS_WINDOW_CREATE, (uint64_t)(size_t)title, (uint64_t)width, (uint64_t)height);
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
    g_win.widget_count=0; g_win.dirty=1; g_win.on_tick=0; g_win.on_resize=0; g_win.on_context_menu=0;
    g_win.menu_count=0; g_win.active_menu_idx=-1;
    return &g_win;
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

        /* ---- Button hover / click (only when no menu is consuming input) - */
        for (i = 0; i < w->widget_count; ++i) {
            struct vui_widget *wd = &w->widgets[i];
            if (wd->type != W_BUTTON) continue;
            uint8_t hov = (uint8_t)(w->active_menu_idx < 0 &&
                                    inside(wd, w->mouse_x, w->mouse_y));
            uint8_t prs = (uint8_t)(hov && w->mouse_down);
            if (hov != wd->hover || prs != wd->pressed) { wd->hover=hov; wd->pressed=prs; w->dirty=1; }
            if (click_x >= 0 && inside(wd, click_x, click_y) && wd->on_click) {
                wd->on_click(wd);
            }
        }

        if (w->dirty) { repaint(w); w->dirty = 0; }
        do_yield();
    }
    sc1(SYS_EXIT, 0);
    for(;;){}
}
