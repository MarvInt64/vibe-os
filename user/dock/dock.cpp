#include "vexui.h"
#include <vibeos.h>
#include <string.h>
#include <stdio.h>

#define PROCESS_STATE_EMPTY  0
#define PROCESS_STATE_EXITED 5

struct DockEntry {
    const char *label;
    int         icon;
    uint32_t    color;
    const char *path;
    const char *icon_path;
};

static DockEntry g_entries[] = {
    {"Terminal", 4, 0x002ea8ffu, "/bin/terminal", "/icons/dock/terminal.svg"},
    {"Files",    5, 0x002ea8ffu, "/bin/filebrowser", "/icons/dock/filebrowser.svg"},
    {"Browser",  1, 0x002ea8ffu, "/bin/browser", "/icons/dock/browser.svg"},
    {"Editor",   4, 0x002ea8ffu, "/bin/texteditor", "/icons/dock/terminal.svg"},
    {"Tasks",    2, 0x002ea8ffu, "/bin/taskmgr", "/icons/dock/taskmgr.svg"},
    {"Player",   3, 0x002ea8ffu, "/bin/audioplayer", "/icons/dock/player.svg"},
};
#define DOCK_COUNT (int)(sizeof(g_entries) / sizeof(g_entries[0]))

/* Icon geometry: 58px buttons, 38px gap, starting x=50. */
#define ICON_SIZE   58
#define ICON_GAP    38
#define ICON_STRIDE (ICON_SIZE + ICON_GAP)
#define ICON_X0     50
#define ICON_Y0     48

static vui_widget *sButtons[6];

/* Drag-mode overlay icons: one per slot, created once and reused.
 * During normal operation they're hidden; during drag they replace
 * the hbox buttons so we can animate positions freely. */
static vui_widget *sOverlays[6];

static void apply_entry(int idx) {
    vui_widget *b = sButtons[idx];
    DockEntry *e = &g_entries[idx];
    vui_set_color(b, e->color);
    vui_set_value(b, e->icon);
    if (e->icon_path) vui_set_icon_svg_path(b, e->icon_path);
    vui_set_user(b, (void *)e->path);
    vui_set_tooltip(b, e->label);
}

static void apply_overlay(vui_widget *ov, int entry_idx) {
    DockEntry *e = &g_entries[entry_idx];
    vui_set_color(ov, e->color);
    vui_set_value(ov, e->icon);
    if (e->icon_path) vui_set_icon_svg_path(ov, e->icon_path);
}

static void dock_on_tick(vui_window *win) {
    (void)win;
    for (int i = 0; i < DOCK_COUNT; i++) {
        bool running = false;
        const char *path = g_entries[i].path;
        const char *name = path;
        for (const char *p = path; *p; p++)
            if (*p == '/') name = p + 1;
        for (unsigned s = 0; s < VUI_PROCESS_MAX; s++) {
            vui_process_info p;
            if (vui_process_snapshot(s, &p) > 0 && p.loaded
                && p.state != PROCESS_STATE_EMPTY && p.state != PROCESS_STATE_EXITED) {
                if (strcmp(p.name, name) == 0 || strcmp(p.name, path) == 0) {
                    running = true; break;
                }
            }
        }
        vui_set_running(sButtons[i], running ? 1 : 0);
    }
}

/* ---- Drag state -------------------------------------------------------- */
static int  g_held_idx   = -1;
static int  g_held_ticks = 0;
static int  g_drag_on    = 0;

/* g_target_slot[i] = which visual slot entry i should occupy during drag.
 * The dragged entry's slot tracks the mouse; neighbours shift accordingly. */
static int   g_target_slot[DOCK_COUNT];
static float g_cur_x[DOCK_COUNT];      /* animated x for each button */
static int   g_animating = 0;
static int   g_drag_src  = -1;         /* entry index being dragged */

static int slot_x(int slot) { return ICON_X0 + slot * ICON_STRIDE; }

static int icon_at(int mx, int my) {
    if (my < 28 || my > 106) return -1;
    for (int i = 0; i < DOCK_COUNT; i++) {
        int ix = ICON_X0 + i * ICON_STRIDE;
        if (mx >= ix && mx < ix + ICON_STRIDE) return i;
    }
    return -1;
}

