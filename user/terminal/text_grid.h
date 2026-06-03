#ifndef VIBEOS_TERMINAL_TEXT_GRID_H
#define VIBEOS_TERMINAL_TEXT_GRID_H

#include <stdint.h>

/*
 * TextGrid — a character screen with scrollback, rendered with the kernel's
 * anti-aliased TrueType atlas (SYS_TEXT_DRAW) into a caller-owned ARGB buffer.
 *
 * Lines are stored in a ring of SCROLLBACK rows; the visible window is the last
 * `rows` lines, shifted up by a view offset the user controls with the wheel.
 * New output snaps the view back to the bottom. A thin scrollbar is painted on
 * the right edge when there is history to scroll through.
 *
 * It is a pure model + renderer: it interprets a byte stream (printable ASCII
 * plus \n, \r, \b, \t) and paints it. It owns no window and does no I/O.
 */
class TextGrid {
public:
    static constexpr int MAX_COLS    = 128;
    static constexpr int MAX_ROWS    = 48;
    static constexpr int SCROLLBACK  = 400;   /* total retained lines */

    /* Query kernel font metrics once (cell size for the given integer scale).
     * Safe to call before any window exists. */
    void load_metrics(int scale);

    /* Lay out a cols x rows visible window. Clamped to MAX_COLS/MAX_ROWS. */
    void resize(int cols, int rows);

    int pixel_width()  const { return cols_ * cell_w_; }
    int pixel_height() const { return rows_ * cell_h_; }
    int cell_w() const { return cell_w_; }
    int cell_h() const { return cell_h_; }

    /* Feed output/echo bytes; this snaps the view to the bottom. */
    void write(const char *data, int len);
    void put(char c);

    void clear();

    /* Scroll the view by delta lines (positive = older/up). */
    void scroll_view(int delta_lines);

    /* Paint into buf; buf_stride is the ARGB row pitch, buf_h the height. */
    void render(uint32_t *buf, int buf_stride, int buf_h) const;

private:
    char *line(int absolute);              /* ring slot for a logical line */
    const char *line(int absolute) const;
    int  oldest() const;                   /* first still-retained line */
    int  max_view_off() const;
    void newline();
    void backspace();
    void tab();

    char ring_[SCROLLBACK][MAX_COLS];
    int  cols_     = 80;
    int  rows_     = 24;
    int  cur_line_ = 0;   /* absolute index of the cursor's line */
    int  cur_c_    = 0;
    int  view_off_ = 0;   /* lines scrolled up from the bottom */

    int  scale_  = 1;
    int  cell_w_ = 9;
    int  cell_h_ = 16;
};

#endif
