/* terminal.cpp — VibeOS terminal emulator (vexui + pty + /bin/sh).
 *
 * Architecture
 * ============
 * A thin, single-threaded terminal emulator:
 *
 *   keystrokes ─▶ line editor ─▶ pty master write ─▶ /bin/sh stdin
 *   /bin/sh stdout ─▶ pty master read ─▶ TextGrid ─▶ canvas ─▶ window
 *
 * The kernel pty is a raw byte pipe; this app owns the line discipline (local
 * echo, backspace) and renders the screen with the kernel TrueType atlas via
 * TextGrid. The shell polls its stdin, so no blocking I/O or worker thread is
 * needed: we drain the master fd each UI tick and repaint when something
 * changed.
 */
#include "vexui.h"
#include "text_grid.h"
#include <string.h>

#include <vibeos.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>

/* Fixed canvas stride (matches vexui's VUI_MAX_W) so the backing buffer's row
 * pitch never depends on the live window width — the same trick the browser
 * uses to stay correct across resizes. */
static constexpr int CANVAS_STRIDE = 900;
static constexpr int CANVAS_MAX_H  = 640;

class Terminal {
public:
    void run();

    /* vexui callbacks (routed through the global instance). */
    void on_key(unsigned int key);
    void on_scroll(int delta);
    void on_resize(int w, int h);
    void on_tick();

    static Terminal *instance;

private:
    void send_line();
    void echo(char c);
    void pump_output();
    void repaint();

    static constexpr int MAX_HISTORY = 32;
    char history_[MAX_HISTORY][256];
    int history_count_ = 0;
    int history_idx_ = -1;

    TextGrid grid_;
    uint32_t canvas_[CANVAS_STRIDE * CANVAS_MAX_H];

    vui_window *win_   = nullptr;
    vui_widget *canvas_widget_ = nullptr;

    int  master_fd_ = -1;
    int  shell_pid_ = -1;

    char line_[256];
    int  line_len_    = 0;
    int  cursor_pos_  = 0;       /* cursor position within line_ */

    bool dirty_ = true;
    int  blink_counter_ = 0;
    int  raw_ticks_ = 0;      /* poll raw_mode every 8 ticks */
};

Terminal *Terminal::instance = nullptr;

/* ---- global callback trampolines -------------------------------------- */

static void on_key_cb(vui_window *, unsigned int key) { Terminal::instance->on_key(key); }
static void on_scroll_cb(vui_window *, int delta)     { Terminal::instance->on_scroll(delta); }
static void on_resize_cb(vui_window *, int w, int h)  { Terminal::instance->on_resize(w, h); }
static void on_tick_cb(vui_window *)                  { Terminal::instance->on_tick(); }

/* ---- line editing ------------------------------------------------------ */

void Terminal::echo(char c) {
    grid_.put(c);
    dirty_ = true;
}

void Terminal::send_line() {
    /* Terminate the line with newline and hand it to the shell.
     * Newline termination matches TTY behaviour; NUL termination
     * breaks programs (su, etc.) that read char-by-char until '\n'. */
    if (line_len_ < (int)sizeof(line_) - 1) {
        line_[line_len_++] = '\n';
    }
    
    /* Save to history */
    if (line_len_ > 1) { // Only if not empty
        if (history_count_ < MAX_HISTORY) {
            strncpy(history_[history_count_++], line_, 256);
        } else {
            // Shift history
            for (int i = 0; i < MAX_HISTORY - 1; i++) strncpy(history_[i], history_[i+1], 256);
            strncpy(history_[MAX_HISTORY - 1], line_, 256);
        }
    }
    history_idx_ = -1;

    echo('\n');
    if (master_fd_ >= 0) {
        write(master_fd_, line_, (size_t)line_len_);
    }
    line_len_ = 0;
    cursor_pos_ = 0;
}

