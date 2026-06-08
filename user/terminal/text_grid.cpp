/* text_grid.cpp — see text_grid.h. */
#include "text_grid.h"

#include <vibeos.h>
#include <sys/syscall.h>

namespace {

/* Palette (XRGB). A calm dark terminal. */
constexpr uint32_t COL_BG      = 0x001a1b21u;
constexpr uint32_t COL_FG      = 0x00e6e6ebu;
constexpr uint32_t COL_CURSOR  = 0x0064f2ccu;
constexpr uint32_t COL_BAR     = 0x00394050u;   /* scrollbar track  */
constexpr uint32_t COL_THUMB   = 0x007a8699u;   /* scrollbar thumb  */

constexpr int TAB_WIDTH  = 4;
constexpr int BAR_W      = 4;   /* scrollbar width in px */

/* Draw one glyph into an ARGB buffer at top-left (x, y) using the kernel atlas. */
void atlas_glyph(uint32_t *buf, int stride, int buf_h, int x, int y,
                 char ch, uint32_t color, int scale) {
    /* Use monospace mode (bit 31 of scale) so each glyph is centred in
     * a fixed-width cell — essential for terminal column alignment.
     * vexui widgets (labels, buttons) don't use this wrapper; they call
     * vos_text_draw directly with proportional spacing. */
    vos_text_draw(buf, stride, buf_h, x, y, (char[]){ch,0}, color,
                  (uint32_t)scale | 0x80000000u);
}

void fill_rect(uint32_t *buf, int stride, int buf_h, int x, int y, int w, int h, uint32_t c) {
    for (int yy = y; yy < y + h && yy < buf_h; ++yy) {
        if (yy < 0) continue;
        for (int xx = x; xx < x + w; ++xx) {
            if (xx >= 0) buf[yy * stride + xx] = c;
        }
    }
}

}  // namespace

char *TextGrid::line(int absolute) { return ring_[((absolute % SCROLLBACK) + SCROLLBACK) % SCROLLBACK]; }
const char *TextGrid::line(int absolute) const { return ring_[((absolute % SCROLLBACK) + SCROLLBACK) % SCROLLBACK]; }

int TextGrid::oldest() const {
    int o = cur_line_ - SCROLLBACK + 1;
    return o < 0 ? 0 : o;
}

int TextGrid::max_view_off() const {
    int total = cur_line_ - oldest() + 1;
    int extra = total - rows_;
    return extra > 0 ? extra : 0;
}

void TextGrid::load_metrics(int scale) {
    scale_ = scale < 1 ? 1 : scale;
    /* SYS_TEXT_METRICS(text=0) packs: lineh | ascent<<8 | cellw<<16 | space<<24. */
    uint32_t m = vos_font_metrics(scale_);
    int lineh = (int)(m & 0xffu);
    int cellw = (int)((m >> 16) & 0xffu);
    cell_h_ = lineh > 0 ? lineh : 16;
    cell_w_ = cellw > 0 ? cellw : 9;
}

void TextGrid::resize(int cols, int rows) {
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    if (cols > MAX_COLS) cols = MAX_COLS;
    if (rows > MAX_ROWS) rows = MAX_ROWS;
    cols_ = cols;
    rows_ = rows;
    if (cur_c_ >= cols_) cur_c_ = cols_ - 1;
}

void TextGrid::clear() {
    for (int r = 0; r < SCROLLBACK; ++r)
        for (int c = 0; c < MAX_COLS; ++c)
            ring_[r][c] = ' ';
    cur_line_ = 0;
    cur_c_ = 0;
    view_off_ = 0;
}

void TextGrid::newline() {
    cur_c_ = 0;
    ++cur_line_;
    char *l = line(cur_line_);          /* clear the line we are moving into */
    for (int c = 0; c < MAX_COLS; ++c) l[c] = ' ';
}

void TextGrid::backspace() {
    if (cur_c_ > 0) {
        --cur_c_;
        line(cur_line_)[cur_c_] = ' ';
    }
}

void TextGrid::tab() {
    int next = (cur_c_ / TAB_WIDTH + 1) * TAB_WIDTH;
    while (cur_c_ < next) put(' ');
}

void TextGrid::put(char c) {
    view_off_ = 0;   /* any new content snaps the view to the bottom */
    switch (c) {
        case '\n': newline();   return;
        case '\r': cur_c_ = 0;  return;
        case '\b': backspace(); return;
        case '\t': tab();       return;
        case 0:    return;
        default: break;
    }
    if (c < 0x20 || (unsigned char)c > 0x7e) {
        return;  /* drop other control / non-ASCII bytes for now */
    }
    line(cur_line_)[cur_c_] = c;
    if (++cur_c_ >= cols_) newline();
}

void TextGrid::write(const char *data, int len) {
    for (int i = 0; i < len; ++i) put(data[i]);
}

void TextGrid::scroll_view(int delta_lines) {
    view_off_ += delta_lines;
    if (view_off_ < 0) view_off_ = 0;
    int m = max_view_off();
    if (view_off_ > m) view_off_ = m;
}

void TextGrid::render(uint32_t *buf, int buf_stride, int buf_h) const {
    if (buf == 0) return;

    /* Background. */
    for (int i = 0; i < buf_stride * buf_h; ++i) buf[i] = COL_BG;

    /* Visible window: rows ending at (cur_line_ - view_off_). */
    int bottom = cur_line_ - view_off_;
    int top = bottom - (rows_ - 1);
    int first = oldest();
    for (int r = 0; r < rows_; ++r) {
        int L = top + r;
        if (L < first || L > cur_line_) continue;
        const char *src = line(L);
        for (int c = 0; c < cols_; ++c) {
            char ch = src[c];
            if (ch > ' ')
                atlas_glyph(buf, buf_stride, buf_h, c * cell_w_, r * cell_h_, ch, COL_FG, scale_);
        }
    }

    /* Blinking block cursor (only visible when following the bottom). */
    if (view_off_ == 0) {
        ++blink_frame_;
        bool cursor_on = (blink_frame_ % (BLINK_RATE * 2)) < BLINK_RATE;
        if (cursor_on) {
            int cx = cur_c_ * cell_w_;
            int cy = (rows_ - 1) * cell_h_;
            fill_rect(buf, buf_stride, buf_h, cx, cy, cell_w_, cell_h_, COL_CURSOR);
            char ch = line(cur_line_)[cur_c_];
            if (ch > ' ')
                atlas_glyph(buf, buf_stride, buf_h, cx, cy, ch, COL_BG, scale_);
        }
    }

    /* Scrollbar on the right edge of the visible area, when there is history. */
    int total = cur_line_ - first + 1;
    if (total > rows_) {
        int track_h = buf_h;
        int bar_x = pixel_width() - BAR_W;
        if (bar_x < 0) bar_x = 0;
        fill_rect(buf, buf_stride, buf_h, bar_x, 0, BAR_W, track_h, COL_BAR);

        int thumb_h = track_h * rows_ / total;
        if (thumb_h < 12) thumb_h = 12;
        int range = max_view_off();                 /* lines of possible scroll */
        int pos   = range - view_off_;               /* 0 at top, range at bottom */
        int thumb_y = range > 0 ? (track_h - thumb_h) * pos / range : 0;
        fill_rect(buf, buf_stride, buf_h, bar_x, thumb_y, BAR_W, thumb_h, COL_THUMB);
    }
}
