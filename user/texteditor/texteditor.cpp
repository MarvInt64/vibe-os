/*
 * texteditor/texteditor.cpp - Implementation of the VibeOS GUI Text Editor.
 *
 * Features: mouse + keyboard selection, copy/paste, file I/O.
 */

#include "texteditor.h"
#include <vibeos.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* --------------------------------------------------------------------------- */
/*  Palette & layout constants                                                 */
/* --------------------------------------------------------------------------- */
static constexpr uint32_t ED_BG              = 0x00161d2bu;
static constexpr uint32_t ED_BG_SELECTION    = 0x00264f78u;
static constexpr uint32_t ED_FG                = 0x00e6edf7u;
static constexpr uint32_t ED_FG_DIM            = 0x009aa7bau;
static constexpr uint32_t ED_CURSOR            = 0x008f7bf0u;
static constexpr uint32_t ED_LINE_NUMBER_FG   = 0x00567396u;
static constexpr uint32_t ED_LINE_NUMBER_BG   = 0x00141e2bu;
static constexpr uint32_t ED_BORDER            = 0x00284058u;
static constexpr uint32_t ED_STATUS_BG         = 0x001f2942u;

static constexpr int ED_GUTTER_WIDTH    = 48;
static constexpr int ED_TOP_MARGIN       = 28;
static constexpr int ED_BOTTOM_MARGIN    = 20;

/* --------------------------------------------------------------------------- */
/*  Helper: text metrics (kernel syscall wrapper)                              */
/* --------------------------------------------------------------------------- */
static int get_text_width(const char *s, int scale) {
    if (!s || !*s) return 0;
    if (scale < 1) scale = 1;
    if (scale > 3) scale = 3;
    return (int)__sc2(SYS_TEXT_METRICS, (uint64_t)(size_t)s, (uint64_t)scale);
}

/* --------------------------------------------------------------------------- */
/*  Global callback trampolines (routed to the singleton instance)           */
/* --------------------------------------------------------------------------- */
static void ed_on_key(vui_window *, unsigned int key)       { TextEditor::instance()->on_key(key);          }
static void ed_on_click(vui_window *, int x, int y)         { TextEditor::instance()->on_click(x, y);       }
static void ed_on_mm(vui_window *, int x, int y)            { TextEditor::instance()->on_mouse_move(x, y);  }
static void ed_on_mr(vui_window *, int x, int y)            { TextEditor::instance()->on_mouse_release(x, y); }
static void ed_on_scroll(vui_window *, int delta)           { TextEditor::instance()->on_scroll(delta);     }
static void ed_on_resize(vui_window *, int w, int h)         { TextEditor::instance()->on_resize(w, h);       }
static void ed_on_tick(vui_window *)                         { TextEditor::instance()->on_tick();             }

/* --------------------------------------------------------------------------- */
/*  Core: text buffer management                                                */
/* --------------------------------------------------------------------------- */

void TextEditor::insert_char(char c) {
    if (num_lines_ == 0) { line_buffer_[0].len = 0; num_lines_ = 1; }
    if (cursor_y_ >= num_lines_) cursor_y_ = num_lines_ - 1;
    if (cursor_y_ < 0) cursor_y_ = 0;

    Line &ln = line_buffer_[cursor_y_];
    if (ln.len >= ED_MAX_LINE_LEN) return;
    if (cursor_x_ > ln.len) cursor_x_ = ln.len;

    for (int i = ln.len; i > cursor_x_; --i) ln.text[i] = ln.text[i - 1];
    ln.text[cursor_x_] = c;
    ln.len++;
    cursor_x_++;
    modified_ = true;
    ensure_cursor_visible();
    request_repaint();
}

void TextEditor::delete_backward() {
    if (has_selection()) { delete_selection(); ensure_cursor_visible(); request_repaint(); return; }
    if (cursor_x_ > 0) {
        Line &ln = line_buffer_[cursor_y_];
        for (int i = cursor_x_ - 1; i < ln.len - 1; ++i) ln.text[i] = ln.text[i + 1];
        ln.len--;
        cursor_x_--;
        modified_ = true;
    } else if (cursor_y_ > 0) {
        Line &prev = line_buffer_[cursor_y_ - 1];
        Line &curr = line_buffer_[cursor_y_];
        int space = ED_MAX_LINE_LEN - prev.len;
        if (space > curr.len) space = curr.len;
        for (int i = 0; i < space; ++i) prev.text[prev.len + i] = curr.text[i];
        prev.len += space;
        for (int i = cursor_y_; i < num_lines_ - 1; ++i) line_buffer_[i] = line_buffer_[i + 1];
        num_lines_--;
        cursor_y_--;
        cursor_x_ = prev.len;
        modified_ = true;
    }
    ensure_cursor_visible();
    request_repaint();
}