void Terminal::on_key(unsigned int key) {
    static int escape_state = 0; // 0=none, 1=ESC, 2=ESC+[

    /* If the PTY is in raw mode (e.g. nano running), forward every
     * keystroke directly to the process without line buffering. */
    if (master_fd_ >= 0) {
        uint32_t raw = 0;
        ioctl(master_fd_, TTY_IOCTL_GET_RAW, &raw);
        if (raw) {
            /* Build the byte(s) to send */
            char buf[8]; int n = 0;

            if (key == 0x03) {           /* Ctrl+C → interrupt */
                vos_pty_interrupt(master_fd_);
                return;
            }
            if (key == '\n' || key == '\r') {
                buf[n++] = '\r';
            } else if (key == 0x08 || key == 0x7f) {
                buf[n++] = 0x7f;          /* DEL / Backspace */
            } else if (key == 0x1b) {
                buf[n++] = 0x1b;          /* ESC */
            } else if (key == 0x09) {
                buf[n++] = '\t';
            } else if (key >= 0x20 && key < 0x7f) {
                buf[n++] = (char)key;
            } else {
                /* Arrow keys → ANSI escape sequences */
                switch (key) {
                    case 0x106: /* Up    */ buf[0]=0x1b; buf[1]='['; buf[2]='A'; n=3; break;
                    case 0x107: /* Down  */ buf[0]=0x1b; buf[1]='['; buf[2]='B'; n=3; break;
                    case 0x108: /* Right */ buf[0]=0x1b; buf[1]='['; buf[2]='C'; n=3; break;
                    case 0x109: /* Left  */ buf[0]=0x1b; buf[1]='['; buf[2]='D'; n=3; break;
                    default: return; /* unknown key, skip */
                }
            }
            if (n > 0) write(master_fd_, buf, (size_t)n);
            return;
        }
    }

    // Simple state machine for ANSI escape codes (ESC + [ + code)
    if (escape_state == 0 && key == 0x1b) {
        escape_state = 1;
        return;
    } else if (escape_state == 1 && key == '[') {
        escape_state = 2;
        return;
    } else if (escape_state == 2) {
        escape_state = 0;
        if (key == 'A') { // Up
            if (history_idx_ < history_count_ - 1) {
                history_idx_++;
                while(line_len_ > 0) { echo('\b'); line_len_--; }
                strncpy(line_, history_[history_count_ - 1 - history_idx_], 256);
                line_len_ = strlen(line_);
                for(int i = 0; i < line_len_; i++) echo(line_[i]);
                cursor_pos_ = line_len_;  /* cursor at end after recall */
            }
            return;
        } else if (key == 'B') { // Down
            if (history_idx_ > 0) {
                history_idx_--;
                while(line_len_ > 0) { echo('\b'); line_len_--; }
                strncpy(line_, history_[history_count_ - 1 - history_idx_], 256);
                line_len_ = strlen(line_);
                for(int i = 0; i < line_len_; i++) echo(line_[i]);
                cursor_pos_ = line_len_;
            } else if (history_idx_ == 0) {
                history_idx_ = -1;
                while(line_len_ > 0) { echo('\b'); line_len_--; }
                line_len_ = 0;
                cursor_pos_ = 0;
            }
            return;
        } else if (key == 'C') { // Right
            if (cursor_pos_ < line_len_) {
                echo(line_[cursor_pos_++]);
            }
            return;
        } else if (key == 'D') { // Left
            if (cursor_pos_ > 0) {
                cursor_pos_--;
                echo('\b');
            }
            return;
        }
        return; // Other escape sequences ignored for now
    }
    escape_state = 0;
    
    if (key == '\n' || key == '\r') {
        send_line();
        cursor_pos_ = 0;
    } else if (key == 0x08 || key == 0x7f) {   /* backspace */
        if (cursor_pos_ > 0) {
            /* Shift everything after cursor left by 1 */
            for (int i = cursor_pos_; i < line_len_; i++)
                line_[i - 1] = line_[i];
            line_len_--;
            cursor_pos_--;
            /* Redraw from cursor to end, then clear trailing cell */
            for (int i = cursor_pos_; i < line_len_; i++)
                echo(line_[i]);
            echo(' ');  /* clear the last cell */
            /* Move cursor back to correct position */
            for (int i = cursor_pos_; i <= line_len_; i++)
                echo('\b');
        }
    } else if (key >= 0x20 && key < 0x7f) {     /* printable ASCII */
        if (line_len_ < (int)sizeof(line_) - 2) {
            /* Shift everything from cursor right by 1 */
            for (int i = line_len_; i > cursor_pos_; i--)
                line_[i] = line_[i - 1];
            line_[cursor_pos_] = (char)key;
            line_len_++;
            /* Redraw from cursor to end */
            for (int i = cursor_pos_; i < line_len_; i++)
                echo(line_[i]);
            cursor_pos_++;
            /* Move cursor back to correct position */
            for (int i = cursor_pos_; i < line_len_; i++)
                echo('\b');
        }
    }
    if (dirty_) repaint();
}

