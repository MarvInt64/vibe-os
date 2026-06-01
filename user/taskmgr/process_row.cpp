#include "process_row.h"
#include "process_state.h"
#include "string_builder.h"

/* Widget geometry constants — keep them in one place so the whole row
 * can be adjusted without hunting through the code. */
static constexpr int kTopRowHeight  = 18;
static constexpr int kRamWidth      = 88;
static constexpr int kThreadWidth   = 60;
static constexpr int kStateWidth    = 72;
static constexpr int kKillWidth     = 52;
static constexpr int kRowPadding    = 3;
static constexpr int kRowGap        = 6;

/* RAM colour thresholds (bytes): green under 8 MB, amber under 20 MB, red above.
 * The browser's static umalloc heap pushes it into the red, which is useful
 * signal rather than alarm. */
static vui_u32 ram_color(unsigned long bytes) {
    if (bytes < 8UL * 1024 * 1024)  return VUI_OK;
    if (bytes < 20UL * 1024 * 1024) return VUI_WARN;
    return VUI_DANGER;
}

void ProcessRow::init(vui_window *win, vui_widget *rows_vbox, int slot,
                      void (*kill_cb)(vui_widget *)) {
    /* ---- Outer row: compact table row ------------------------------------ */
    row_vbox_ = vui_vbox(win, 0, 0, 0, 0);
    vui_set_color(row_vbox_, (slot & 1) ? 0x00141f30u : 0x00182334u);
    vui_set_padding(row_vbox_, kRowPadding);
    vui_set_gap(row_vbox_, 2);
    vui_set_expand(row_vbox_); /* each row shares the outer VBox's height evenly */
    vui_set_fill(row_vbox_);   /* stretch to the full width of rows_vbox         */
    vui_box_add(rows_vbox, row_vbox_);

    /* ---- Top bar: name | ram | threads | state | kill -------------------- */
    auto *top_hbox = vui_hbox(win, 0, 0, 0, kTopRowHeight);
    vui_set_gap(top_hbox, kRowGap);
    vui_set_fill(top_hbox); /* fill the row_vbox's width */
    vui_box_add(row_vbox_, top_hbox);

    name_label_ = vui_label(win, 0, 0, "");
    vui_set_expand(name_label_);    /* grows to push the columns to the right */
    vui_set_fill(name_label_);
    vui_box_add(top_hbox, name_label_);

    ram_label_ = vui_label(win, 0, 0, "");
    vui_set_size(ram_label_, kRamWidth, 0);
    vui_box_add(top_hbox, ram_label_);

    thread_label_ = vui_label(win, 0, 0, "");
    vui_set_size(thread_label_, kThreadWidth, 0);
    vui_box_add(top_hbox, thread_label_);

    state_label_ = vui_badge(win, 0, 0, "");
    vui_set_size(state_label_, kStateWidth, 0);
    vui_box_add(top_hbox, state_label_);

    kill_button_ = vui_button(win, 0, 0, "KILL");
    vui_set_button_width(kill_button_, kKillWidth);
    vui_set_color(kill_button_, VUI_DANGER);
    /* Store slot index so TaskManager::on_kill_cb can map widget → pid. */
    vui_set_user(kill_button_, reinterpret_cast<void *>(static_cast<unsigned long>(slot)));
    vui_on_click(kill_button_, kill_cb);
    vui_box_add(top_hbox, kill_button_);

    metric_label_ = vui_label(win, 0, 0, "");
    vui_set_color(metric_label_, VUI_TEXT_DIM);
    vui_set_visible(metric_label_, 0);

    pid_ = 0;
}

void ProcessRow::update(const vui_process_info &p) {
    auto s = static_cast<ProcessState>(p.state);
    pid_ = p.pid;

    StringBuilder<96> name;
    name.append("PID ").append(p.pid)
        .append("  ").append(p.name[0] ? p.name : "process");

    StringBuilder<24> ram;
    ram.append_size(p.mem_bytes);

    StringBuilder<16> threads;
    threads.append(p.thread_count).append(" thr");

    vui_set_text(name_label_,   name.c_str());
    vui_set_color(name_label_,  s == ProcessState::Running ? VUI_TEXT : VUI_TEXT_DIM);

    vui_set_text(ram_label_,    ram.c_str());
    vui_set_color(ram_label_,   ram_color(p.mem_bytes));

    vui_set_text(thread_label_, threads.c_str());
    vui_set_color(thread_label_, p.thread_count > 1 ? VUI_ACCENT : VUI_TEXT_DIM);

    vui_set_text(state_label_,  state_name(s));
    vui_set_color(state_label_, state_color(s));

    /* The running process is the scheduler — disallow killing it. */
    vui_set_visible(kill_button_, s != ProcessState::Running ? 1 : 0);

    vui_set_visible(row_vbox_,     1);
    vui_set_visible(name_label_,   1);
    vui_set_visible(ram_label_,    1);
    vui_set_visible(thread_label_, 1);
    vui_set_visible(state_label_,  1);
    vui_set_visible(metric_label_, 0);
}

void ProcessRow::hide() {
    pid_ = 0;
    /* Hide all child widgets; the row_vbox stays in layout (keeps its slot). */
    vui_set_visible(name_label_,   0);
    vui_set_visible(ram_label_,    0);
    vui_set_visible(thread_label_, 0);
    vui_set_visible(state_label_,  0);
    vui_set_visible(kill_button_,  0);
    vui_set_visible(metric_label_, 0);
}