void TextEditor::delete_forward() {
    if (has_selection()) { delete_selection(); ensure_cursor_visible(); request_repaint(); return; }
    if (cursor_y_ >= num_lines_) return;
    Line &ln = line_buffer_[cursor_y_];
    if (cursor_x_ < ln.len) {
        for (int i = cursor_x_; i < ln.len - 1; ++i) ln.text[i] = ln.text[i + 1];
        ln.len--;
        modified_ = true;
    } else if (cursor_y_ < num_lines_ - 1) {
        Line &next = line_buffer_[cursor_y_ + 1];
        int space = ED_MAX_LINE_LEN - ln.len;
        if (space > next.len) space = next.len;
        for (int i = 0; i < space; ++i) ln.text[ln.len + i] = next.text[i];
        ln.len += space;
        for (int i = cursor_y_ + 1; i < num_lines_ - 1; ++i) line_buffer_[i] = line_buffer_[i + 1];
        num_lines_--;
        modified_ = true;
    }
    ensure_cursor_visible();
    request_repaint();
}

void TextEditor::insert_newline() {
    if (num_lines_ >= ED_MAX_LINES) return;
    Line &ln = line_buffer_[cursor_y_];
    Line new_ln;
    new_ln.len = ln.len - cursor_x_;
    for (int i = 0; i < new_ln.len; ++i) new_ln.text[i] = ln.text[cursor_x_ + i];
    ln.len = cursor_x_;
    for (int i = num_lines_; i > cursor_y_ + 1; --i) line_buffer_[i] = line_buffer_[i - 1];
    line_buffer_[cursor_y_ + 1] = new_ln;
    num_lines_++;
    cursor_y_++; cursor_x_ = 0;
    modified_ = true;
    ensure_cursor_visible();
    request_repaint();
}

void TextEditor::move_cursor_home()   { cursor_x_ = 0; clear_selection(); ensure_cursor_visible(); request_repaint(); }
void TextEditor::move_cursor_end()    {
    if (cursor_y_ < num_lines_) cursor_x_ = line_buffer_[cursor_y_].len;
    clear_selection(); ensure_cursor_visible(); request_repaint();
}

void TextEditor::move_cursor_word_left() {
    if (cursor_x_ > 0) {
        Line &ln = line_buffer_[cursor_y_];
        while (cursor_x_ > 0 && !((ln.text[cursor_x_ - 1] >= 'A' && ln.text[cursor_x_ - 1] <= 'Z') ||
                                  (ln.text[cursor_x_ - 1] >= 'a' && ln.text[cursor_x_ - 1] <= 'z') ||
                                  (ln.text[cursor_x_ - 1] >= '0' && ln.text[cursor_x_ - 1] <= '9') ||
                                  ln.text[cursor_x_ - 1] == '_')) cursor_x_--;
        while (cursor_x_ > 0 && ((ln.text[cursor_x_ - 1] >= 'A' && ln.text[cursor_x_ - 1] <= 'Z') ||
                                 (ln.text[cursor_x_ - 1] >= 'a' && ln.text[cursor_x_ - 1] <= 'z') ||
                                 (ln.text[cursor_x_ - 1] >= '0' && ln.text[cursor_x_ - 1] <= '9') ||
                                 ln.text[cursor_x_ - 1] == '_')) cursor_x_--;
    }
    clear_selection(); ensure_cursor_visible(); request_repaint();
}

void TextEditor::move_cursor_word_right() {
    if (cursor_y_ >= num_lines_) return;
    Line &ln = line_buffer_[cursor_y_];
    if (cursor_x_ < ln.len) {
        while (cursor_x_ < ln.len && ((ln.text[cursor_x_] >= 'A' && ln.text[cursor_x_] <= 'Z') ||
                                      (ln.text[cursor_x_] >= 'a' && ln.text[cursor_x_] <= 'z') ||
                                      (ln.text[cursor_x_] >= '0' && ln.text[cursor_x_] <= '9') ||
                                      ln.text[cursor_x_] == '_')) cursor_x_++;
        while (cursor_x_ < ln.len && !((ln.text[cursor_x_] >= 'A' && ln.text[cursor_x_] <= 'Z') ||
                                         (ln.text[cursor_x_] >= 'a' && ln.text[cursor_x_] <= 'z') ||
                                         (ln.text[cursor_x_] >= '0' && ln.text[cursor_x_] <= '9') ||
                                         ln.text[cursor_x_] == '_')) cursor_x_++;
    }
    clear_selection(); ensure_cursor_visible(); request_repaint();
}

void TextEditor::move_cursor_line_up()    {
    if (cursor_y_ > 0) { cursor_y_--; if (cursor_x_ > line_buffer_[cursor_y_].len) cursor_x_ = line_buffer_[cursor_y_].len; }
    clear_selection(); ensure_cursor_visible(); request_repaint();
}
void TextEditor::move_cursor_line_down()  {
    if (cursor_y_ < num_lines_ - 1) { cursor_y_++; if (cursor_x_ > line_buffer_[cursor_y_].len) cursor_x_ = line_buffer_[cursor_y_].len; }
    clear_selection(); ensure_cursor_visible(); request_repaint();
}