void Terminal::on_scroll(int delta) {
    /* Three lines per wheel notch; positive notch scrolls toward older output. */
    grid_.scroll_view(delta * 3);
    dirty_ = true;
    repaint();
}

void Terminal::on_resize(int w, int h) {
    /* Re-flow the grid to the new content size (clamped to the canvas buffer). */
    int cols = w / grid_.cell_w();
    int rows = h / grid_.cell_h();
    if (cols * grid_.cell_w() > CANVAS_STRIDE) cols = CANVAS_STRIDE / grid_.cell_w();
    if (rows * grid_.cell_h() > CANVAS_MAX_H)  rows = CANVAS_MAX_H / grid_.cell_h();
    grid_.resize(cols, rows);
    dirty_ = true;
    repaint();
}

/* ---- shell output ------------------------------------------------------ */

void Terminal::pump_output() {
    if (master_fd_ < 0) return;
    char buf[1024];
    ssize_t n;
    /* Drain everything buffered this tick (master reads are non-blocking). */
    while ((n = read(master_fd_, buf, sizeof(buf))) > 0) {
        grid_.write(buf, (int)n);
        dirty_ = true;
    }
}

void Terminal::on_tick() {
    pump_output();
    if (dirty_) {
        repaint();
        return;
    }
    /* Force periodic repaints so the cursor blinks even when idle. */
    if ((++blink_counter_ % 8) == 0) {
        repaint();
    }
}

void Terminal::repaint() {
    grid_.render(canvas_, CANVAS_STRIDE, grid_.pixel_height());
    vui_request_repaint(win_);
    dirty_ = false;
}

/* ---- setup ------------------------------------------------------------- */

void Terminal::run() {
    instance = this;

    grid_.load_metrics(1);

    /* Choose a grid that fits within the canvas limits. */
    int cols = 80, rows = 24;
    if (cols * grid_.cell_w() > CANVAS_STRIDE - grid_.cell_w())
        cols = (CANVAS_STRIDE - grid_.cell_w()) / grid_.cell_w();
    if (rows * grid_.cell_h() > CANVAS_MAX_H)
        rows = CANVAS_MAX_H / grid_.cell_h();
    grid_.resize(cols, rows);
    grid_.clear();

    int win_w = grid_.pixel_width();
    int win_h = grid_.pixel_height();

    win_ = vui_window_open("Terminal", win_w, win_h);
    if (!win_) exit(1);

    canvas_widget_ = vui_canvas_ex(win_, 0, 0, win_w, win_h, canvas_, CANVAS_STRIDE);
    vui_set_anchor(canvas_widget_,
                   VUI_ANCHOR_LEFT | VUI_ANCHOR_RIGHT | VUI_ANCHOR_TOP | VUI_ANCHOR_BOTTOM);

    vui_on_key(win_, on_key_cb);
    vui_on_scroll(win_, on_scroll_cb);
    vui_on_resize(win_, on_resize_cb);
    vui_on_tick(win_, on_tick_cb);

    /* Open a pty and run the shell on its slave end. */
    master_fd_ = vos_pty_open();
    if (master_fd_ < 0) {
        grid_.write("terminal: cannot open pty\n", 26);
    } else {
        shell_pid_ = vos_spawn_pty("/bin/sh", master_fd_);
        if (shell_pid_ < 0) {
            grid_.write("terminal: cannot start /bin/sh\n", 31);
        }
    }

    repaint();
    vui_run(win_);
}

int main() {
    static Terminal terminal;
    terminal.run();
    return 0;
}
