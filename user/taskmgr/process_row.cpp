#include "process_row.h"
#include "process_state.h"
#include "string_builder.h"
#include "layout.h"
#include <stdio.h>
#include <string.h>

/* Widget geometry constants — keep them in one place so the whole row
 * can be adjusted without hunting through the code. */
static constexpr int kTopRowHeight  = Layout::kRowHeight;
/* Columns: NAME(expand) | USER | STATUS | CPU | MEM | KILL — widths mirror the
 * header HBox in task_manager.cpp::build_ui. */
static constexpr int kUserWidth     = Layout::kUserWidth;
static constexpr int kStateWidth    = Layout::kStateWidth;
static constexpr int kCpuWidth      = Layout::kCpuWidth;
static constexpr int kMemWidth      = Layout::kMemWidth;
static constexpr int kKillWidth     = Layout::kKillWidth;
static constexpr int kRowPadding    = Layout::kRowPadding;
static constexpr int kRowGap        = Layout::kRowGap;

/* RAM colour thresholds (bytes): green under 8 MB, amber under 20 MB, red above.
 * The browser's static umalloc heap pushes it into the red, which is useful
 * signal rather than alarm. */
static vui_u32 ram_color(unsigned long bytes) {
    if (bytes < 8UL * 1024 * 1024)  return VUI_OK;
    if (bytes < 20UL * 1024 * 1024) return VUI_WARN;
    return VUI_DANGER;
}

/* Resolve a numeric UID to a username by parsing /etc/passwd.
 * Returns a pointer to a static buffer (the last match is reused).
 * Falls back to "root" for uid 0, otherwise the uid as a decimal string. */
static const char *uid_to_name(unsigned int uid) {
    static char result[16];
    static char line_buf[128];
    static bool loaded = false;
    static char passwd_cache[1024];
    static size_t cache_len = 0;

    /* Read /etc/passwd once. */
    if (!loaded) {
        loaded = true;
        FILE *fp = fopen("/etc/passwd", "r");
        if (fp) {
            cache_len = fread(passwd_cache, 1, sizeof(passwd_cache) - 1, fp);
            fclose(fp);
            if (cache_len > 0)
                passwd_cache[cache_len] = '\0';
        }
    }

    if (cache_len > 0) {
        const char *p = passwd_cache;
        while (*p) {
            /* Copy one line. */
            size_t li = 0;
            while (*p && *p != '\n' && li < sizeof(line_buf) - 1)
                line_buf[li++] = *p++;
            line_buf[li] = '\0';
            if (*p == '\n') p++;

            /* Extract username and uid fields.
             * Format: name:x:uid:... */
            const char *line = line_buf;
            /* field 0: name */
            const char *name_start = line;
            const char *name_end   = line;
            while (*name_end && *name_end != ':') name_end++;
            if (*name_end != ':') continue;

            /* skip to field 2 (uid) */
            const char *f = name_end + 1;  /* skip colon, now field 1 (x) */
            while (*f && *f != ':') f++;
            if (*f == ':') f++;            /* now field 2 (uid) */
            unsigned int entry_uid = 0;
            while (*f >= '0' && *f <= '9') {
                entry_uid = entry_uid * 10 + (unsigned int)(*f - '0');
                f++;
            }

            if (entry_uid == uid) {
                size_t nlen = (size_t)(name_end - name_start);
                if (nlen >= sizeof(result)) nlen = sizeof(result) - 1;
                for (size_t i = 0; i < nlen; i++) result[i] = name_start[i];
                result[nlen] = '\0';
                return result;
            }
        }
    }

    /* Fallback. */
    if (uid == 0) return "root";
    int len = 0;
    unsigned int n = uid;
    if (n == 0) {
        result[len++] = '0';
    } else {
        char tmp[12];
        int ti = 0;
        while (n) { tmp[ti++] = (char)('0' + n % 10); n /= 10; }
        while (ti > 0) result[len++] = tmp[--ti];
    }
    result[len] = '\0';
    return result;
}