void TextEditor::move_cursor_page_up()    {
    int vis = visible_lines(); if (vis < 2) vis = 2;
    cursor_y_ -= vis - 1; if (cursor_y_ < 0) cursor_y_ = 0;
    if (cursor_x_ > line_buffer_[cursor_y_].len) cursor_x_ = line_buffer_[cursor_y_].len;
    clear_selection(); ensure_cursor_visible(); request_repaint();
}
void TextEditor::move_cursor_page_down()  {
    int vis = visible_lines(); if (vis < 2) vis = 2;
    cursor_y_ += vis - 1; if (cursor_y_ >= num_lines_) cursor_y_ = num_lines_ - 1;
    if (cursor_x_ > line_buffer_[cursor_y_].len) cursor_x_ = line_buffer_[cursor_y_].len;
    clear_selection(); ensure_cursor_visible(); request_repaint();
}

void TextEditor::ensure_cursor_visible() {
    if (cursor_y_ < 0) cursor_y_ = 0;
    if (cursor_y_ >= num_lines_) cursor_y_ = num_lines_ - 1;
    if (cursor_y_ < 0) cursor_y_ = 0;
    if (cursor_x_ < 0) cursor_x_ = 0;
    if (cursor_y_ < num_lines_ && cursor_x_ > line_buffer_[cursor_y_].len) cursor_x_ = line_buffer_[cursor_y_].len;
    int vis_lines = visible_lines();
    if (cursor_y_ < scroll_y_) scroll_y_ = cursor_y_;
    if (cursor_y_ >= scroll_y_ + vis_lines) scroll_y_ = cursor_y_ - vis_lines + 1;
    if (scroll_y_ < 0) scroll_y_ = 0;
    int vis_cols = visible_cols();
    if (cursor_x_ < scroll_x_) scroll_x_ = cursor_x_;
    if (cursor_x_ >= scroll_x_ + vis_cols) scroll_x_ = cursor_x_ - vis_cols + 1;
    if (scroll_x_ < 0) scroll_x_ = 0;
    cursor_visible_ = true;
    cursor_blink_counter_ = 0;
}

/* --------------------------------------------------------------------------- */
/*  Selection & clipboard                                                       */
/* --------------------------------------------------------------------------- */

void TextEditor::get_selection_range(int &sx, int &sy, int &ex, int &ey) const {
    sx = anchor_x_; sy = anchor_y_; ex = cursor_x_; ey = cursor_y_;
    if (sy > ey || (sy == ey && sx > ex)) { int tx = sx, ty = sy; sx = ex; sy = ey; ex = tx; ey = ty; }
}

void TextEditor::delete_selection() {
    int sx, sy, ex, ey;
    if (!has_selection()) return;
    get_selection_range(sx, sy, ex, ey);
    if (sy == ey) {
        Line &ln = line_buffer_[sy];
        for (int i = sx; i < ln.len - (ex - sx); ++i) ln.text[i] = ln.text[i + (ex - sx)];
        ln.len -= (ex - sx);
        cursor_x_ = sx; cursor_y_ = sy;
    } else {
        Line &first = line_buffer_[sy];
        Line &last = line_buffer_[ey];
        first.len = sx;
        int keep = last.len - ex;
        for (int i = 0; i < keep; ++i) { if (first.len >= ED_MAX_LINE_LEN) break; first.text[first.len++] = last.text[ex + i]; }
        int removed = ey - sy;
        for (int i = sy + 1; i + removed < num_lines_; ++i) line_buffer_[i] = line_buffer_[i + removed];
        num_lines_ -= removed;
        cursor_x_ = sx; cursor_y_ = sy;
    }
    clear_selection(); modified_ = true;
    ensure_cursor_visible(); request_repaint();
}

void TextEditor::cut_selection()   { if (!has_selection()) { cut_line(); return; } copy_selection(); delete_selection(); request_repaint(); }

void TextEditor::copy_selection() {
    if (!has_selection()) return;
    int sx, sy, ex, ey;
    get_selection_range(sx, sy, ex, ey);
    int buf_pos = 0;
    for (int row = sy; row <= ey && buf_pos < (int)sizeof(clipboard_) - 1; ++row) {
        if (row < 0 || row >= num_lines_) continue;
        const Line &ln = line_buffer_[row];
        int start = (row == sy) ? sx : 0;
        int end   = (row == ey) ? ex : ln.len;
        for (int i = start; i < end && buf_pos < (int)sizeof(clipboard_) - 1; ++i) clipboard_[buf_pos++] = ln.text[i];
        if (row < ey && buf_pos < (int)sizeof(clipboard_) - 1) clipboard_[buf_pos++] = '\n';
    }
    clipboard_[buf_pos] = '\0';
}