/* Rebuild g_target_slot[]: the dragged entry (src_entry) appears at
 * mouse_slot; entries between src_entry and mouse_slot shift by one. */
static void recalc_targets(int src_entry, int mouse_slot) {
    /* Default: identity mapping (entry i → slot i). */
    for (int i = 0; i < DOCK_COUNT; i++)
        g_target_slot[i] = i;

    if (mouse_slot < 0 || mouse_slot >= DOCK_COUNT || mouse_slot == g_target_slot[src_entry])
        return;

    int src_slot = g_target_slot[src_entry];

    if (mouse_slot > src_slot) {
        for (int e = 0; e < DOCK_COUNT; e++) {
            int s = g_target_slot[e];
            if (s > src_slot && s <= mouse_slot)
                g_target_slot[e] = s - 1;
        }
        g_target_slot[src_entry] = mouse_slot;
    } else {
        for (int e = 0; e < DOCK_COUNT; e++) {
            int s = g_target_slot[e];
            if (s >= mouse_slot && s < src_slot)
                g_target_slot[e] = s + 1;
        }
        g_target_slot[src_entry] = mouse_slot;
    }
}

/* Lerp overlays toward their target slots. */
static void animate_drag(void) {
    float speed = 8.0f;
    for (int i = 0; i < DOCK_COUNT; i++) {
        float target = (float)slot_x(g_target_slot[i]);
        float dx = target - g_cur_x[i];
        if (dx < -speed)      g_cur_x[i] -= speed;
        else if (dx > speed)  g_cur_x[i] += speed;
        else                  g_cur_x[i] = target;

        vui_set_bounds(sOverlays[i], (int)(g_cur_x[i] + 0.5f),
                       ICON_Y0, ICON_SIZE, ICON_SIZE);
    }
}

/* Apply the final reorder from g_target_slot to g_entries. */
static void commit_targets(void) {
    DockEntry old[DOCK_COUNT];
    for (int i = 0; i < DOCK_COUNT; i++) old[i] = g_entries[i];
    /* Invert: find which entry goes to each slot. */
    for (int slot = 0; slot < DOCK_COUNT; slot++) {
        for (int e = 0; e < DOCK_COUNT; e++) {
            if (g_target_slot[e] == slot) { g_entries[slot] = old[e]; break; }
        }
    }
}

/* --- Drag lifecycle ----------------------------------------------------- */

static void start_drag(vui_window *win, int idx) {
    (void)win;
    if (idx < 0 || idx >= DOCK_COUNT) return;
    g_drag_on  = 1;
    g_drag_src = idx;
    g_animating = 0;

    /* Hide all hbox buttons, show overlay icons instead. */
    for (int i = 0; i < DOCK_COUNT; i++) {
        vui_set_visible(sButtons[i], 0);
        g_cur_x[i] = (float)slot_x(i);
        g_target_slot[i] = i;
        apply_overlay(sOverlays[i], i);
        vui_set_visible(sOverlays[i], 1);
    }

    vos_log(VOS_LOG_APP, "dock: drag started");
}

static void end_drag(int x, int y) {
    int dst = icon_at(x, y);

    /* Apply the final reorder. */
    recalc_targets(g_drag_src, dst);
    commit_targets();

    /* Hide overlays, show hbox buttons with updated entries. */
    for (int i = 0; i < DOCK_COUNT; i++) {
        vui_set_visible(sOverlays[i], 0);
        vui_set_visible(sButtons[i], 1);
        apply_entry(i);
    }

    g_drag_on    = 0;
    g_held_idx   = -1;
    g_held_ticks = 0;
    g_drag_src   = -1;
    g_animating  = 0;
}

/* ---- Window-level callbacks -------------------------------------------- */

static void on_mouse_dn(vui_window *win, int x, int y, vui_u32 btns) {
    (void)win; (void)btns;
    int idx = icon_at(x, y);
    g_held_idx   = idx;
    g_held_ticks = 0;
    g_drag_on    = 0;
}

