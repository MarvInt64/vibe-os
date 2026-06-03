/*
 * texteditor/texteditor.h — Header for the VibeOS GUI Text Editor.
 *
 * This editor uses the VexUI (libvexui) in retained mode.
 * The editing area is hand-drawn via canvas (pixel buffer),
 * the toolbar chrome consists of real VexUI widgets.
 *
 * Language: C++11 (compatible with the VibeOS cross-compiler).
 */

#ifndef TEXTEDITOR_H
#define TEXTEDITOR_H

#include "vexui.h"
#include <stdint.h>
#include <stddef.h>

/*
 * Conservative fixed maximums, allocated statically.
 * The buffers are small enough to avoid needing a heap.
 */
static constexpr int ED_MAX_LINES    = 2048;
static constexpr int ED_MAX_LINE_LEN = 512;
static constexpr int ED_MAX_W        = 1024;
static constexpr int ED_MAX_H        = 768;

/*
 * Line — a single line in the buffer.
 * Implemented as a resizable array within the fixed line pool.
 */
struct Line {
    int  len = 0;                  /* never allowed to exceed ED_MAX_LINE_LEN */
    char text[ED_MAX_LINE_LEN];   /* line content, NOT zero-terminated */
};

/*
 * TextEditor — state machine for the GUI text editor application.
 *
 * Core state:
 *   - line_buffer_[] : all lines of the document
 *   - num_lines_     : current number of lines
 *   - cursor_y_      : cursor row index (0-based)
 *   - cursor_x_      : cursor column index (0-based)
 *   - scroll_y_      : how many lines are clipped off the top of the viewport
 *   - scroll_x_      : how many columns are clipped off the left (h. scroll)
 *   - modified_      : changed since last save?
 *   - filename_      : path to the file (empty = untitled)
 */
class TextEditor {
public:
    TextEditor();
    ~TextEditor();

    void run();                    /* open window, enter event loop */

    /* Event handlers (routed from global C callbacks) */
    void on_key(unsigned int key);
    void on_click(int x, int y);
    void on_mouse_move(int x, int y);
    void on_mouse_release(int x, int y);
    void on_scroll(int delta);
    void on_resize(int w, int h);
    void on_tick();

    /* File operations */
    void new_file();
    bool open_file(const char *path);
    bool save_file();              /* requires a filename set */
    bool save_as(const char *path);

    /* Selection & clipboard */
    void cut();
    void copy();
    void paste();

    /* Toolbar actions (called from VexUI widget callbacks) */
    void action_new()          { new_file(); }
    void action_open()         { show_open_dialog(); }
    void action_save()         { if (filename_[0]) save_file(); else show_save_dialog(); }
    void action_save_as()      { show_save_dialog(); }
    void action_find()         { show_find_bar(); }
    void action_goto_line()    { show_goto_dialog(); }

    /* Singleton accessor for global C callbacks */
    static TextEditor *instance() { return s_instance; }

private:
    static TextEditor *s_instance;

    /* --- VexUI --- */
    vui_window *win_ = nullptr;
    uint32_t canvas_[ED_MAX_W * ED_MAX_H];
    int win_w_ = 900;
    int win_h_ = 640;

    /* --- Toolbar widgets (menubar, buttons, search input etc.) --- */
    vui_widget *status_label_    = nullptr;   /* status line (bottom left) */
    vui_widget *find_input_      = nullptr;   /* find/replace input */
    vui_widget *goto_input_      = nullptr;   /* go to line input */
    vui_widget *saveas_input_    = nullptr;   /* save-as input */

    bool find_bar_visible_       = false;
    bool goto_visible_           = false;
    bool saveas_visible_         = false;

    /* --- text buffer --- */
    Line line_buffer_[ED_MAX_LINES];
    int  num_lines_  = 0;
    int  total_allocated_ = ED_MAX_LINES;