void TextEditor::paste_at_cursor() {
    int i = 0;
    while (clipboard_[i] != '\0') {
        if (clipboard_[i] == '\n') insert_newline();
        else insert_char(clipboard_[i]);
        i++;
        if (i >= (int)sizeof(clipboard_)) break;
    }
    request_repaint();
}

void TextEditor::cut_line() {
    if (cursor_y_ < 0 || cursor_y_ >= num_lines_) return;
    Line &ln = line_buffer_[cursor_y_];
    int n = ln.len;
    if (n > (int)sizeof(clipboard_) - 1) n = (int)sizeof(clipboard_) - 1;
    for (int i = 0; i < n; ++i) clipboard_[i] = ln.text[i];
    clipboard_[n] = '\0';
    for (int i = cursor_y_; i < num_lines_ - 1; ++i) line_buffer_[i] = line_buffer_[i + 1];
    num_lines_--;
    if (num_lines_ == 0) { line_buffer_[0].len = 0; num_lines_ = 1; }
    if (cursor_y_ >= num_lines_) cursor_y_ = num_lines_ - 1;
    cursor_x_ = 0; modified_ = true; request_repaint();
}

void TextEditor::duplicate_line() {
    if (num_lines_ >= ED_MAX_LINES || cursor_y_ < 0 || cursor_y_ >= num_lines_) return;
    for (int i = num_lines_; i > cursor_y_ + 1; --i) line_buffer_[i] = line_buffer_[i - 1];
    line_buffer_[cursor_y_ + 1] = line_buffer_[cursor_y_];
    num_lines_++; cursor_y_++; cursor_x_ = 0;
    modified_ = true; request_repaint();
}

void TextEditor::extend_selection_to(int x, int y) {
    if (x < 0) x = 0; if (y < 0) y = 0;
    if (y >= num_lines_) y = num_lines_ - 1;
    if (y >= num_lines_) y = (num_lines_ > 0) ? num_lines_ - 1 : 0;
    if (x > line_buffer_[y].len) x = line_buffer_[y].len;
    cursor_x_ = x; cursor_y_ = y;
    request_repaint();
}

bool TextEditor::select_word_at(int x, int y) {
    if (y < 0 || y >= num_lines_) return false;
    if (x < 0) x = 0; if (x > line_buffer_[y].len) x = line_buffer_[y].len;
    const Line &ln = line_buffer_[y];
    if (x >= ln.len) return false;
    char c = ln.text[x];
    bool alnum = ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_');
    if (!alnum) return false;
    int start = x;
    while (start > 0) {
        char ch = ln.text[start - 1];
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_')) break;
        start--;
    }
    int end = x;
    while (end < ln.len) {
        char ch = ln.text[end];
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_')) break;
        end++;
    }
    anchor_x_ = start; anchor_y_ = y; cursor_x_ = end; cursor_y_ = y;
    return true;
}

bool TextEditor::select_line_at(int y) {
    if (y < 0 || y >= num_lines_) return false;
    anchor_x_ = 0; anchor_y_ = y; cursor_x_ = line_buffer_[y].len; cursor_y_ = y;
    return true;
}

/* --------------------------------------------------------------------------- */
/*  Drawing helpers                                                             */
/* --------------------------------------------------------------------------- */

void TextEditor::fill_rect(int x, int y, int w, int h, uint32_t color) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > win_w_) w = win_w_ - x;
    if (y + h > win_h_) h = win_h_ - y;
    if (w <= 0 || h <= 0) return;
    for (int iy = y; iy < y + h; ++iy) {
        uint32_t *row = &canvas_[iy * ED_MAX_W];
        for (int ix = x; ix < x + w; ++ix) row[ix] = color;
    }
}

void TextEditor::set_pixel(int x, int y, uint32_t color) {
    if (x >= 0 && x < win_w_ && y >= 0 && y < win_h_)
        canvas_[y * ED_MAX_W + x] = color;
}

void TextEditor::draw_text(int x, int y, const char *s, uint32_t color, int scale) {
    if (!s || !*s) return;
    if (scale < 1) scale = 1;
    if (scale > 3) scale = 3;
    __sc6(SYS_TEXT_DRAW,
        (uint64_t)(size_t)canvas_, (uint64_t)(size_t)s,
        (((uint64_t)(uint32_t)ED_MAX_W) << 16) | (uint32_t)win_h_,
        (((uint64_t)(uint16_t)x) << 16) | (uint16_t)y,
        (uint64_t)color, (uint64_t)scale);
}

int TextEditor::text_width(const char *s, int scale) { return get_text_width(s, scale); }

/* --------------------------------------------------------------------------- */
/*  Rendering                                                                   */
/* --------------------------------------------------------------------------- */

