#include "task_manager.h"
#include "process_state.h"
#include "string_builder.h"
#include "layout.h"
#include <sys/syscall.h>
#include <vibeos.h>

TaskManager *TaskManager::s_instance_ = nullptr;

TaskManager::TaskManager() = default;

/* ---- Public ------------------------------------------------------------ */

void __attribute__((noreturn)) TaskManager::run() {
    uint32_t mode = vos_display_mode_get();
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
    /* ---- Metric cards: self-contained vui_metric widgets (big value + chart) */
    total_label_  = vui_metric(window_,  16, 14, 144, 84, "PROCESSES", 0);
    cpu_label_    = vui_metric(window_, 168, 14, 144, 84, "CPU",       0);  /* chart */
    mem_label_    = vui_metric(window_, 320, 14, 144, 84, "MEMORY",    1);  /* progress bar */
    status_label_ = vui_metric(window_, 472, 14, 152, 84, "UPTIME",    2);  /* value only */
    vui_set_color(total_label_,  VUI_ACCENT);
    vui_set_color(cpu_label_,    VUI_ACCENT);
    vui_set_color(mem_label_,    VUI_WARN);
    vui_set_color(status_label_, VUI_ACCENT);

    /* ---- Navigation/search strip ---------------------------------------- */
    auto *tabs = vui_tabs(window_, 16, 112, 400, "PROCESSES|SYSTEM|SERVICES|NETWORK", 0);
    vui_set_anchor(tabs, VUI_ANCHOR_LEFT | VUI_ANCHOR_TOP);

    search_ = vui_input(window_, 422, 113, 202, "Search processes...");
    vui_set_icon_svg_path(search_, "/icons/search.svg");
    vui_set_anchor(search_, VUI_ANCHOR_TOP | VUI_ANCHOR_RIGHT);
    vui_on_click(search_, on_search_cb);   /* re-filter as the user types */

    /* Process table: no bordered panel (the reference has none) — just a
     * header row, a thin underline, then the rows on the window surface. ---- */
    auto *headers = vui_hbox(window_, 30, 164, 580, 16);
    vui_set_anchor(headers, VUI_ANCHOR_LEFT | VUI_ANCHOR_TOP | VUI_ANCHOR_RIGHT);
    vui_set_gap(headers, Layout::kRowGap);
    vui_set_padding(headers, Layout::kRowPadding);

    /* Column headers — widths mirror the per-row columns in process_row.cpp
     * (NAME expands; USER 56, STATUS 76, CPU 52, MEM 80, ACTION 52). */
    auto *h_name = vui_label(window_, 0, 0, "PID  NAME");
    vui_set_expand(h_name);
    vui_box_add(headers, h_name);

    auto *h_user = vui_label(window_, 0, 0, "USER");
    vui_set_size(h_user, Layout::kUserWidth, 0);
    vui_box_add(headers, h_user);

    auto *h_state = vui_label(window_, 0, 0, "STATUS");
    vui_set_size(h_state, Layout::kStateWidth, 0);
    vui_box_add(headers, h_state);

    auto *h_cpu = vui_label(window_, 0, 0, "CPU");
    vui_set_size(h_cpu, Layout::kCpuWidth, 0);
    vui_box_add(headers, h_cpu);

    auto *h_mem = vui_label(window_, 0, 0, "MEM");
    vui_set_size(h_mem, Layout::kMemWidth, 0);
    vui_box_add(headers, h_mem);

    auto *h_action = vui_label(window_, 0, 0, "ACTION");
    vui_set_size(h_action, Layout::kKillWidth, 0);
    vui_box_add(headers, h_action);


    /* Process-rows VBox — fills the remaining panel area on all sides. */
    rows_vbox_ = vui_vbox(window_, 30, 188, 580, 210);
    vui_set_anchor(rows_vbox_,
                   VUI_ANCHOR_LEFT | VUI_ANCHOR_TOP | VUI_ANCHOR_RIGHT | VUI_ANCHOR_BOTTOM);
    vui_set_gap(rows_vbox_, 0);
    vui_set_padding(rows_vbox_, 0);

    /* Each ProcessRow adds its own row_vbox into rows_vbox_. */
    /* Create a fixed number of generously-sized rows; active processes are
     * packed into them top-down each refresh (see refresh()). */
    for (int i = 0; i < kRows; ++i)
        rows_[i].init(window_, rows_vbox_, i, on_kill_cb);
}

/* ---- Private — data refresh -------------------------------------------- */

/* Case-insensitive substring match (does `name` contain `q`?). */
static bool name_matches(const char *name, const char *q) {
    auto lc = [](char c){ return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; };
    for (int i = 0; name[i]; ++i) {
        int j = 0;
        while (q[j] && name[i + j] && lc(name[i + j]) == lc(q[j])) ++j;
        if (!q[j]) return true;
    }
    return q[0] == '\0';
}

