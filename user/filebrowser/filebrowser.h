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
    void navigate(const char *path, bool push_history = true);
    void refresh_files();
    void render();

    // Event handlers
    void on_click(int x, int y);
    void on_mouse_move(int x, int y);
    void on_mouse_release(int x, int y);
    void on_key(uint32_t key);
    void on_scroll(int delta);
    void on_resize(int w, int h);
    void on_tick();

private:
    static FileBrowser *instance_;

    // UI Window and Framebuffer
    vui_window *win_ = nullptr;
    uint32_t *canvas_pixels_ = nullptr;
    int win_w_ = 960;
    int win_h_ = 640;

    // Navigation and state
    char current_path_[256];
    char search_query_[64];
    bool search_focused_ = false;
    bool grid_view_ = false;
    int selected_index_ = -1;

    // Scrollbar state
    int scroll_y_ = 0;
    int max_scroll_y_ = 0;
    bool scroll_dragging_ = false;
    int scroll_drag_start_y_ = 0;
    int scroll_drag_start_scroll_ = 0;
    bool scroll_hovered_ = false;

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

    // Helper functions for drawing
    void fill_rect(int x, int y, int w, int h, uint32_t color);
    void draw_rect(int x, int y, int w, int h, uint32_t color);
    void draw_line(int x1, int y1, int x2, int y2, uint32_t color);
    void draw_text(int x, int y, const char *text, uint32_t color, int scale = 1);
    void draw_svg(int x, int y, int size, const char *svg, uint32_t color);
    void draw_glass_panel(int x, int y, int w, int h, uint32_t base_color, int corner_r = 6);

    // Layout helper bounds
    void get_sidebar_rect(int &x, int &y, int &w, int &h);
    void get_listing_rect(int &x, int &y, int &w, int &h);
    void get_detail_rect(int &x, int &y, int &w, int &h);

    void update_preview();
    void create_new_folder();
};

#endif // FILEBROWSER_H
