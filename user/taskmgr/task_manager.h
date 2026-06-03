#pragma once

#include <array>
#include "../vexui.h"
#include "process_row.h"

/* Root application object.  Owns the window, the header widgets, and the
 * fixed-size array of ProcessRows.
 *
 * VexUI callback functions must have C linkage (function-pointer type), so
 * they are implemented as static members.  A single static instance pointer
 * (s_instance_) bridges the gap between the C callback and the object.
 * This is safe because there is exactly one TaskManager per process. */
class TaskManager {
public:
    TaskManager();

    /* Build the UI, start the event loop, and never return. */
    void __attribute__((noreturn)) run();

    /* Singleton pointer — must be set before vui_run() is called. */
    static TaskManager *s_instance_;

private:
    /* Number of process rows shown (active processes are packed top-down into
     * these; kept small so each row is generously tall, like the reference). */
    static constexpr int kRows = 8;

    /* --- UI construction ------------------------------------------------- */
    void build_ui();

    /* --- Data refresh ---------------------------------------------------- */
    /* Snapshot all process slots from the kernel and update every row. */
    void refresh();

    /* Update the status label in the top summary panel. */
    void set_status(const char *text, vui_u32 color);

    /* --- Static VexUI callbacks ------------------------------------------ */
    /* Called when the user clicks a KILL button. */
    static void on_kill_cb(vui_widget *self);

    /* Called when the user clicks the Refresh button or menu item. */
    static void on_refresh_cb(vui_widget *self);

    /* Called from View → Quit. */
    static void on_quit_cb(vui_widget *self);

    /* Called as the user types in the search box → re-filter the process list. */
    static void on_search_cb(vui_widget *self);

    /* Called by the VexUI event loop roughly 18 times per second.
     * Triggers a data refresh once every 18 ticks (~1 s). */
    static void on_tick_cb(vui_window *win);

    /* --- Members --------------------------------------------------------- */
    vui_window *window_       = nullptr;
    vui_widget *total_label_   = nullptr; /* "PROC n / 4"          */
    vui_widget *cpu_label_     = nullptr; /* "CPU n%"              */
    vui_widget *ready_label_   = nullptr; /* "READY n   SLEEP n"  */
    vui_widget *mem_label_     = nullptr; /* "RAM x.x MB  n thr"  */
    vui_widget *status_label_  = nullptr; /* last action feedback  */
    vui_widget *search_        = nullptr; /* process-name filter box */

    std::array<ProcessRow, VUI_PROCESS_MAX> rows_;
    vui_widget *rows_vbox_ = nullptr; /* VBox that owns all ProcessRow slots */

    int tick_ = 0; /* incremented each VexUI tick */

    /* Per-slot runtime snapshot from the previous refresh cycle.  Used to
     * compute delta-CPU (current utilisation) instead of lifetime fraction. */
    unsigned long prev_runtime_[VUI_PROCESS_MAX] = {};
    unsigned long prev_uptime_ = 0; /* uptime_ticks at the last refresh */
};
