#include "task_manager.h"
#include "process_state.h"
#include "string_builder.h"

TaskManager *TaskManager::s_instance_ = nullptr;

TaskManager::TaskManager() = default;

/* ---- Public ------------------------------------------------------------ */

void __attribute__((noreturn)) TaskManager::run() {
    window_ = vui_window_open("Task Manager", 560, 340);
    build_ui();
    refresh();
    vui_on_tick(window_, on_tick_cb);
    vui_run(window_);
}

/* ---- Private — UI construction ----------------------------------------- */
/*
 * Layout tree:
 *
 *   Window (560 × 340)
 *   ├── "TASK MANAGER" label          absolute, top-left
 *   ├── subtitle label                absolute
 *   ├── Refresh button                absolute, right-anchored
 *   │
 *   ├── System panel  (resizes H)
 *   │   └── summary_hbox  (HBox, anchors L+T+R)
 *   │       ├── total_label  (expand)  — "PROCESSES n / 4"
 *   │       ├── ready_label  (expand)  — "READY n   SLEEP n"
 *   │       └── status_label           — last-action feedback
 *   │
 *   └── Processes panel  (resizes all)
 *       ├── header_hbox  (HBox, anchors L+T+R)
 *       │   ├── "PROCESS" label  (expand)
 *       │   ├── "STATE"   label
 *       │   └── "ACTION"  label
 *       └── rows_vbox_  (VBox, anchors all)
 *           └── ProcessRow × VUI_PROCESS_MAX
 *               └── (see process_row.h for internal structure)
 */
void TaskManager::build_ui() {
    /* ---- Menu bar ------------------------------------------------------- */
    auto *mb        = vui_menubar(window_);
    auto *view_menu = vui_menu(window_, mb, "View");
    vui_on_click(vui_menuitem(window_, view_menu, "Refresh"), on_refresh_cb);
    vui_menu_separator(window_, view_menu);
    vui_on_click(vui_menuitem(window_, view_menu, "Quit"), on_quit_cb);

    /* ---- Title bar (below menu bar) ------------------------------------- */
    vui_label(window_, 18, 12 + VUI_MENUBAR_HEIGHT, "TASK MANAGER");

    auto *subtitle = vui_label(window_, 152, 12 + VUI_MENUBAR_HEIGHT, "LIVE PROCESS CONTROL");
    vui_set_color(subtitle, VUI_TEXT_DIM);

    auto *refresh_btn = vui_button(window_, 458, 7 + VUI_MENUBAR_HEIGHT, "Refresh");
    vui_set_button_width(refresh_btn, 76);
    vui_set_anchor(refresh_btn, VUI_ANCHOR_TOP | VUI_ANCHOR_RIGHT);
    vui_on_click(refresh_btn, on_refresh_cb);

    /* ---- System summary panel ------------------------------------------- */
    vui_set_anchor(vui_panel(window_, 16, 40 + VUI_MENUBAR_HEIGHT, 528, 58, "System"),
                   VUI_ANCHOR_LEFT | VUI_ANCHOR_TOP | VUI_ANCHOR_RIGHT);

    /* HBox distributes the three stat labels across the panel's inner width. */
    auto *summary = vui_hbox(window_, 30, 62 + VUI_MENUBAR_HEIGHT, 500, 20);
    vui_set_anchor(summary, VUI_ANCHOR_LEFT | VUI_ANCHOR_TOP | VUI_ANCHOR_RIGHT);
    vui_set_gap(summary, 0);

    total_label_ = vui_label(window_, 0, 0, "PROCESSES");
    vui_set_color(total_label_, VUI_ACCENT);
    vui_set_expand(total_label_);
    vui_box_add(summary, total_label_);

    ready_label_ = vui_label(window_, 0, 0, "READY");
    vui_set_color(ready_label_, VUI_OK);
    vui_set_expand(ready_label_);
    vui_box_add(summary, ready_label_);

    status_label_ = vui_label(window_, 0, 0, "READY");
    vui_set_color(status_label_, VUI_TEXT_DIM);
    vui_box_add(summary, status_label_);

    /* ---- Processes panel ------------------------------------------------ */
    vui_set_anchor(vui_panel(window_, 16, 112 + VUI_MENUBAR_HEIGHT, 528, 210, "Processes"),
                   VUI_ANCHOR_LEFT | VUI_ANCHOR_TOP | VUI_ANCHOR_RIGHT | VUI_ANCHOR_BOTTOM);

    /* Column header HBox — stretches horizontally with the panel. */
    auto *headers = vui_hbox(window_, 30, 136 + VUI_MENUBAR_HEIGHT, 500, 16);
    vui_set_anchor(headers, VUI_ANCHOR_LEFT | VUI_ANCHOR_TOP | VUI_ANCHOR_RIGHT);
    vui_set_gap(headers, 6);

    auto *h_process = vui_label(window_, 0, 0, "PROCESS");
    vui_set_expand(h_process);
    vui_box_add(headers, h_process);

    auto *h_state = vui_label(window_, 0, 0, "STATE");
    vui_set_size(h_state, 80, 0);
    vui_box_add(headers, h_state);

    auto *h_action = vui_label(window_, 0, 0, "ACTION");
    vui_set_size(h_action, 52, 0);
    vui_box_add(headers, h_action);

    /* Process-rows VBox — fills the remaining panel area on all sides. */
    rows_vbox_ = vui_vbox(window_, 30, 158 + VUI_MENUBAR_HEIGHT, 500, 155);
    vui_set_anchor(rows_vbox_,
                   VUI_ANCHOR_LEFT | VUI_ANCHOR_TOP | VUI_ANCHOR_RIGHT | VUI_ANCHOR_BOTTOM);
    vui_set_gap(rows_vbox_, 0);
    vui_set_padding(rows_vbox_, 0);

    /* Each ProcessRow adds its own row_vbox into rows_vbox_. */
    for (int i = 0; i < VUI_PROCESS_MAX; ++i)
        rows_[i].init(window_, rows_vbox_, i, on_kill_cb);
}

