#include "process_row.h"
#include "process_state.h"
#include "string_builder.h"

/* Widget geometry constants — keep them in one place so the whole row
 * can be adjusted without hunting through the code. */
static constexpr int kTopRowHeight  = 18;
static constexpr int kMetricHeight  = 14;
static constexpr int kStateWidth    = 80;
static constexpr int kKillWidth     = 52;
static constexpr int kRowPadding    = 6;
static constexpr int kRowGap        = 6;

void ProcessRow::init(vui_window *win, vui_widget *rows_vbox, int slot,
                      void (*kill_cb)(vui_widget *)) {
    /* ---- Outer row: vertical stack of top-bar + metric line -------------- */
    row_vbox_ = vui_vbox(win, 0, 0, 0, 0);
    vui_set_padding(row_vbox_, kRowPadding);
    vui_set_gap(row_vbox_, 2);
    vui_set_expand(row_vbox_); /* each row shares the outer VBox's height evenly */
    vui_set_fill(row_vbox_);   /* stretch to the full width of rows_vbox         */
    vui_box_add(rows_vbox, row_vbox_);

    /* ---- Top bar: name | state | kill ------------------------------------ */
    auto *top_hbox = vui_hbox(win, 0, 0, 0, kTopRowHeight);
    vui_set_gap(top_hbox, kRowGap);
    vui_set_fill(top_hbox); /* fill the row_vbox's width */
    vui_box_add(row_vbox_, top_hbox);

    name_label_ = vui_label(win, 0, 0, "");
    vui_set_expand(name_label_);    /* grows to push state and kill to the right */
    vui_set_fill(name_label_);
    vui_box_add(top_hbox, name_label_);

    state_label_ = vui_label(win, 0, 0, "");
    vui_set_size(state_label_, kStateWidth, 0); /* fixed width — no expand/fill */
    vui_box_add(top_hbox, state_label_);

    kill_button_ = vui_button(win, 0, 0, "KILL");
    vui_set_button_width(kill_button_, kKillWidth);
    vui_set_color(kill_button_, VUI_DANGER);
    /* Store slot index so TaskManager::on_kill_cb can map widget → pid. */
    vui_set_user(kill_button_, reinterpret_cast<void *>(static_cast<unsigned long>(slot)));
    vui_on_click(kill_button_, kill_cb);
    vui_box_add(top_hbox, kill_button_);

    /* ---- Metric line ----------------------------------------------------- */
    metric_label_ = vui_label(win, 0, 0, "");
    vui_set_size(metric_label_, 0, kMetricHeight);
    vui_set_color(metric_label_, VUI_TEXT_DIM);
    vui_set_fill(metric_label_);
    vui_box_add(row_vbox_, metric_label_);

    pid_ = 0;
}

void ProcessRow::update(const vui_process_info &p) {
    auto s = static_cast<ProcessState>(p.state);
    pid_ = p.pid;

    StringBuilder<96> name;
    name.append("PID ").append(p.pid)
        .append("  ").append(p.name[0] ? p.name : "process");

    StringBuilder<80> metric;
    metric.append("ticks ").append(p.runtime_ticks)
          .append("   switches ").append(p.switch_count)
          .append("   parent ").append(p.parent_pid);

    vui_set_text(name_label_,   name.c_str());
    vui_set_color(name_label_,  s == ProcessState::Running ? VUI_TEXT : VUI_TEXT_DIM);

    vui_set_text(state_label_,  state_name(s));
    vui_set_color(state_label_, state_color(s));

    vui_set_text(metric_label_, metric.c_str());

    /* The running process is the scheduler — disallow killing it. */
    vui_set_visible(kill_button_, s != ProcessState::Running ? 1 : 0);

    vui_set_visible(row_vbox_,     1);
    vui_set_visible(name_label_,   1);
    vui_set_visible(state_label_,  1);
    vui_set_visible(metric_label_, 1);
}

void ProcessRow::hide() {
    pid_ = 0;
    /* Hide all child widgets; the row_vbox stays in layout (keeps its slot). */
    vui_set_visible(name_label_,   0);
    vui_set_visible(state_label_,  0);
    vui_set_visible(kill_button_,  0);
    vui_set_visible(metric_label_, 0);
}