void TextEditor::render() {
    fill_rect(0, 0, win_w_, win_h_, ED_BG);
    int top = top_margin();
    int content_h = win_h_ - top - ED_BOTTOM_MARGIN;
    int line_h = line_height_px();
    int char_w = char_width_px();

    // Gutter
    fill_rect(0, top, ED_GUTTER_WIDTH, content_h, ED_LINE_NUMBER_BG);
    fill_rect(ED_GUTTER_WIDTH, top, 1, content_h, ED_BORDER);

    int first_line = scroll_y_;
    int last_line = first_line + (content_h / line_h) + 1;
    if (last_line >= num_lines_) last_line = num_lines_ - 1;

    // Draw visible lines
    for (int l = first_line; l <= last_line; ++l) {
        int row_y = top + (l - first_line) * line_h;

        // line number
        char num_str[12];
        snprintf(num_str, sizeof(num_str), "%d", l + 1);
        draw_text(4, row_y, num_str, ED_LINE_NUMBER_FG, font_scale_);

        const Line &ln = line_buffer_[l];
        int start_col = scroll_x_;
        int end_col = start_col + (win_w_ - ED_GUTTER_WIDTH) / char_w;
        if (end_col > ln.len) end_col = ln.len;
        if (start_col < 0) start_col = 0;

        // Draw text chunk
        if (start_col < end_col) {
            char buf[512];
            int len = end_col - start_col;
            if (len >= (int)sizeof(buf)) len = (int)sizeof(buf) - 1;
            for (int i = 0; i < len; ++i) buf[i] = ln.text[start_col + i];
            buf[len] = '\0';
            int draw_x = ED_GUTTER_WIDTH + 4 + (start_col - scroll_x_) * char_w;
            draw_text(draw_x, row_y, buf, ED_FG, font_scale_);
        }

        // Selection highlight
        if (has_selection()) {
            int sx, sy, ex, ey;
            get_selection_range(sx, sy, ex, ey);
            if (l >= sy && l <= ey) {
                int line_sx = (l == sy) ? sx : 0;
                int line_ex = (l == ey) ? ex : ln.len;
                if (line_sx < scroll_x_) line_sx = scroll_x_;
                int max_vis = scroll_x_ + (win_w_ - ED_GUTTER_WIDTH) / char_w;
                if (line_ex > max_vis) line_ex = max_vis;
                if (line_sx < line_ex) {
                    int sel_x = ED_GUTTER_WIDTH + 4 + (line_sx - scroll_x_) * char_w;
                    int sel_w = (line_ex - line_sx) * char_w;
                    fill_rect(sel_x, row_y, sel_w, line_h, ED_BG_SELECTION);
                }
            }
        }
    }

    // Cursor
    if (cursor_visible_ && cursor_y_ >= first_line && cursor_y_ <= last_line) {
        int cy = top + (cursor_y_ - first_line) * line_h;
        int cx = ED_GUTTER_WIDTH + 4 + (cursor_x_ - scroll_x_) * char_w;
        if (cx >= ED_GUTTER_WIDTH + 4 && cx < win_w_) {
            for (int yy = cy; yy < cy + line_h; ++yy) set_pixel(cx, yy, ED_CURSOR);
        }
    }

    // Status bar
    int status_y = win_h_ - ED_BOTTOM_MARGIN;
    fill_rect(0, status_y, win_w_, ED_BOTTOM_MARGIN, ED_STATUS_BG);
    fill_rect(0, status_y, win_w_, 1, ED_BORDER);
    char status[256];
    const char *base = filename_[0] ? filename_ : "untitled.txt";
    snprintf(status, sizeof(status), "  %s  %s  |  ln %d, col %d  |  %d lines",
             base, modified_ ? "[modified]" : "", cursor_y_ + 1, cursor_x_ + 1, num_lines_);
    draw_text(4, status_y + 2, status, ED_FG_DIM, font_scale_);
}

void TextEditor::request_repaint() { render(); if (win_) vui_request_repaint(win_); }

/* --------------------------------------------------------------------------- */
/*  Geometry helpers                                                            */
/* --------------------------------------------------------------------------- */

int TextEditor::top_margin()    const { return ED_TOP_MARGIN; }
int TextEditor::visible_lines() const { int h = win_h_ - ED_TOP_MARGIN - ED_BOTTOM_MARGIN; return h / line_height_px(); }
int TextEditor::visible_cols()  const { int w = win_w_ - ED_GUTTER_WIDTH; return w / char_width_px(); }

/* --------------------------------------------------------------------------- */
/*  File I/O                                                                    */
/* --------------------------------------------------------------------------- */

