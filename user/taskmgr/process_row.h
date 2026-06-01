#pragma once

#include "../vexui.h"

/* Owns all widgets that make up one slot in the process table.
 *
 * Visual structure (nested VexUI boxes):
 *
 *   row_vbox  (VBox, height shared equally in the outer rows_vbox)
 *   ├── top_hbox  (HBox, h=18)
 *   │   ├── name_label    — "PID n  process-name"  (expands to fill width)
 *   │   ├── state_label   — "RUNNING" / "SLEEP" …  (fixed width)
 *   │   └── kill_button   — "KILL"                  (fixed width)
 *   └── metric_label      — "ticks n  switches n  parent n"  (fills width)
 *
 * init() creates all widgets and adds them to the parent rows_vbox.
 * update() / hide() are called every refresh cycle from TaskManager. */
class ProcessRow {
public:
    ProcessRow() = default;

    /* Create all widgets.
     *   win       — the application window
     *   rows_vbox — the outer VBox that owns all process rows
     *   slot      — row index stored in the kill-button user pointer
     *   kill_cb   — click handler registered on the kill button */
    void init(vui_window *win, vui_widget *rows_vbox, int slot,
              void (*kill_cb)(vui_widget *));

    /* Populate widgets from a live process snapshot and make the row visible. */
    void update(const vui_process_info &p);

    /* Hide all widgets (row stays in layout but renders nothing). */
    void hide();

    unsigned int pid() const { return pid_; }

private:
    vui_widget  *row_vbox_    = nullptr;
    vui_widget  *name_label_  = nullptr;
    vui_widget  *state_label_ = nullptr;
    vui_widget  *kill_button_ = nullptr;
    vui_widget  *metric_label_= nullptr;
    unsigned int pid_         = 0;
};