void ProcessRow::init(vui_window *win, vui_widget *rows_vbox, int slot,
                      void (*kill_cb)(vui_widget *)) {
    /* ---- Outer row: compact table row ------------------------------------ */
    /* Transparent row (no fill, no zebra) — rows are separated only by a single
     * light hairline at the bottom (per the reference: one separator, no side
     * or dark borders). */
    (void)slot;
    row_vbox_ = vui_vbox(win, 0, 0, 0, 0);
    /* color left at the default sentinel => W_VBOX draws no fill. */
    vui_set_padding(row_vbox_, kRowPadding);
    vui_set_gap(row_vbox_, 0);
    vui_set_expand(row_vbox_); /* each row shares the outer VBox's height evenly */
    vui_set_fill(row_vbox_);   /* stretch to the full width of rows_vbox         */
    vui_box_add(rows_vbox, row_vbox_);

    /* ---- Columns: NAME | USER | STATUS | CPU | MEM | KILL ---------------- */
    auto *top_hbox = vui_hbox(win, 0, 0, 0, kTopRowHeight);
    vui_set_gap(top_hbox, kRowGap);
    vui_set_expand(top_hbox); /* take the full row height (avoids fixed-h overflow) */
    vui_set_fill(top_hbox);   /* fill the row_vbox's width */
    vui_box_add(row_vbox_, top_hbox);

    /* NAME (PID + name), expands to push the fixed columns right. */
    name_label_ = vui_label(win, 0, 0, "");
    vui_set_expand(name_label_);
    vui_set_fill(name_label_);
    vui_box_add(top_hbox, name_label_);

    /* USER (repurposed thread_label_). */
    thread_label_ = vui_label(win, 0, 0, "");
    vui_set_size(thread_label_, kUserWidth, 0);
    vui_box_add(top_hbox, thread_label_);

    /* STATUS as coloured text (per the reference), not a filled badge. */
    state_label_ = vui_label(win, 0, 0, "");
    vui_set_size(state_label_, kStateWidth, 0);
    vui_box_add(top_hbox, state_label_);

    /* CPU % (repurposed metric_label_). */
    metric_label_ = vui_label(win, 0, 0, "");
    vui_set_size(metric_label_, kCpuWidth, 0);
    vui_box_add(top_hbox, metric_label_);

    /* MEM (repurposed ram_label_). */
    ram_label_ = vui_label(win, 0, 0, "");
    vui_set_size(ram_label_, kMemWidth, 0);
    vui_box_add(top_hbox, ram_label_);

    /* ACTION: a compact danger button (subtle rounded rect via the global
     * radius, not a full pill). */
    kill_button_ = vui_button(win, 0, 0, "KILL");
    vui_set_size(kill_button_, kKillWidth, 22);
    vui_set_color(kill_button_, VUI_DANGER);
    vui_set_user(kill_button_, reinterpret_cast<void *>(static_cast<unsigned long>(slot)));
    vui_on_click(kill_button_, kill_cb);
    vui_box_add(top_hbox, kill_button_);

    /* Single light hairline at the bottom of the row (the only row separator). */
    sep_ = vui_hbox(win, 0, 0, 0, 1);
    vui_set_color(sep_, 0x00263c56u);   /* faint light line on the dark bg */
    vui_set_fill(sep_);
    vui_box_add(row_vbox_, sep_);

    pid_ = 0;
    /* Start hidden — update() makes a row visible when it gets a real process. */
    vui_set_visible(row_vbox_,     0);
    vui_set_visible(name_label_,   0);
    vui_set_visible(thread_label_, 0);
    vui_set_visible(state_label_,  0);
    vui_set_visible(metric_label_, 0);
    vui_set_visible(ram_label_,    0);
    vui_set_visible(kill_button_,  0);
    vui_set_visible(sep_,          0);
}

void ProcessRow::update(const vui_process_info &p, int cpu_tenths) {
    auto s = static_cast<ProcessState>(p.state);
    pid_ = p.pid;

    StringBuilder<80> name;
    name.append(p.pid).append("  ").append(p.name[0] ? p.name : "process");

    StringBuilder<24> mem;  mem.append_size(p.mem_bytes);

    StringBuilder<12> cpu;
    if (cpu_tenths < 0) cpu_tenths = 0;
    cpu.append(static_cast<unsigned int>(cpu_tenths / 10)).append(".")
       .append(static_cast<unsigned int>(cpu_tenths % 10)).append("%");

    vui_set_text(name_label_,   name.c_str());
    vui_set_color(name_label_,  s == ProcessState::Running ? VUI_TEXT : VUI_TEXT_DIM);

    vui_set_text(thread_label_, uid_to_name(p.uid));
    vui_set_color(thread_label_, VUI_TEXT_DIM);

    vui_set_text(state_label_,  state_name(s));
    vui_set_color(state_label_, state_color(s));

    vui_set_text(metric_label_, cpu.c_str());
    vui_set_color(metric_label_, VUI_TEXT_DIM);

    vui_set_text(ram_label_,    mem.c_str());
    vui_set_color(ram_label_,   ram_color(p.mem_bytes));

    /* The running process is the scheduler — disallow killing it. */
    vui_set_visible(kill_button_, s != ProcessState::Running ? 1 : 0);
    vui_set_user(kill_button_, reinterpret_cast<void *>(static_cast<unsigned long>(p.pid)));

    vui_set_visible(row_vbox_,     1);
    vui_set_visible(name_label_,   1);
    vui_set_visible(thread_label_, 1);
    vui_set_visible(state_label_,  1);
    vui_set_visible(metric_label_, 1);
    vui_set_visible(ram_label_,    1);
    vui_set_visible(sep_,          1);
}

void ProcessRow::hide() {
    pid_ = 0;
    /* Hide the whole row (incl. its background) so empty slots leave clean space. */
    vui_set_visible(row_vbox_,     0);
    vui_set_visible(name_label_,   0);
    vui_set_visible(ram_label_,    0);
    vui_set_visible(thread_label_, 0);
    vui_set_visible(state_label_,  0);
    vui_set_visible(kill_button_,  0);
    vui_set_visible(metric_label_, 0);
    vui_set_visible(sep_,          0);
}