/* ---- Private — data refresh -------------------------------------------- */

void TaskManager::refresh() {
    unsigned int loaded = 0, ready = 0, sleeping = 0;

    for (int slot = 0; slot < VUI_PROCESS_MAX; ++slot) {
        vui_process_info p{};
        auto s = ProcessState::Empty;

        if (vui_process_snapshot(static_cast<unsigned int>(slot), &p) > 0)
            s = static_cast<ProcessState>(p.state);

        if (!p.loaded || !state_is_active(s)) {
            rows_[slot].hide();
            continue;
        }

        ++loaded;
        if (s == ProcessState::Ready)    ++ready;
        if (s == ProcessState::Sleeping) ++sleeping;
        rows_[slot].update(p);
    }

    StringBuilder<64> total;
    total.append("PROCESSES ").append(loaded)
         .append(" / ").append(static_cast<unsigned int>(VUI_PROCESS_MAX));
    vui_set_text(total_label_, total.c_str());

    StringBuilder<64> stats;
    stats.append("READY ").append(ready)
         .append("   SLEEP ").append(sleeping);
    vui_set_text(ready_label_, stats.c_str());
}

void TaskManager::set_status(const char *text, vui_u32 color) {
    vui_set_text(status_label_, text);
    vui_set_color(status_label_, color);
}

/* ---- Static callbacks -------------------------------------------------- */

void TaskManager::on_kill_cb(vui_widget *self) {
    /* Recover the slot index stored in the button's user pointer. */
    int slot = static_cast<int>(
        reinterpret_cast<unsigned long>(vui_get_user(self)));
    if (slot < 0 || slot >= VUI_PROCESS_MAX) return;

    unsigned int pid = s_instance_->rows_[slot].pid();
    if (pid == 0) return;

    int result = vui_process_kill(pid);
    if (result == 0) {
        s_instance_->set_status("KILL SENT", VUI_WARN);
    } else {
        StringBuilder<32> msg;
        msg.append("KILL ERR ").append(result);
        s_instance_->set_status(msg.c_str(), VUI_DANGER);
    }
    s_instance_->refresh();
}

void TaskManager::on_refresh_cb(vui_widget *) {
    s_instance_->set_status("REFRESHED", VUI_ACCENT);
    s_instance_->refresh();
}

void TaskManager::on_quit_cb(vui_widget *) {
    vui_quit(s_instance_->window_);
}

void TaskManager::on_tick_cb(vui_window *) {
    /* Auto-refresh approximately once per second (VexUI ticks at ~18 Hz). */
    if ((++s_instance_->tick_ % 18) == 0)
        s_instance_->refresh();
}