bool TextEditor::load_from_path(const char *path) {
    int fd = (int)__sc1(SYS_OPEN, (uint64_t)(size_t)path);
    if (fd < 0) return false;
    char buf[4096];
    int total = 0;
    int n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) total += n;
    close(fd);
    num_lines_ = 0;
    int line_start = 0;
    for (int i = 0; i < total; ++i) {
        if (buf[i] == '\n') {
            if (num_lines_ >= ED_MAX_LINES) break;
            Line &ln = line_buffer_[num_lines_];
            int len = i - line_start;
            if (len > ED_MAX_LINE_LEN) len = ED_MAX_LINE_LEN;
            ln.len = len;
            for (int j = 0; j < len; ++j) ln.text[j] = buf[line_start + j];
            num_lines_++;
            line_start = i + 1;
        }
    }
    if (line_start < total && num_lines_ < ED_MAX_LINES) {
        Line &ln = line_buffer_[num_lines_];
        int len = total - line_start;
        if (len > ED_MAX_LINE_LEN) len = ED_MAX_LINE_LEN;
        ln.len = len;
        for (int j = 0; j < len; ++j) ln.text[j] = buf[line_start + j];
        num_lines_++;
    }
    if (num_lines_ == 0) { line_buffer_[0].len = 0; num_lines_ = 1; }
    cursor_x_ = 0; cursor_y_ = 0; scroll_x_ = 0; scroll_y_ = 0;
    clear_selection(); modified_ = false;
    snprintf(filename_, sizeof(filename_), "%s", path);
    return true;
}

bool TextEditor::write_to_path(const char *path) {
    int fd = (int)__sc1(SYS_CREAT, (uint64_t)(size_t)path);
    if (fd < 0) return false;
    bool ok = true;
    for (int i = 0; i < num_lines_; ++i) {
        const Line &ln = line_buffer_[i];
        if (write(fd, ln.text, ln.len) != ln.len) { ok = false; break; }
        if (i < num_lines_ - 1) { if (write(fd, "\n", 1) != 1) { ok = false; break; } }
    }
    close(fd);
    if (ok) modified_ = false;
    return ok;
}

/* --------------------------------------------------------------------------- */
/*  Public actions                                                              */
/* --------------------------------------------------------------------------- */

void TextEditor::new_file() {
    num_lines_ = 1; line_buffer_[0].len = 0;
    cursor_x_ = 0; cursor_y_ = 0; scroll_x_ = 0; scroll_y_ = 0;
    clear_selection(); filename_[0] = '\0'; modified_ = false;
    request_repaint();
}

bool TextEditor::open_file(const char *path) { return load_from_path(path); }
bool TextEditor::save_file() { if (!filename_[0]) return false; return write_to_path(filename_); }
bool TextEditor::save_as(const char *path) { snprintf(filename_, sizeof(filename_), "%s", path); return write_to_path(path); }

/* --------------------------------------------------------------------------- */
/*  Missing implementations from header                                        */
/* --------------------------------------------------------------------------- */

void TextEditor::update_layout() {
    // re-read font metrics from the kernel
    uint32_t m = (uint32_t)__sc2(SYS_TEXT_METRICS, 0, (uint64_t)font_scale_);
    int lineh = (int)(m & 0xffu);
    int cellw = (int)((m >> 16) & 0xffu);
    cell_h_ = lineh > 0 ? lineh : 16;
    cell_w_ = cellw > 0 ? cellw : 9;
}

int TextEditor::row_to_px(int row) const {
    return top_margin() + (row - scroll_y_) * line_height_px();
}

void TextEditor::scroll_up(int lines)   { scroll_y_ -= lines; if (scroll_y_ < 0) scroll_y_ = 0; request_repaint(); }
void TextEditor::scroll_down(int lines) { scroll_y_ += lines; int max = num_lines_ - visible_lines(); if (scroll_y_ > max) scroll_y_ = max; request_repaint(); }

void TextEditor::remove_selected_within_line(int line_no, int col0, int col1) {
    if (line_no < 0 || line_no >= num_lines_) return;
    Line &ln = line_buffer_[line_no];
    if (col0 < 0) col0 = 0;
    if (col1 > ln.len) col1 = ln.len;
    if (col0 >= col1) return;
    int diff = col1 - col0;
    for (int i = col0; i < ln.len - diff; ++i) ln.text[i] = ln.text[i + diff];
    ln.len -= diff;
    modified_ = true;
}

void TextEditor::on_mouse_select_start(int x, int y) {
    if (y < 0 || y >= num_lines_) return;
    if (x < 0) x = 0;
    if (x > line_buffer_[y].len) x = line_buffer_[y].len;
    anchor_x_ = x; anchor_y_ = y;
    cursor_x_ = x; cursor_y_ = y;
    mouse_selecting_ = true;
    request_repaint();
}

void TextEditor::on_mouse_select_drag(int x, int y) {
    if (!mouse_selecting_) return;
    if (y < 0) y = 0;
    if (y >= num_lines_) y = num_lines_ - 1;
    if (x < 0) x = 0;
    if (line_buffer_[y].len < x) x = line_buffer_[y].len;
    extend_selection_to(x, y);
    ensure_cursor_visible();
}