    /* --- cursor & viewport --- */
    int  cursor_x_  = 0;
    int  cursor_y_  = 0;
    int  scroll_y_  = 0;           /* vertical scroll offset in rows */
    int  scroll_x_  = 0;           /* horizontal scroll offset in visible columns */

    /* --- selection --- */
    int  anchor_x_  = -1;          /* column of the anchor point */
    int  anchor_y_  = -1;          /* row of the anchor point */

    bool mouse_selecting_ = false; /* whether we are currently selecting by mouse */

    /* --- clipboard --- */
    char clipboard_[8192];         /* large enough for multi-line clip */

    /* --- cursor blink --- */
    bool cursor_visible_ = true;
    int  cursor_blink_counter_ = 0;

    /* --- program state --- */
    bool modified_  = false;
    char filename_[256];           /* path to the file (empty = untitled) */

    /* --- font metrics --- */
    int  font_scale_ = 1;
    int  cell_w_     = 9;
    int  cell_h_     = 16;

    /* --- rendering --- */
    void render();                 /* full redraw of the canvas */
    void request_repaint();        /* flags the need for a frame redraw */

    /* low-level draw code directly into the pixel array (like FileBrowser) */
    void fill_rect(int x, int y, int w, int h, uint32_t color);
    void draw_text(int x, int y, const char *s, uint32_t color, int scale = 1);
    int  text_width(const char *s, int scale = 1);
    void set_pixel(int x, int y, uint32_t color);

    /* update cell dimensions from the kernel font metrics */
    void update_layout();

    /* --- selection & clipboard operations --- */
    void start_selection()                { anchor_x_ = cursor_x_; anchor_y_ = cursor_y_; }
    void clear_selection()                  { anchor_x_ = -1; anchor_y_ = -1; }
    void extend_selection_to(int x, int y);
    bool has_selection() const             { return anchor_x_ != -1 && anchor_y_ != -1; }
    void get_selection_range(int &sx, int &sy, int &ex, int &ey) const;
    void delete_selection();
    void cut_selection();
    void copy_selection();
    void paste_at_cursor();

    /* selection helpers */
    bool select_word_at(int x, int y);
    bool select_line_at(int y);

    /* mouse selection (x,y as pixel coordinates in the editor area) */
    void on_mouse_select_start(int x, int y);
    void on_mouse_select_drag(int x, int y);
    void on_mouse_select_end();

    /* --- buffer operations --- */
    void insert_char(char c);
    void delete_backward();
    void delete_forward();
    void insert_newline();
    void move_cursor_home();
    void move_cursor_end();
    void move_cursor_word_left();
    void move_cursor_word_right();
    void move_cursor_line_up();
    void move_cursor_line_down();
    void move_cursor_page_up();
    void move_cursor_page_down();
    void ensure_cursor_visible();
    void scroll_up(int lines);
    void scroll_down(int lines);
    void cut_line();
    void duplicate_line();

    /* remove the selected characters from a single line */
    void remove_selected_within_line(int line_no, int col0, int col1);

    /* --- file I/O --- */
    bool load_from_path(const char *path);
    bool write_to_path(const char *path);

    /* --- dialog helpers --- */
    void show_open_dialog();
    void show_save_dialog();
    void show_find_bar();
    void show_goto_dialog();
    void hide_find_bar()    { find_bar_visible_ = false; status_update(); request_repaint(); }
    void hide_goto_bar()    { goto_visible_     = false; status_update(); request_repaint(); }
    void hide_saveas_bar()  { saveas_visible_   = false; status_update(); request_repaint(); }

    void do_find_next(const char *needle);
    void do_goto_line(int n);

    /* --- status line --- */
    void status_update();

    /* --- geometry helpers --- */
    int  line_height_px() const    { return cell_h_ * font_scale_; }
    int  char_width_px()  const    { return cell_w_ * font_scale_; }
    int  visible_lines() const;
    int  visible_cols() const;
    int  col_to_px(int col) const  { return col * char_width_px(); }
    int  row_to_px(int row) const;
    int  top_margin() const;
};

#endif /* TEXTEDITOR_H */