static void on_mouse_up(vui_window *win, int x, int y, vui_u32 btns) {
    (void)win; (void)btns;

    if (g_drag_on) {
        end_drag(x, y);
        return;
    }

    if (g_held_idx >= 0) {
        int over = icon_at(x, y);
        if (over == g_held_idx) {
            const char *path = (const char *)vui_get_user(sButtons[g_held_idx]);
            if (path) vos_spawn(path);
        }
    }
    g_held_idx   = -1;
    g_held_ticks = 0;
}

static void on_mouse_mv(vui_window *win, int x, int y, vui_u32 btns) {
    (void)win; (void)btns;

    if (!g_drag_on || g_drag_src < 0) return;

    int mouse_slot = icon_at(x, y);
    recalc_targets(g_drag_src, mouse_slot);
    g_animating = 1;

    /* Dragged icon follows the mouse freely on the horizontal axis,
     * centred under the cursor (macOS-style). */
    float gx = (float)(x - ICON_SIZE / 2);
    if (gx < 0) gx = 0;
    g_cur_x[g_drag_src] = gx;
}

static void dock_tick2(vui_window *win) {
    dock_on_tick(win);

    if (g_held_idx >= 0 && !g_drag_on) {
        if (++g_held_ticks > 25) start_drag(win, g_held_idx);
    }

    if (g_drag_on && g_animating) animate_drag();
}

int main() {
    uint32_t mode = vos_display_mode_get();
    int screen_w = (int)((mode >> 16) & 0xffffu);
    int screen_h = (int)(mode & 0xffffu);
    int width, dock_h = 78, tooltip_h = 28, height = dock_h + tooltip_h, x, y;

    if (screen_w <= 0) screen_w = 1024;
    if (screen_h <= 0) screen_h = 768;
    width = 680;
    if (screen_w < 900) width = screen_w - 40;
    if (width > screen_w - 48) width = screen_w - 48;
    x = (screen_w - width) / 2;
    y = screen_h - height - 24;
    if (y < 0) y = 0;

    vos_log(VOS_LOG_APP, "dock ready");

    vui_window *win = vui_window_open_inset(
        "Dock", width, height,
        VUI_WINDOW_FRAMELESS | VUI_WINDOW_NO_DOCK |
            VUI_WINDOW_POSITIONED | VUI_WINDOW_ALWAYS_ON_TOP |
            VUI_WINDOW_TRANSLUCENT | VUI_WINDOW_SINGLE_INSTANCE,
        x, y, tooltip_h);
    vui_set_clear_color(win, VUI_COLOR_TRANSPARENT);

    vui_widget *surface = vui_pill(win, 0, tooltip_h, width, dock_h);
    vui_set_anchor(surface, VUI_ANCHOR_LEFT | VUI_ANCHOR_TOP | VUI_ANCHOR_RIGHT);
    vui_set_color(surface, 0x000b2342u);

    vui_widget *row = vui_hbox(win, 50, tooltip_h + 10, width - 100, dock_h - 18);
    vui_set_anchor(row, VUI_ANCHOR_LEFT | VUI_ANCHOR_TOP | VUI_ANCHOR_RIGHT);
    vui_set_gap(row, 38);
    vui_set_padding(row, 0);

    for (int i = 0; i < DOCK_COUNT; i++) {
        vui_widget *button = vui_tile_button(win, 0, 0, "");
        sButtons[i] = button;
        vui_set_size(button, 58, 58);
        apply_entry(i);
        vui_box_add(row, button);

        /* Pre-create overlay tiles for drag animation, hidden initially.
         * Using vui_tile_button (W_TILE) so they render identically to the
         * normal dock buttons: dark rounded-rect pill + SVG icon. */
        vui_widget *ov = vui_tile_button(win, slot_x(i), ICON_Y0, "");
        sOverlays[i] = ov;
        vui_set_size(ov, ICON_SIZE, ICON_SIZE);
        vui_set_visible(ov, 0);
    }

    vui_on_tick(win, dock_tick2);
    vui_on_mouse_click(win, on_mouse_dn);
    vui_on_mouse_release(win, on_mouse_up);
    vui_on_mouse_move(win, on_mouse_mv);

    vui_run(win);
}