void TaskManager::refresh() {
    unsigned int  loaded = 0, ready = 0, sleeping = 0, total_threads = 0;
    unsigned long total_mem = 0;
    unsigned long total_cpu_tenths = 0;
    int row = 0;   /* pack active processes into the top rows (no gaps) */

    struct vos_system_info info{};
    vos_system_info(&info);

    const char *query = search_ ? vui_input_text(search_) : "";

    /* Time elapsed since the last refresh (in ticks).  Used as the denominator
     * for delta-CPU so we show current utilisation, not a lifetime average. */
    unsigned long delta_uptime = (info.uptime_ticks > prev_uptime_)
                                 ? (info.uptime_ticks - prev_uptime_) : 1;

    for (int slot = 0; slot < VUI_PROCESS_MAX; ++slot) {
        vui_process_info p{};
        auto s = ProcessState::Empty;

        if (vui_process_snapshot(static_cast<unsigned int>(slot), &p) > 0)
            s = static_cast<ProcessState>(p.state);

        if (!p.loaded || !state_is_active(s)) {
            prev_runtime_[slot] = 0;
            continue;
        }

        /* Worker threads are scheduler entities sharing their owner's address
         * space — they are not separate apps. Surface them only via the owner's
         * thread_count, never as their own row (and don't double-count their
         * shared memory). */
        if (p.is_thread) {
            prev_runtime_[slot] = 0;
            continue;
        }

        /* Cards/counts reflect ALL processes. */
        ++loaded;
        if (s == ProcessState::Ready)    ++ready;
        if (s == ProcessState::Sleeping) ++sleeping;
        total_mem     += p.mem_bytes;
        total_threads += p.thread_count;

        /* Delta-CPU: fraction of wall-clock ticks since last refresh that this
         * process ran.  Multiply by 1000 for one decimal place (tenths of %).
         * Falls back to lifetime average on the first refresh when prev==0. */
        unsigned long rt_now  = p.runtime_ticks;
        unsigned long rt_prev = prev_runtime_[slot];
        unsigned long delta_rt = (rt_now >= rt_prev) ? (rt_now - rt_prev) : rt_now;
        int cpu_tenths = (int)((unsigned long long)delta_rt * 1000ULL / delta_uptime);
        if (cpu_tenths > 1000) cpu_tenths = 1000; /* clamp to 100.0% */
        prev_runtime_[slot] = rt_now;

        /* The search box only filters which ROWS are shown. */
        if (query[0] && !name_matches(p.name, query)) continue;
        if (row < kRows)
            rows_[row++].update(p, cpu_tenths);
        total_cpu_tenths += cpu_tenths;
    }
    prev_uptime_ = info.uptime_ticks;
    for (int r = row; r < kRows; ++r) rows_[r].hide();

    unsigned int pmax = info.process_max ? info.process_max
                                         : static_cast<unsigned int>(VUI_PROCESS_MAX);
    /* PROCESSES: count + "of N", chart = utilisation of process slots. */
    {
        StringBuilder<16> v; v.append(loaded);
        StringBuilder<24> s; s.append("of ").append(pmax);
        vui_set_metric(total_label_, v.c_str(), s.c_str());
        vui_metric_push(total_label_, pmax ? (int)(loaded * 100u / pmax) : 0);
    }
    /* CPU: total utilisation %. */
    {
        unsigned int cpu_pct = (unsigned int)(total_cpu_tenths / 10);
        StringBuilder<16> v;
        v.append(cpu_pct);
        v.append("%");
        vui_set_metric(cpu_label_, v.c_str(), "utilisation");
        vui_metric_push(cpu_label_, (int)cpu_pct);
    }
    /* MEMORY: heap used %, used / total, with a progress bar. */
    {
        int pct = info.heap_total_bytes
                  ? (int)((info.heap_used_bytes * 100ULL) / info.heap_total_bytes) : 0;
        StringBuilder<16> v; v.append(pct).append("%");
        StringBuilder<32> s; s.append_size(info.heap_used_bytes).append(" / ")
                              .append_size(info.heap_total_bytes);
        vui_set_metric(mem_label_, v.c_str(), s.c_str());
        vui_set_value(mem_label_, pct);
    }
    /* THREADS: total threads across processes (chart scaled to ~2x slots). */
    {
        StringBuilder<16> v; v.append(total_threads);
        vui_set_metric(ready_label_, v.c_str(), "active");
        unsigned int cap = pmax ? pmax * 2u : 16u;
        vui_metric_push(ready_label_, (int)(total_threads * 100u / cap));
    }
    /* UPTIME: hh:mm:ss + version. */
    {
        unsigned long secs = info.timer_hz ? (unsigned long)(info.uptime_ticks / info.timer_hz) : 0;
        unsigned long hh = secs / 3600, mm = (secs / 60) % 60, ss = secs % 60;
        char t[12];
        t[0]='0'+(char)(hh/10%10); t[1]='0'+(char)(hh%10); t[2]=':';
        t[3]='0'+(char)(mm/10);    t[4]='0'+(char)(mm%10);  t[5]=':';
        t[6]='0'+(char)(ss/10);    t[7]='0'+(char)(ss%10);  t[8]='\0';
        vui_set_metric(status_label_, t, info.version[0] ? info.version : "VibeOS");
    }
    (void)ready; (void)sleeping; (void)total_mem;
}

void TaskManager::set_status(const char *text, vui_u32 color) {
    vui_set_text(status_label_, text);
    vui_set_color(status_label_, color);
}

/* ---- Static callbacks -------------------------------------------------- */

void TaskManager::on_kill_cb(vui_widget *self) {
    /* Recover the PID stored in the button's user pointer. */
    unsigned int pid = static_cast<unsigned int>(
        reinterpret_cast<unsigned long>(vui_get_user(self)));
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

void TaskManager::on_search_cb(vui_widget *) {
    s_instance_->refresh();   /* re-filter the row list as the user types */
}

void TaskManager::on_quit_cb(vui_widget *) {
    vui_quit(s_instance_->window_);
}

void TaskManager::on_tick_cb(vui_window *) {
    /* Auto-refresh approximately once per second (VexUI ticks at ~18 Hz). */
    if ((++s_instance_->tick_ % 18) == 0)
        s_instance_->refresh();
}
