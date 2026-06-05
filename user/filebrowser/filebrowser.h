#ifndef FILEBROWSER_H
#define FILEBROWSER_H

#include "vexui.h"
#include <stdint.h>
#include <stddef.h>

#define BROWSER_MAX_W 1024
#define BROWSER_MAX_H 768

struct FileEntry {
    char name[128];
    uint64_t size;
    uint64_t kind;  // 1 = Regular File, 2 = Directory
    char formatted_type[32];
    char formatted_size[16];
    char formatted_date[32];
};

struct SidebarItem {
    const char *label;
    const char *path;
    const char *svg_icon;
    bool is_header;
};

class FileBrowser {
public:
    static FileBrowser *instance() { return instance_; }

    FileBrowser();
    ~FileBrowser();

    void init();
    void run();

    // Dialog mode: when the file browser is spawned as a file picker by
    // vui_file_dialog() it runs as a modal OPEN/SAVE dialog. The browsing UI is
    // unchanged; an extra action bar (filename field + Open/Save + Cancel) is
    // added at the bottom and the chosen path is written to result_file.
    void set_dialog(bool save_mode, const char *initial_path, const char *result_file);

    void navigate(const char *path, bool push_history = true);
    void refresh_files();
    void render();

    // Event handlers (canvas region: file list, scrollbar, breadcrumbs).
    void on_click(int x, int y, uint32_t buttons);
    void on_mouse_move(int x, int y);
    void on_mouse_release(int x, int y, uint32_t buttons);
    void on_key(uint32_t key);
    void on_scroll(int delta);
    void on_resize(int w, int h);
    void on_tick();

    // Toolbar / sidebar actions, invoked from VexUI widget callbacks.
    void nav_back();
    void nav_forward();
    void nav_up();
    void do_refresh();
    void set_grid_view(bool grid);
    void select_sidebar(int index);
    void search_changed(const char *text);
    void create_new_folder();

    // Dialog action-bar handlers (invoked from VexUI widget callbacks).
    void dialog_accept();   // confirm the current selection / typed filename
    void dialog_cancel();   // abort without a result

private:
    // Write the chosen path to the result file and terminate the dialog. A
    // zero-byte result (Cancel) signals "no selection" back to the caller.
    void dialog_finish(const char *path);
    static FileBrowser *instance_;

    // UI Window and Framebuffer.
    // VexUI clamps windows to VUI_MAX_W/H (900x640); win_w_/win_h_ are set from
    // the real window size after open, never assumed.
    vui_window *win_ = nullptr;
    uint32_t *canvas_pixels_ = nullptr;
    int win_w_ = 900;
    int win_h_ = 640;

    // Retained VexUI controls (toolbar + sidebar live as real widgets; the file
    // list, breadcrumbs, preview and status bar are hand-rendered on the canvas).
    vui_widget *search_input_ = nullptr;
    vui_widget *sidebar_widgets_[24] = {};

    // Dialog mode state (see set_dialog). dialog_mode_ off => normal browser.
    bool dialog_mode_ = false;
    bool dialog_save_ = false;        // SAVE (filename field) vs OPEN (pick file)
    char result_file_[128] = {};      // path to write the chosen result into
    vui_widget *filename_input_ = nullptr;  // SAVE-mode filename entry
    vui_widget *accept_btn_ = nullptr;      // "Open" / "Save"
    vui_widget *cancel_btn_ = nullptr;      // "Cancel"

    // Navigation and state
    char current_path_[256];
    char search_query_[64];
    bool grid_view_ = false;
    int selected_index_ = -1;

    // Scrollbar state
    int scroll_y_ = 0;
    int max_scroll_y_ = 0;
    bool scroll_dragging_ = false;
    int scroll_drag_start_y_ = 0;
    int scroll_drag_start_scroll_ = 0;
    bool scroll_hovered_ = false;

    // Breadcrumb hit-testing state
    int  breadcrumb_count_ = 0;           /* number of path segments shown  */
    int  breadcrumb_x_[16] = {};          /* x-start of each segment        */
    int  breadcrumb_w_[16] = {};          /* pixel width of each segment    */
    char breadcrumb_path_[16][256] = {};  /* absolute path for each segment */

    // Preview panel
    bool preview_expanded_ = true;

    // Context menu (right-click on file)
    bool context_menu_visible_ = false;
    int  context_menu_x_ = 0;
    int  context_menu_y_ = 0;
    int  context_item_idx_ = -1;          /* index into filtered_indices_   */

    // Detail panel Open button (VexUI widget, created in build_widgets)
    vui_widget *open_file_btn_ = nullptr;

    // File lists
    FileEntry entries_[256];
    int entry_count_ = 0;
    int filtered_indices_[256];
    int filtered_count_ = 0;

    // History
    char history_[64][256];
    int history_pos_ = -1;
    int history_count_ = 0;

    // Sidebar
    SidebarItem sidebar_items_[24];
    int sidebar_count_ = 0;
    int selected_sidebar_idx_ = -1;

    // Details and preview
    char preview_text_[2048];
    int preview_lines_count_ = 0;
    bool has_preview_ = false;
    char preview_file_path_[256];

    // System metrics
    uint64_t free_disk_space_ = 0;
    uint64_t total_disk_space_ = 0;

    // Helper functions for drawing.
    // noinline: keeps these out of render()'s inlining budget so the compiler
    // does not reuse callee-saved registers (r13/this) as vectorised loop
    // counters inside render(), which caused silent stack corruption at -O2.
    __attribute__((noinline)) void fill_rect(int x, int y, int w, int h, uint32_t color);
    __attribute__((noinline)) void draw_rect(int x, int y, int w, int h, uint32_t color);
    __attribute__((noinline)) void draw_line(int x1, int y1, int x2, int y2, uint32_t color);
    __attribute__((noinline)) void draw_text(int x, int y, const char *text, uint32_t color, int scale = 1);
    __attribute__((noinline)) void draw_svg(int x, int y, int size, const char *svg, uint32_t color);
    void draw_glass_panel(int x, int y, int w, int h, uint32_t base_color, int corner_r = 6);

    // Layout helper bounds
    int  content_bottom() const;   // bottom Y of the content panels (above bars)
    void get_sidebar_rect(int &x, int &y, int &w, int &h);
    void get_listing_rect(int &x, int &y, int &w, int &h);
    void get_detail_rect(int &x, int &y, int &w, int &h);

    void update_preview();
    void build_widgets();              // create toolbar + sidebar VexUI controls
    void sync_sidebar_selection();     // reflect selected_sidebar_idx_ on widgets
};

#endif // FILEBROWSER_H