void TextEditor::on_mouse_select_end() {
    mouse_selecting_ = false;
    if (anchor_x_ == cursor_x_ && anchor_y_ == cursor_y_) clear_selection();
    request_repaint();
}

void TextEditor::cut()  { cut_selection(); }
void TextEditor::copy()  { copy_selection(); }
void TextEditor::paste()  { paste_at_cursor(); }

/* --------------------------------------------------------------------------- */
/*  Event handlers                                                              */
/* --------------------------------------------------------------------------- */

void TextEditor::on_key(unsigned int key) {
    if (key < 0x20 && key != 0x08 && key != 0x0A && key != 0x0D && key != 0x1B) {
        switch (key) {
            case 0x01: { anchor_x_ = 0; anchor_y_ = 0; cursor_y_ = num_lines_ - 1; cursor_x_ = line_buffer_[cursor_y_].len; request_repaint(); return; }
            case 0x03: copy_selection(); return;
            case 0x0E: action_new(); return;
            case 0x0F: action_open(); return;
            case 0x13: action_save(); return;
            case 0x16: paste_at_cursor(); return;
            case 0x18: cut_selection(); return;
            case 0x1A: return; /* Ctrl+Z: undo not implemented */
            default: return;
        }
    }
    if (key == 0x1B) { clear_selection(); request_repaint(); return; }
    if (key == 0x08) { delete_backward(); return; }
    if (key == 0x0A || key == 0x0D) { insert_newline(); return; }
    if (key >= 0x20 && key < 0x7F) { insert_char((char)key); return; }
    if (key == 0x7F) { delete_forward(); return; }
}

void TextEditor::on_click(int x, int y) {
    int top = top_margin();
    int line_h = line_height_px();
    if (y < top || y > win_h_ - ED_BOTTOM_MARGIN) return;
    if (x < ED_GUTTER_WIDTH) return;
    int line_idx = scroll_y_ + (y - top) / line_h;
    if (line_idx < 0) line_idx = 0;
    if (line_idx >= num_lines_) line_idx = num_lines_ - 1;
    int char_w = char_width_px();
    int col = scroll_x_ + (x - ED_GUTTER_WIDTH - 4) / char_w;
    if (col < 0) col = 0;
    if (line_idx >= 0 && line_idx < num_lines_) { if (col > line_buffer_[line_idx].len) col = line_buffer_[line_idx].len; }
    cursor_x_ = col; cursor_y_ = line_idx;
    clear_selection(); mouse_selecting_ = true; start_selection();
    ensure_cursor_visible(); request_repaint();
}

void TextEditor::on_mouse_move(int x, int y) {
    if (!mouse_selecting_) return;
    int top = top_margin();
    int line_h = line_height_px();
    if (y < top || y > win_h_ - ED_BOTTOM_MARGIN) return;
    int line_idx = scroll_y_ + (y - top) / line_h;
    if (line_idx < 0) line_idx = 0;
    if (line_idx >= num_lines_) line_idx = num_lines_ - 1;
    int char_w = char_width_px();
    int col = scroll_x_ + (x - ED_GUTTER_WIDTH - 4) / char_w;
    if (col < 0) col = 0;
    if (line_idx >= 0 && line_idx < num_lines_) { if (col > line_buffer_[line_idx].len) col = line_buffer_[line_idx].len; }
    extend_selection_to(col, line_idx);
    ensure_cursor_visible(); request_repaint();
}

void TextEditor::on_mouse_release(int x, int y) {
    (void)x; (void)y;
    if (mouse_selecting_) {
        mouse_selecting_ = false;
        if (anchor_x_ == cursor_x_ && anchor_y_ == cursor_y_) clear_selection();
        request_repaint();
    }
}

void TextEditor::on_scroll(int delta) {
    scroll_y_ += delta * 3;
    if (scroll_y_ < 0) scroll_y_ = 0;
    if (scroll_y_ >= num_lines_) scroll_y_ = num_lines_ - 1;
    request_repaint();
}

void TextEditor::on_resize(int w, int h) {
    if (w < 400) w = 400; if (h < 300) h = 300;
    if (w > ED_MAX_W) w = ED_MAX_W; if (h > ED_MAX_H) h = ED_MAX_H;
    win_w_ = w; win_h_ = h;
    ensure_cursor_visible(); request_repaint();
}

void TextEditor::on_tick() {
    status_update();
    cursor_blink_counter_++;
    if (cursor_blink_counter_ >= 25) {
        cursor_blink_counter_ = 0;
        cursor_visible_ = !cursor_visible_;
        request_repaint();
    }
}

/* --------------------------------------------------------------------------- */
/*  Dialogs (stubs)                                                             */
/* --------------------------------------------------------------------------- */

void TextEditor::show_open_dialog() {
    char path[256];
    if (vui_file_dialog("Open File", "/home/user", path, sizeof(path), 0)) {
        open_file(path);
    }
}

