#include "task_manager.h"
#include "process_state.h"
#include "string_builder.h"
#include <sys/syscall.h>

TaskManager *TaskManager::s_instance_ = nullptr;

TaskManager::TaskManager() = default;

/* ---- Public ------------------------------------------------------------ */

void __attribute__((noreturn)) TaskManager::run() {
    uint32_t mode = (uint32_t)__sc2(SYS_DISPLAY_MODE, 0, 0);
    int screen_w = (int)((mode >> 16) & 0xffffu);
    int screen_h = (int)(mode & 0xffffu);
    int x = 780;
    int y = 250;

    if (screen_w > 0 && screen_h > 0) {
        if (x + 680 > screen_w) x = screen_w - 700;
        if (y + 520 > screen_h) y = screen_h - 560;
        if (x < 20) x = 20;
        if (y < 72) y = 72;
    }

    window_ = vui_window_open_ex("Task Manager", 680, 520, VUI_WINDOW_POSITIONED, x, y);
    build_ui();
    refresh();
    vui_on_tick(window_, on_tick_cb);
    vui_run(window_);
}

/* ---- Private — UI construction ----------------------------------------- */
void TaskManager::build_ui() {
    /* ---- Metric cards --------------------------------------------------- */
    vui_set_anchor(vui_card(window_, 16, 14, 144, 66, "Processes"),
                   VUI_ANCHOR_LEFT | VUI_ANCHOR_TOP);
    total_label_ = vui_label(window_, 28, 42, "0 / 0");
    vui_set_color(total_label_, VUI_ACCENT);

    vui_set_anchor(vui_card(window_, 168, 14, 144, 66, "Scheduler"),
                   VUI_ANCHOR_LEFT | VUI_ANCHOR_TOP);
    ready_label_ = vui_label(window_, 180, 42, "READY 0");
    vui_set_color(ready_label_, VUI_OK);

    vui_set_anchor(vui_card(window_, 320, 14, 144, 66, "Memory"),
                   VUI_ANCHOR_LEFT | VUI_ANCHOR_TOP);
    mem_label_ = vui_label(window_, 332, 42, "0 B");
    vui_set_color(mem_label_, VUI_WARN);

    vui_set_anchor(vui_card(window_, 472, 14, 152, 66, "Status"),
                   VUI_ANCHOR_TOP | VUI_ANCHOR_RIGHT);
    status_label_ = vui_label(window_, 484, 42, "READY");
    vui_set_color(status_label_, VUI_TEXT_DIM);

    /* ---- Navigation/search strip ---------------------------------------- */
    auto *tabs = vui_tabs(window_, 16, 94, 392, "PROCESSES|SYSTEM|SERVICES|PERF", 0);
    vui_set_anchor(tabs, VUI_ANCHOR_LEFT | VUI_ANCHOR_TOP);

    auto *search = vui_input(window_, 422, 95, 202, "Search processes...");
    vui_set_anchor(search, VUI_ANCHOR_TOP | VUI_ANCHOR_RIGHT);

    /* ---- Processes panel ------------------------------------------------ */
    vui_set_anchor(vui_panel(window_, 16, 130, 608, 274, ""),
                   VUI_ANCHOR_LEFT | VUI_ANCHOR_TOP | VUI_ANCHOR_RIGHT | VUI_ANCHOR_BOTTOM);

    /* Column header HBox — stretches horizontally with the panel. */
    auto *headers = vui_hbox(window_, 30, 146, 580, 16);
    vui_set_anchor(headers, VUI_ANCHOR_LEFT | VUI_ANCHOR_TOP | VUI_ANCHOR_RIGHT);
    vui_set_gap(headers, 6);

    /* Column headers — widths mirror the per-row columns in process_row.cpp
     * (name expands; RAM 88, THR 60, STATE 72, action/kill 52). */
    auto *h_process = vui_label(window_, 0, 0, "PROCESS");
    vui_set_expand(h_process);
    vui_box_add(headers, h_process);

    auto *h_ram = vui_label(window_, 0, 0, "RAM");
    vui_set_size(h_ram, 88, 0);
    vui_box_add(headers, h_ram);

    auto *h_thr = vui_label(window_, 0, 0, "THREADS");
    vui_set_size(h_thr, 60, 0);
    vui_box_add(headers, h_thr);

    auto *h_state = vui_label(window_, 0, 0, "STATE");
    vui_set_size(h_state, 72, 0);
    vui_box_add(headers, h_state);

    auto *h_action = vui_label(window_, 0, 0, "ACTION");
    vui_set_size(h_action, 52, 0);
    vui_box_add(headers, h_action);

    /* Process-rows VBox — fills the remaining panel area on all sides. */
    rows_vbox_ = vui_vbox(window_, 30, 170, 580, 220);
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
    unsigned int  loaded = 0, ready = 0, sleeping = 0, total_threads = 0;
    unsigned long total_mem = 0;

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
        total_mem     += p.mem_bytes;
        total_threads += p.thread_count;
        rows_[slot].update(p);
    }

    StringBuilder<48> total;
    total.append("PROC ").append(loaded)
         .append(" / ").append(static_cast<unsigned int>(VUI_PROCESS_MAX));
    vui_set_text(total_label_, total.c_str());

    StringBuilder<48> stats;
    stats.append("READY ").append(ready)
         .append("  SLEEP ").append(sleeping);
    vui_set_text(ready_label_, stats.c_str());

    StringBuilder<48> mem;
    mem.append_size(total_mem).append("  ").append(total_threads).append(" thr");
    vui_set_text(mem_label_, mem.c_str());
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