void TextEditor::show_save_dialog() {
    char path[256];
    if (vui_file_dialog("Save As", "/home/user", path, sizeof(path), 1)) {
        save_as(path);
    }
}
void TextEditor::show_find_bar()     { find_bar_visible_ = true;  request_repaint(); }
void TextEditor::show_goto_dialog()  { goto_visible_     = true;  request_repaint(); }
void TextEditor::do_find_next(const char *) {}
void TextEditor::do_goto_line(int n) {
    if (n < 1) n = 1; if (n > num_lines_) n = num_lines_;
    cursor_y_ = n - 1; cursor_x_ = 0; clear_selection();
    ensure_cursor_visible(); request_repaint();
}
void TextEditor::status_update() {
    if (!status_label_) return;
    char buf[128];
    const char *fn = filename_[0] ? filename_ : "Untitled";
    snprintf(buf, sizeof(buf), "%s%s | L: %d, C: %d", 
             fn, modified_ ? "*" : "", cursor_y_ + 1, cursor_x_ + 1);
    vui_set_text(status_label_, buf);
}

/* --------------------------------------------------------------------------- */
/*  Constructor / main loop                                                     */
/* --------------------------------------------------------------------------- */

TextEditor *TextEditor::s_instance = nullptr;

TextEditor::TextEditor() {
    s_instance = this;
    num_lines_ = 1; line_buffer_[0].len = 0;
    cursor_x_ = 0; cursor_y_ = 0; scroll_x_ = 0; scroll_y_ = 0;
    clear_selection(); filename_[0] = '\0'; modified_ = false;
    font_scale_ = 1; cell_w_ = 9; cell_h_ = 16;
    cursor_visible_ = true; cursor_blink_counter_ = 0;
}

TextEditor::~TextEditor() {
    if (win_) vui_quit(win_);
    s_instance = nullptr;
}

void TextEditor::run() {
    win_ = vui_window_open("TextEditor", win_w_, win_h_);
    if (!win_) return;
    win_w_ = vui_window_width(win_);
    win_h_ = vui_window_height(win_);

    /* Full-window canvas that presents the pixel buffer */
    vui_widget *canvas = vui_canvas_ex(win_, 0, 0, win_w_, win_h_,
                                       canvas_, ED_MAX_W);
    vui_set_anchor(canvas, VUI_ANCHOR_LEFT | VUI_ANCHOR_RIGHT |
                            VUI_ANCHOR_TOP | VUI_ANCHOR_BOTTOM);

    /* Menu bar */
    {
        vui_widget *mb = vui_menubar(win_);
        vui_widget *mf = vui_menu(win_, mb, "File");
        vui_on_click(vui_menuitem(win_, mf, "New"),       [](vui_widget *){ s_instance->action_new(); });
        vui_on_click(vui_menuitem(win_, mf, "Open..."),   [](vui_widget *){ s_instance->action_open(); });
        vui_on_click(vui_menuitem(win_, mf, "Save"),      [](vui_widget *){ s_instance->action_save(); });
        vui_on_click(vui_menuitem(win_, mf, "Save As..."),[](vui_widget *){ s_instance->action_save_as(); });
        vui_menu_separator(win_, mf);
        vui_on_click(vui_menuitem(win_, mf, "Quit"),      [](vui_widget *){ exit(0); });
        
        vui_widget *me = vui_menu(win_, mb, "Edit");
        vui_on_click(vui_menuitem(win_, me, "Cut"),       [](vui_widget *){ s_instance->cut(); });
        vui_on_click(vui_menuitem(win_, me, "Copy"),      [](vui_widget *){ s_instance->copy(); });
        vui_on_click(vui_menuitem(win_, me, "Paste"),     [](vui_widget *){ s_instance->paste(); });
        vui_menu_separator(win_, me);
        vui_on_click(vui_menuitem(win_, me, "Go to Line..."), [](vui_widget *){ s_instance->action_goto_line(); });

        vui_sync_menubar(win_);
    }

    /* Status bar at the bottom */
    status_label_ = vui_label(win_, 8, win_h_ - 18, "Ready");
    vui_set_anchor(status_label_, VUI_ANCHOR_LEFT | VUI_ANCHOR_BOTTOM);
    vui_set_color(status_label_, ED_FG_DIM);

    /* Dock context menu */
    vui_add_dock_item(win_, "New Editor", [](vui_window *){
        vos_spawn("/bin/texteditor");
    });

    vui_on_key(win_, ed_on_key);
    vui_on_mouse_click(win_, ed_on_click);
    vui_on_mouse_move(win_, ed_on_mm);
    vui_on_mouse_release(win_, ed_on_mr);
    vui_on_scroll(win_, ed_on_scroll);
    vui_on_resize(win_, ed_on_resize);
    vui_on_tick(win_, ed_on_tick);

    update_layout();
    ensure_cursor_visible();
    render();
    vui_request_repaint(win_);
    vui_run(win_);
}
