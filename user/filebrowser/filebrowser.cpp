#include "filebrowser.h"
#include "icons.h"
#include "svg.h"
#include <sys/syscall.h>
#include <vibeos.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>

// Static buffer for the canvas framebuffer
static uint32_t g_canvas_pixels[BROWSER_MAX_W * BROWSER_MAX_H];

// Toolbar layout constants, shared between build_widgets() (widget placement)
// and render() (the canvas-drawn view-toggle pill + active-segment highlight).
#define TB_NAV_X        14   // x of the first navigation icon
#define TB_NAV_STEP     42   // spacing between navigation icons
#define TB_NEWFOLDER_X  188  // x of the "New Folder" button
#define TB_NEWFOLDER_W  132  // width of the "New Folder" button
#define TB_TOGGLE_X     336  // x of the list/grid toggle pill
#define TB_TOGGLE_SEG   44   // width of one toggle segment (two segments total)
#define TB_ROW_Y        7    // top y of the row-height controls (button/toggle/search)
#define TB_ROW_H        34   // height of the row-height controls

// Vertical layout constants shared across render() and the rect helpers.
#define CONTENT_TOP  80      // top y of the sidebar/listing/detail panels
#define STATUS_BAR_H 24      // height of the bottom status bar
#define DIALOG_BAR_H 48      // height of the OPEN/SAVE action bar (dialog mode)

FileBrowser *FileBrowser::instance_ = nullptr;

// Custom readdir wrapper to retrieve kind and size in one syscall
static int readdir_entry_ex(const char *path, uint32_t index, char *name, size_t name_size, uint64_t *kind, uint64_t *size) {
    long ret;
    uint64_t out_kind = 0;
    register uint64_t r8_val __asm__("r8");
    register uint64_t r10 __asm__("r10") = name_size;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret), "=d"(out_kind), "=r"(r8_val)
        : "a"((long)SYS_READDIR), "D"((uint64_t)(size_t)path), "S"((uint64_t)index), "d"((uint64_t)(size_t)name), "r"(r10)
        : "rcx", "r11", "memory"
    );
    if (kind) *kind = out_kind;
    if (size) *size = r8_val;
    return (int)ret;
}

// Proportional text width syscall wrapper
static int get_text_width(const char *s, int scale = 1) {
    if (!s || !*s) return 0;
    if (scale < 1) scale = 1;
    if (scale > 3) scale = 3;
    return vos_text_metrics(s, scale);
}

// Helper to truncate text to fit a max width in pixels
static void truncate_text_to_width(const char *src, char *dst, int max_w, int scale = 1) {
    if (get_text_width(src, scale) <= max_w) {
        strcpy(dst, src);
        return;
    }
    int dot_w = get_text_width("...", scale);
    int target_w = max_w - dot_w;
    if (target_w < 0) target_w = 0;
    
    char tmp[128];
    int i = 0;
    while (src[i] && i < 120) {
        tmp[i] = src[i];
        tmp[i+1] = '\0';
        if (get_text_width(tmp, scale) > target_w) {
            break;
        }
        i++;
    }
    
    int j = 0;
    for (j = 0; j < i; ++j) {
        dst[j] = tmp[j];
    }
    dst[j++] = '.';
    dst[j++] = '.';
    dst[j++] = '.';
    dst[j] = '\0';
}

// Custom sorting comparator: directories first, then files alphabetically
static bool compare_entries(const FileEntry &a, const FileEntry &b) {
    if (a.kind != b.kind) {
        return a.kind > b.kind; // Dir (2) comes before File (1)
    }
    int i = 0;
    while (a.name[i] && b.name[i]) {
        char ca = a.name[i];
        char cb = b.name[i];
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return ca < cb;
        i++;
    }
    return a.name[i] == '\0';
}

FileBrowser::FileBrowser() {
    instance_ = this;
    canvas_pixels_ = g_canvas_pixels;
    strcpy(current_path_, "/");
    search_query_[0] = '\0';
    preview_text_[0] = '\0';
    preview_file_path_[0] = '\0';
    
    // Initialize navigation history
    history_pos_ = -1;
    history_count_ = 0;
}

FileBrowser::~FileBrowser() {
    if (win_) {
        vui_quit(win_);
    }
}

void FileBrowser::init() {
    instance_ = this;
    canvas_pixels_ = g_canvas_pixels;

    // Resolve the current user's home directory for sidebar paths.
    char home[128];
    if (vos_home_dir(home, sizeof(home)) <= 0)
        strcpy(home, "/root");  /* fallback */

    char desktop[160], documents[160], downloads[160];
    snprintf(desktop,   sizeof(desktop),   "%s/Desktop",   home);
    snprintf(documents, sizeof(documents), "%s/Documents", home);
    snprintf(downloads, sizeof(downloads), "%s/Downloads", home);

    // Populate Sidebar items
    sidebar_count_ = 0;
    sidebar_items_[sidebar_count_++] = {"FAVORITES", "", "", true};
    sidebar_items_[sidebar_count_++] = {"Recent", "/", SVG_RECENT, false};
    sidebar_items_[sidebar_count_++] = {"Starred", "/", SVG_STAR, false};
    sidebar_items_[sidebar_count_++] = {"Projects", "/home", SVG_FOLDER_OUTLINE, false};

    sidebar_items_[sidebar_count_++] = {"HOME", "", "", true};
    sidebar_items_[sidebar_count_++] = {"Home", home, SVG_HOME, false};
    sidebar_items_[sidebar_count_++] = {"Desktop", desktop, SVG_DESKTOP, false};
    sidebar_items_[sidebar_count_++] = {"Documents", documents, SVG_DOCUMENTS, false};
    sidebar_items_[sidebar_count_++] = {"Downloads", downloads, SVG_DOWNLOAD, false};

    sidebar_items_[sidebar_count_++] = {"SYSTEM", "", "", true};
    sidebar_items_[sidebar_count_++] = {"Root", "/", SVG_DISK, false};
    sidebar_items_[sidebar_count_++] = {"System", "/bin", SVG_CHIP, false};
    sidebar_items_[sidebar_count_++] = {"Packages", "/lib", SVG_PACKAGE, false};

    sidebar_items_[sidebar_count_++] = {"NETWORK", "", "", true};
    sidebar_items_[sidebar_count_++] = {"Network", "/", SVG_NETWORK, false};
    sidebar_items_[sidebar_count_++] = {"Servers", "/", SVG_ROOT, false};

    selected_sidebar_idx_ = 1; // Default to Recent (/)

    // Initialize mock disk size
    total_disk_space_ = 128ULL * 1024ULL * 1024ULL * 1024ULL; // 128 GB
    free_disk_space_ = 42ULL * 1024ULL * 1024ULL * 1024ULL + 800ULL * 1024ULL * 1024ULL; // 42.8 GB

    // Push initial history path
    navigate(current_path_, true);
}

// Configure the browser as a modal OPEN/SAVE file dialog. Called from main()
// before run() when the app is spawned by vui_file_dialog(). The action bar
// itself is created later in build_widgets() (which needs the open window).
void FileBrowser::set_dialog(bool save_mode, const char *initial_path, const char *result_file) {
    dialog_mode_ = true;
    dialog_save_ = save_mode;
    strncpy(result_file_, result_file ? result_file : "/tmp/fd_result", sizeof(result_file_) - 1);
    result_file_[sizeof(result_file_) - 1] = '\0';
    if (initial_path && initial_path[0])
        navigate(initial_path, true);
}

// Confirm the dialog: in SAVE mode the result is current_path/<typed name>; in
// OPEN mode it is the currently selected file. A folder selection in OPEN mode
// just navigates into it (nothing to accept yet).
void FileBrowser::dialog_accept() {
    char final_path[512];

    if (dialog_save_) {
        const char *name = filename_input_ ? vui_input_text(filename_input_) : "";
        if (!name || !name[0]) return;   // a filename is required
        if (strcmp(current_path_, "/") == 0)
            snprintf(final_path, sizeof(final_path), "/%s", name);
        else
            snprintf(final_path, sizeof(final_path), "%s/%s", current_path_, name);
    } else {
        if (selected_index_ < 0 || selected_index_ >= filtered_count_) return;
        FileEntry &fe = entries_[filtered_indices_[selected_index_]];
        if (strcmp(current_path_, "/") == 0)
            snprintf(final_path, sizeof(final_path), "/%s", fe.name);
        else
            snprintf(final_path, sizeof(final_path), "%s/%s", current_path_, fe.name);
        if (fe.kind == 2) {              // a directory: browse into it instead
            navigate(final_path, true);
            return;
        }
    }
    dialog_finish(final_path);
}

// Abort the dialog. Leaving the result file empty signals "cancelled" to the
// caller (vui_file_dialog reads zero bytes and returns 0).
void FileBrowser::dialog_cancel() {
    exit(1);
}

void FileBrowser::dialog_finish(const char *path) {
    // Write the chosen path to the result file the caller hands us. "w"
    // truncates any previous content so a shorter selection can't leave a tail.
    FILE *f = fopen(result_file_, "w");
    if (f) {
        fwrite(path, 1, strlen(path), f);
        fclose(f);
    }
    exit(0);
}

void FileBrowser::navigate(const char *path, bool push_history) {
    if (!path || !path[0]) return;
    
    // Normalize path bounds
    char norm[256];
    strncpy(norm, path, sizeof(norm));
    norm[sizeof(norm)-1] = '\0';
    
    // Strip trailing slash unless it is "/"
    int len = strlen(norm);
    if (len > 1 && norm[len-1] == '/') {
        norm[len-1] = '\0';
    }
    
    strcpy(current_path_, norm);
    selected_index_ = -1;
    scroll_y_ = 0;
    
    refresh_files();

    if (push_history) {
        // Discard forward history
        if (history_pos_ >= 0) {
            history_count_ = history_pos_ + 1;
        }
        if (history_pos_ < 63) {
            history_pos_++;
            strcpy(history_[history_pos_], current_path_);
            history_count_ = history_pos_ + 1;
        }
    }

    // Match sidebar item path to highlight it
    selected_sidebar_idx_ = -1;
    for (int i = 0; i < sidebar_count_; ++i) {
        if (!sidebar_items_[i].is_header && strcmp(sidebar_items_[i].path, current_path_) == 0) {
            selected_sidebar_idx_ = i;
            break;
        }
    }
    
    update_preview();
    sync_sidebar_selection();
    render();
    if (win_) vui_request_repaint(win_);
}

void FileBrowser::refresh_files() {
    entry_count_ = 0;
    char name[128];
    uint64_t kind = 0;
    uint64_t size = 0;

    for (uint32_t idx = 0; idx < 256; idx++) {
        if (entry_count_ >= 256) break;
        name[0] = '\0';
        int result = readdir_entry_ex(current_path_, idx, name, sizeof(name), &kind, &size);
        if (result <= 0) break;
        if (name[0] == '\0') continue;
        
        // Skip dot files (hidden or loop paths)
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;

        FileEntry &fe = entries_[entry_count_];
        strncpy(fe.name, name, sizeof(fe.name));
        fe.kind = kind;
        fe.size = size;

        // Extract dates and extensions for styling
        if (kind == 2) {
            strcpy(fe.formatted_type, "Folder");
            strcpy(fe.formatted_size, "—");
        } else {
            // Check extension
            const char *ext = strrchr(name, '.');
            if (ext) {
                if (strcmp(ext, ".md") == 0 || strcmp(ext, ".markdown") == 0) strcpy(fe.formatted_type, "Markdown");
                else if (strcmp(ext, ".cpp") == 0 || strcmp(ext, ".h") == 0) strcpy(fe.formatted_type, "C++ Source");
                else if (strcmp(ext, ".log") == 0) strcpy(fe.formatted_type, "Log");
                else if (strcmp(ext, ".txt") == 0) strcpy(fe.formatted_type, "Text");
                else if (strcmp(ext, ".png") == 0 || strcmp(ext, ".jpg") == 0) strcpy(fe.formatted_type, "Image");
                else if (strcmp(ext, ".svg") == 0) strcpy(fe.formatted_type, "Vector Image");
                else if (strcmp(ext, ".elf") == 0) strcpy(fe.formatted_type, "Executable");
                else snprintf(fe.formatted_type, sizeof(fe.formatted_type), "%s File", ext + 1);
            } else {
                strcpy(fe.formatted_type, "Document");
            }

            // Format size
            if (size >= 1024ULL * 1024ULL) {
                snprintf(fe.formatted_size, sizeof(fe.formatted_size), "%d.%d MB", (int)(size / (1024*1024)), (int)((size % (1024*1024)) / 100000));
            } else if (size >= 1024) {
                snprintf(fe.formatted_size, sizeof(fe.formatted_size), "%d.%d KB", (int)(size / 1024), (int)((size % 1024) / 100));
            } else {
                snprintf(fe.formatted_size, sizeof(fe.formatted_size), "%d B", (int)size);
            }
        }

        // Mock a realistic date modified based on name hashing so it matches look and feel
        int hash = 7;
        for (int i = 0; name[i]; ++i) hash = hash * 31 + name[i];
        int day = 10 + (abs(hash) % 18);
        int hour = 9 + (abs(hash) % 11);
        int minute = 10 + (abs(hash) % 48);
        snprintf(fe.formatted_date, sizeof(fe.formatted_date), "May %d, 2026 %02d:%02d", day, hour, minute);

        entry_count_++;
    }

    // Sort entries: folders first, then files alphabetically
    for (int i = 0; i < entry_count_ - 1; ++i) {
        for (int j = 0; j < entry_count_ - i - 1; ++j) {
            if (!compare_entries(entries_[j], entries_[j+1])) {
                FileEntry tmp = entries_[j];
                entries_[j] = entries_[j+1];
                entries_[j+1] = tmp;
            }
        }
    }

    // Filter index list based on search query
    filtered_count_ = 0;
    for (int i = 0; i < entry_count_; ++i) {
        if (search_query_[0] == '\0') {
            filtered_indices_[filtered_count_++] = i;
        } else {
            // Minimal substring match
            bool match = false;
            int q_len = strlen(search_query_);
            int n_len = strlen(entries_[i].name);
            for (int k = 0; k <= n_len - q_len; ++k) {
                int m = 0;
                for (m = 0; m < q_len; ++m) {
                    char c1 = entries_[i].name[k + m];
                    char c2 = search_query_[m];
                    if (c1 >= 'a' && c1 <= 'z') c1 -= 32;
                    if (c2 >= 'a' && c2 <= 'z') c2 -= 32;
                    if (c1 != c2) break;
                }
                if (m == q_len) {
                    match = true;
                    break;
                }
            }
            if (match) {
                filtered_indices_[filtered_count_++] = i;
            }
        }
    }

    // Compute max scroll limits
    int item_h = grid_view_ ? 96 : 36;
    int items_per_row = grid_view_ ? 4 : 1;
    int rows = (filtered_count_ + items_per_row - 1) / items_per_row;
    int content_h = rows * item_h;
    int visible_h = (content_bottom() - CONTENT_TOP) - 36; // listing height minus header row
    max_scroll_y_ = content_h - visible_h;
    if (max_scroll_y_ < 0) max_scroll_y_ = 0;
    if (scroll_y_ > max_scroll_y_) scroll_y_ = max_scroll_y_;
}

void FileBrowser::update_preview() {
    has_preview_ = false;
    preview_text_[0] = '\0';
    preview_lines_count_ = 0;
    preview_file_path_[0] = '\0';

    if (selected_index_ < 0 || selected_index_ >= filtered_count_) return;
    
    int actual_idx = filtered_indices_[selected_index_];
    FileEntry &fe = entries_[actual_idx];
    
    if (fe.kind != 1) return; // Directory has no file text preview

    // Open file and read lines
    char full_path[512];
    if (strcmp(current_path_, "/") == 0) {
        snprintf(full_path, sizeof(full_path), "/%s", fe.name);
    } else {
        snprintf(full_path, sizeof(full_path), "%s/%s", current_path_, fe.name);
    }
    strcpy(preview_file_path_, full_path);

    // Only text formats
    const char *ext = strrchr(fe.name, '.');
    if (!ext) return;

    if (strcmp(ext, ".txt") != 0 && strcmp(ext, ".cpp") != 0 && 
        strcmp(ext, ".h") != 0 && strcmp(ext, ".log") != 0 && 
        strcmp(ext, ".md") != 0 && strcmp(ext, ".markdown") != 0) {
        return;
    }

    FILE *f = fopen(full_path, "r");
    if (!f) return;

    has_preview_ = true;
    char line_buf[128];
    int line_num = 0;
    char *p = preview_text_;
    char *end = preview_text_ + sizeof(preview_text_);

    while (fgets(line_buf, sizeof(line_buf), f) && line_num < 6 && p < end - 128) {
        // Strip trailing newline to normalize
        int l = strlen(line_buf);
        if (l > 0 && line_buf[l-1] == '\n') line_buf[l-1] = '\0';
        
        // Strip carriage returns too
        l = strlen(line_buf);
        if (l > 0 && line_buf[l-1] == '\r') line_buf[l-1] = '\0';

        p += snprintf(p, end - p, "%s\n", line_buf);
        line_num++;
    }
    preview_lines_count_ = line_num;
    fclose(f);
}

void FileBrowser::create_new_folder() {
    char new_path[512];
    int suffix = 0;
    
    // Find unique directory name
    while (true) {
        if (suffix == 0) {
            if (strcmp(current_path_, "/") == 0) {
                snprintf(new_path, sizeof(new_path), "/New Folder");
            } else {
                snprintf(new_path, sizeof(new_path), "%s/New Folder", current_path_);
            }
        } else {
            if (strcmp(current_path_, "/") == 0) {
                snprintf(new_path, sizeof(new_path), "/New Folder (%d)", suffix);
            } else {
                snprintf(new_path, sizeof(new_path), "%s/New Folder (%d)", current_path_, suffix);
            }
        }
        
        // Check if directory already exists
        int check = (int)__sc2(SYS_STAT, (uint64_t)(size_t)new_path, 0);
        if (check < 0) {
            // Path not found, safe to create
            break;
        }
        suffix++;
    }

    // Call VibeOS sys_mkdir wrapper
    mkdir(new_path, 0755);
    
    refresh_files();
    render();
}

// Drawing wrapper methods
void FileBrowser::fill_rect(int x, int y, int w, int h, uint32_t color) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > win_w_) w = win_w_ - x;
    if (y + h > win_h_) h = win_h_ - y;
    if (w <= 0 || h <= 0) return;
    for (int iy = y; iy < y + h; ++iy) {
        uint32_t *row = &canvas_pixels_[iy * BROWSER_MAX_W];
        for (int ix = x; ix < x + w; ++ix) {
            row[ix] = color;
        }
    }
}

void FileBrowser::draw_rect(int x, int y, int w, int h, uint32_t color) {
    if (w <= 0 || h <= 0) return;
    for (int ix = x; ix < x + w; ++ix) {
        if (ix >= 0 && ix < win_w_) {
            if (y >= 0 && y < win_h_) canvas_pixels_[y * BROWSER_MAX_W + ix] = color;
            if (y + h - 1 >= 0 && y + h - 1 < win_h_) canvas_pixels_[(y + h - 1) * BROWSER_MAX_W + ix] = color;
        }
    }
    for (int iy = y + 1; iy < y + h - 1; ++iy) {
        if (iy >= 0 && iy < win_h_) {
            if (x >= 0 && x < win_w_) canvas_pixels_[iy * BROWSER_MAX_W + x] = color;
            if (x + w - 1 >= 0 && x + w - 1 < win_w_) canvas_pixels_[iy * BROWSER_MAX_W + x + w - 1] = color;
        }
    }
}

void FileBrowser::draw_line(int x1, int y1, int x2, int y2, uint32_t color) {
    int dx = abs(x2 - x1), sx = x1 < x2 ? 1 : -1;
    int dy = -abs(y2 - y1), sy = y1 < y2 ? 1 : -1;
    int err = dx + dy, e2;
    while (true) {
        if (x1 >= 0 && x1 < win_w_ && y1 >= 0 && y1 < win_h_) {
            canvas_pixels_[y1 * BROWSER_MAX_W + x1] = color;
        }
        if (x1 == x2 && y1 == y2) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x1 += sx; }
        if (e2 <= dx) { err += dx; y1 += sy; }
    }
}

void FileBrowser::draw_text(int x, int y, const char *text, uint32_t color, int scale) {
    if (!text || !*text) return;
    if (scale < 1) scale = 1; else if (scale > 3) scale = 3;
    // The kernel uses the "buffer width" argument as the row stride (pitch) when
    // writing glyphs. Our framebuffer is allocated at BROWSER_MAX_W, NOT win_w_,
    // so we MUST pass the real stride here — otherwise text shears diagonally
    // relative to the rest of the canvas (which is blitted at BROWSER_MAX_W).
    vos_text_draw(canvas_pixels_, BROWSER_MAX_W, win_h_, x, y, text, color, scale);
}

void FileBrowser::draw_svg(int x, int y, int size, const char *svg, uint32_t color) {
    if (size > 128) size = 128;
    static uint32_t svg_buf[128 * 128];
    svg_render_rgba(svg, svg_buf, size, color & 0x00ffffffu);
    
    for (int iy = 0; iy < size; ++iy) {
        int py = y + iy;
        if (py < 0 || py >= win_h_) continue;
        uint32_t *row = &canvas_pixels_[py * BROWSER_MAX_W];
        uint32_t *src_row = &svg_buf[iy * size];
        for (int ix = 0; ix < size; ++ix) {
            int px = x + ix;
            if (px < 0 || px >= win_w_) continue;
            uint32_t src = src_row[ix];
            uint8_t alpha = (src >> 24) & 255;
            if (alpha == 0) continue;
            if (alpha == 255) {
                row[px] = src & 0x00ffffffu;
            } else {
                uint32_t bg = row[px];
                uint32_t r = (((src >> 16) & 0xff) * alpha + ((bg >> 16) & 0xff) * (255 - alpha)) / 255;
                uint32_t g = (((src >> 8) & 0xff) * alpha + ((bg >> 8) & 0xff) * (255 - alpha)) / 255;
                uint32_t b = ((src & 0xff) * alpha + (bg & 0xff) * (255 - alpha)) / 255;
                row[px] = (r << 16) | (g << 8) | b;
            }
        }
    }
}

void FileBrowser::draw_glass_panel(int x, int y, int w, int h, uint32_t base_color, int corner_r) {
    for (int iy = 0; iy < h; ++iy) {
        int py = y + iy;
        if (py < 0 || py >= win_h_) continue;
        
        int cx = 0;
        if (iy < corner_r) cx = corner_r - iy;
        else if (iy >= h - corner_r) cx = corner_r - (h - 1 - iy);
        
        int x0 = x + cx;
        int x1 = x + w - cx;
        if (x0 < 0) x0 = 0;
        if (x1 > win_w_) x1 = win_w_;
        
        uint32_t *row = &canvas_pixels_[py * BROWSER_MAX_W];
        for (int px = x0; px < x1; ++px) {
            uint32_t bg = row[px];
            uint32_t br = (base_color >> 16) & 255;
            uint32_t bg_r = (bg >> 16) & 255;
            uint32_t r = (br * 220 + bg_r * 35) / 255;
            
            uint32_t bg_g = (bg >> 8) & 255;
            uint32_t bg_b = bg & 255;
            uint32_t g = (((base_color >> 8) & 255) * 220 + bg_g * 35) / 255;
            uint32_t b = ((base_color & 255) * 220 + bg_b * 35) / 255;
            row[px] = (r << 16) | (g << 8) | b;
        }
    }
    // Highlight top reflection line
    draw_line(x + corner_r, y, x + w - corner_r, y, 0x003f4f66u);
}

// Bottom Y of the three content panels: above the status bar, and above the
// dialog action bar when present. Derived from win_h_ so a shorter dialog
// window (opened to clear the always-on-top dock) lays out correctly. For the
// standard 640-tall browser this evaluates to 616 (unchanged).
int FileBrowser::content_bottom() const {
    return win_h_ - STATUS_BAR_H - (dialog_mode_ ? DIALOG_BAR_H : 0);
}

void FileBrowser::get_sidebar_rect(int &x, int &y, int &w, int &h) {
    x = 0; y = CONTENT_TOP; w = 220; h = content_bottom() - CONTENT_TOP;
}

void FileBrowser::get_listing_rect(int &x, int &y, int &w, int &h) {
    x = 220; y = CONTENT_TOP; w = 460; h = content_bottom() - CONTENT_TOP;
}

void FileBrowser::get_detail_rect(int &x, int &y, int &w, int &h) {
    x = 680; y = CONTENT_TOP; w = 280; h = content_bottom() - CONTENT_TOP;
}

void FileBrowser::render() {
    if (!win_ || !canvas_pixels_) return;   // window not yet open
    // 1. Clear background of listing window
    fill_rect(0, 0, win_w_, win_h_, 0x0020384fu);

    // 2. Sidebar column background + divider. The interactive rows are real
    //    VexUI list-item widgets (created in run()); here we only paint the
    //    panel backdrop and the non-interactive section headers behind them.
    fill_rect(0, 80, 220, win_h_ - 80, 0x001b3147u);
    draw_line(220, 80, 220, win_h_, 0x00294058u);

    for (int i = 0; i < sidebar_count_; ++i) {
        if (sidebar_items_[i].is_header) {
            int item_y = 90 + i * 32;
            draw_text(16, item_y + 10, sidebar_items_[i].label, 0x005c6a7eu, 1);
        }
    }

    // 3. Toolbar band background + divider. The nav buttons, "New Folder",
    //    the view toggle and the search field are all real VexUI widgets; only
    //    the segmented view-toggle pill (and its active-segment highlight, which
    //    is stateful) is hand-painted here, behind the two toggle icons.
    fill_rect(0, 0, win_w_, 48, 0x0020384fu);
    draw_line(0, 48, win_w_, 48, 0x00294058u);

    int tg_x = TB_TOGGLE_X, tg_y = TB_ROW_Y, tg_w = TB_TOGGLE_SEG * 2, tg_h = TB_ROW_H;
    draw_glass_panel(tg_x, tg_y, tg_w, tg_h, 0x00121c2au, 6);
    int seg_x = grid_view_ ? tg_x + TB_TOGGLE_SEG : tg_x;
    draw_glass_panel(seg_x + 3, tg_y + 3, TB_TOGGLE_SEG - 6, tg_h - 6, 0x00243954u, 5);

    // 4. Render Breadcrumbs path bar (y: 48..80)
    fill_rect(0, 48, win_w_, 32, 0x0020384fu);
    draw_line(0, 80, win_w_, 80, 0x00294058u);

    // Home Icon + Path segments
    draw_svg(16, 55, 18, SVG_HOME, 0x009aa7bau);
    int bc_x = 40;
    draw_text(bc_x, 56, "Home", 0x009aa7bau);
    /* Store Home as breadcrumb segment 0 for hit-testing. */
    breadcrumb_x_[0] = bc_x;
    breadcrumb_w_[0] = get_text_width("Home");
    strcpy(breadcrumb_path_[0], "/");

    bc_x += breadcrumb_w_[0] + 8;
    int seg_idx = 1;

    // Split the current path into "/"-separated segments without relying on
    // strtok (not provided by the VibeOS libc). Each segment is rendered as a
    // breadcrumb preceded by a ">" separator.
    const char *p = current_path_;
    char accum_path[256] = "";
    while (*p) {
        while (*p == '/') ++p;          // skip separator(s)
        if (!*p) break;
        char segment[128];
        int n = 0;
        while (*p && *p != '/' && n < (int)sizeof(segment) - 1) {
            segment[n++] = *p++;
        }
        segment[n] = '\0';

        // Build accumulated path for this segment
        if (strcmp(accum_path, "/") == 0)
            snprintf(accum_path, sizeof(accum_path), "/%s", segment);
        else
            snprintf(accum_path, sizeof(accum_path), "%s/%s",
                     breadcrumb_path_[seg_idx - 1], segment);

        draw_text(bc_x, 56, ">", 0x005c6a7eu);
        bc_x += 16;
        draw_text(bc_x, 56, segment, 0x009aa7bau);

        /* Store segment hit-test info. */
        if (seg_idx < 16) {
            breadcrumb_x_[seg_idx] = bc_x;
            breadcrumb_w_[seg_idx] = get_text_width(segment);
            strcpy(breadcrumb_path_[seg_idx], accum_path);
        }

        bc_x += get_text_width(segment) + 8;
        ++seg_idx;
    }
    breadcrumb_count_ = seg_idx;

    // 5. Render Central File Workspace Listing
    int list_x, list_y, list_w, list_h;
    get_listing_rect(list_x, list_y, list_w, list_h);

    if (!grid_view_) {
        // Table Header
        fill_rect(list_x, list_y, list_w, 36, 0x0020384fu);
        draw_line(list_x, list_y + 36, list_x + list_w, list_y + 36, 0x00294058u);
        
        draw_text(list_x + 20, list_y + 11, "Name", 0x009aa7bau);
        draw_text(list_x + 190, list_y + 11, "Modified", 0x009aa7bau);
        draw_text(list_x + 290, list_y + 11, "Type", 0x009aa7bau);
        draw_text(list_x + 390, list_y + 11, "Size", 0x009aa7bau);

        // List Rows
        int row_start_y = list_y + 36;
        int visible_count = (list_h - 36) / 36;
        
        int start_item = scroll_y_ / 36;
        if (start_item < 0) start_item = 0;
        
        for (int i = 0; i < visible_count && (start_item + i) < filtered_count_; ++i) {
            int item_idx = start_item + i;
            int actual_idx = filtered_indices_[item_idx];
            FileEntry &fe = entries_[actual_idx];
            int ry = row_start_y + i * 36;

            // Highlight selected row
            if (item_idx == selected_index_) {
                fill_rect(list_x + 4, ry + 2, list_w - 8, 32, 0x00264a6bu);
                draw_rect(list_x + 4, ry + 2, list_w - 8, 32, 0x003b82f6u);
            }

            // Draw Icon (Folder/File)
            const char *svg = (fe.kind == 2) ? SVG_FOLDER : SVG_FILE;
            uint32_t icon_col = (fe.kind == 2) ? 0x003b82f6u : 0x009aa7bau;
            if (fe.kind == 1) {
                if (strstr(fe.name, ".md")) { svg = SVG_FILE_MD; icon_col = 0x003b82f6u; }
                else if (strstr(fe.name, ".cpp") || strstr(fe.name, ".h")) { svg = SVG_FILE_CODE; icon_col = 0x008f7bf0u; }
                else if (strstr(fe.name, ".log")) { svg = SVG_FILE_LOG; icon_col = 0x00f0b86eu; }
            }
            draw_svg(list_x + 10, ry + 7, 22, svg, icon_col);

            // Draw columns (Name, Modified, Type, Size). Each value is truncated
            // to its column width so neighbouring columns never overlap.
            char col_buf[64];
            truncate_text_to_width(fe.name, col_buf, 142);
            draw_text(list_x + 38, ry + 10, col_buf, 0x00e6edf7u);
            truncate_text_to_width(fe.formatted_date, col_buf, 92);
            draw_text(list_x + 190, ry + 10, col_buf, 0x009aa7bau);
            truncate_text_to_width(fe.formatted_type, col_buf, 88);
            draw_text(list_x + 290, ry + 10, col_buf, 0x009aa7bau);
            draw_text(list_x + 390, ry + 10, fe.formatted_size, 0x009aa7bau);
        }
    } else {
        // Grid View
        int visible_w_rows = list_h / 96;
        int start_row = scroll_y_ / 96;
        if (start_row < 0) start_row = 0;
        
        for (int i = 0; i < filtered_count_; ++i) {
            int grid_row = i / 4;
            int grid_col = i % 4;
            if (grid_row < start_row || grid_row >= start_row + visible_w_rows + 1) continue;

            int actual_idx = filtered_indices_[i];
            FileEntry &fe = entries_[actual_idx];
            
            int gx = list_x + 16 + grid_col * 108;
            int gy = list_y + 16 + (grid_row - start_row) * 96;

            // Highlight selected item grid cell
            if (i == selected_index_) {
                fill_rect(gx - 4, gy - 4, 96, 88, 0x00264a6bu);
                draw_rect(gx - 4, gy - 4, 96, 88, 0x003b82f6u);
            }

            // Big Icon
            const char *svg = (fe.kind == 2) ? SVG_FOLDER : SVG_FILE;
            uint32_t icon_col = (fe.kind == 2) ? 0x003b82f6u : 0x009aa7bau;
            if (fe.kind == 1) {
                if (strstr(fe.name, ".md")) { svg = SVG_FILE_MD; icon_col = 0x003b82f6u; }
                else if (strstr(fe.name, ".cpp") || strstr(fe.name, ".h")) { svg = SVG_FILE_CODE; icon_col = 0x008f7bf0u; }
                else if (strstr(fe.name, ".log")) { svg = SVG_FILE_LOG; icon_col = 0x00f0b86eu; }
            }
            draw_svg(gx + 20, gy + 4, 48, svg, icon_col);

            // Centered name label with truncation
            char trunc_name[64];
            truncate_text_to_width(fe.name, trunc_name, 80);
            int name_w = get_text_width(trunc_name);
            int name_x = gx + 44 - name_w / 2;
            draw_text(name_x, gy + 58, trunc_name, 0x00e6edf7u);
        }
    }

    // Scrollbar on the right side of the list column
    int sb_x = list_x + list_w - 12;
    int sb_y = list_y + 4;
    int sb_h = list_h - 8;
    int sb_w = 8;
    
    // Draw scrollbar track
    fill_rect(sb_x, sb_y, sb_w, sb_h, 0x001b3147u);
    
    // Calculate thumb metrics
    int items_h = grid_view_ ? 96 : 36;
    int total_rows = (filtered_count_ + (grid_view_ ? 3 : 0)) / (grid_view_ ? 4 : 1);
    int content_h = total_rows * items_h;
    int visible_h = list_h - (grid_view_ ? 0 : 36);
    
    if (content_h > visible_h) {
        int thumb_h = (sb_h * visible_h) / content_h;
        if (thumb_h < 20) thumb_h = 20;
        int thumb_y = sb_y + (scroll_y_ * (sb_h - thumb_h)) / (content_h - visible_h);
        
        uint32_t thumb_color = 0x00344259u; // default track divider color
        if (scroll_dragging_) thumb_color = 0x003b82f6u; // bright drag blue
        else if (scroll_hovered_) thumb_color = 0x004e6382u; // brighter hover slate
        
        fill_rect(sb_x, thumb_y, sb_w, thumb_h, thumb_color);
    }

    // Workspace column right divider
    draw_line(680, 80, 680, content_bottom(), 0x00294058u);

    // 6. Render Right Details Sidebar Pane
    int detail_x, detail_y, detail_w, detail_h;
    get_detail_rect(detail_x, detail_y, detail_w, detail_h);
    fill_rect(detail_x, detail_y, detail_w, detail_h, 0x0020384fu);

    if (selected_index_ >= 0 && selected_index_ < filtered_count_) {
        int actual_idx = filtered_indices_[selected_index_];
        FileEntry &fe = entries_[actual_idx];
        
        // Render detailed panel
        int pane_center_x = detail_x + detail_w / 2;

        // Draw big file type icon centered
        const char *svg = (fe.kind == 2) ? SVG_FOLDER : SVG_FILE;
        uint32_t icon_col = (fe.kind == 2) ? 0x003b82f6u : 0x009aa7bau;
        if (fe.kind == 1) {
            if (strstr(fe.name, ".md")) { svg = SVG_FILE_MD; icon_col = 0x003b82f6u; }
            else if (strstr(fe.name, ".cpp") || strstr(fe.name, ".h")) { svg = SVG_FILE_CODE; icon_col = 0x008f7bf0u; }
            else if (strstr(fe.name, ".log")) { svg = SVG_FILE_LOG; icon_col = 0x00f0b86eu; }
        }
        draw_svg(pane_center_x - 32, detail_y + 24, 64, svg, icon_col);

        // Render file name (bold)
        char trunc_title[64];
        truncate_text_to_width(fe.name, trunc_title, 240);
        int title_w = get_text_width(trunc_title);
        draw_text(pane_center_x - title_w / 2, detail_y + 100, trunc_title, 0x00e6edf7u);

        // Render subtitle details
        char subtitle[64];
        if (fe.kind == 2) {
            strcpy(subtitle, "Folder");
        } else {
            snprintf(subtitle, sizeof(subtitle), "%s Document - %s", fe.formatted_type, fe.formatted_size);
        }
        int sub_w = get_text_width(subtitle);
        draw_text(pane_center_x - sub_w / 2, detail_y + 118, subtitle, 0x009aa7bau);

        // Draw horizontal list dividers
        draw_line(detail_x + 16, detail_y + 140, detail_x + detail_w - 16, detail_y + 140, 0x00294058u);

        // Metadata grid (key-value text lines)
        int info_y = detail_y + 154;
        
        // Location row
        draw_text(detail_x + 16, info_y, "Location", 0x005c6a7eu);
        char loc_trunc[64];
        truncate_text_to_width(current_path_, loc_trunc, 160);
        draw_text(detail_x + 96, info_y, loc_trunc, 0x009aa7bau);
        info_y += 20;

        // Modified row
        draw_text(detail_x + 16, info_y, "Modified", 0x005c6a7eu);
        draw_text(detail_x + 96, info_y, fe.formatted_date, 0x009aa7bau);
        info_y += 20;

        // Created row
        draw_text(detail_x + 16, info_y, "Created", 0x005c6a7eu);
        // Simulate creation date slightly older than hash-modified date
        char mock_created[32];
        int day = 15;
        snprintf(mock_created, sizeof(mock_created), "May %d, 2026 13:07", day);
        draw_text(detail_x + 96, info_y, mock_created, 0x009aa7bau);
        info_y += 20;

        // Size row
        draw_text(detail_x + 16, info_y, "Size", 0x005c6a7eu);
        char size_bytes[32];
        if (fe.kind == 2) {
            strcpy(size_bytes, "—");
        } else {
            snprintf(size_bytes, sizeof(size_bytes), "%d bytes", (int)fe.size);
        }
        draw_text(detail_x + 96, info_y, size_bytes, 0x009aa7bau);
        info_y += 20;

        // Lines/Permissions/Owner rows
        if (fe.kind == 1) {
            draw_text(detail_x + 16, info_y, "Permissions", 0x005c6a7eu);
            draw_text(detail_x + 96, info_y, "-rw-r--r-- (644)", 0x009aa7bau);
            info_y += 20;

            draw_text(detail_x + 16, info_y, "Owner", 0x005c6a7eu);
            draw_text(detail_x + 96, info_y, "dev", 0x009aa7bau);
            info_y += 20;
        } else {
            draw_text(detail_x + 16, info_y, "Permissions", 0x005c6a7eu);
            draw_text(detail_x + 96, info_y, "drwxr-xr-x (755)", 0x009aa7bau);
            info_y += 20;
        }

        // Tags pill badge tags
        int tags_y = info_y + 12;
        draw_text(detail_x + 16, tags_y, "Tags", 0x005c6a7eu);
        
        int badge_x = detail_x + 60;
        // Pill: documentation badge
        fill_rect(badge_x, tags_y - 4, 95, 20, 0x00223147u);
        draw_rect(badge_x, tags_y - 4, 95, 20, 0x003b82f6u);
        draw_text(badge_x + 8, tags_y - 2, "documentation", 0x003b82f6u);
        
        badge_x += 102;
        // Pill: vibeos badge
        fill_rect(badge_x, tags_y - 4, 52, 20, 0x00223147u);
        draw_rect(badge_x, tags_y - 4, 52, 20, 0x003b82f6u);
        draw_text(badge_x + 8, tags_y - 2, "vibeos", 0x003b82f6u);
        
        badge_x += 58;
        // Draw plus badge
        fill_rect(badge_x, tags_y - 4, 20, 20, 0x00223147u);
        draw_text(badge_x + 6, tags_y - 2, "+", 0x009aa7bau);

        // 7. Embedded Code Preview folding panel
        if (has_preview_) {
            int prev_y = tags_y + 32;
            draw_line(detail_x + 16, prev_y, detail_x + detail_w - 16, prev_y, 0x00294058u);

            prev_y += 12;
            const char *toggle_label = preview_expanded_ ? "v Preview" : "> Preview";
            draw_text(detail_x + 16, prev_y, toggle_label, 0x00e6edf7u);

            if (preview_expanded_) {
                // Dark code block box
                int box_y = prev_y + 24;
                int box_h = detail_y + detail_h - box_y - 12;
                fill_rect(detail_x + 16, box_y, detail_w - 32, box_h, 0x00070c12u);
                draw_rect(detail_x + 16, box_y, detail_w - 32, box_h, 0x00294058u);

                // Parse lines out of preview_text_ and print with numbers
                int render_line_y = box_y + 10;
                char line_temp[128];
                const char *src_p = preview_text_;
                int curr_num = 1;

                while (*src_p && curr_num <= 6) {
                    int c = 0;
                    while (*src_p && *src_p != '\n' && c < 120) {
                        line_temp[c++] = *src_p++;
                    }
                    line_temp[c] = '\0';
                    if (*src_p == '\n') src_p++;

                    char num_str[8];
                    snprintf(num_str, sizeof(num_str), "%d", curr_num);
                    draw_text(detail_x + 24, render_line_y, num_str, 0x003b4d61u);

                    char trunc_code[64];
                    truncate_text_to_width(line_temp, trunc_code, 200);
                    draw_text(detail_x + 44, render_line_y, trunc_code, 0x00cbd5e1u);

                    render_line_y += 18;
                    curr_num++;
                }
            }
        }
    } else {
        // Empty selection details state
        draw_text(detail_x + 60, detail_y + 120, "Select an item to view properties", 0x005c6a7eu);
    }

    // 7b. Dialog action bar band (y: 568..616). The filename field and the
    //     Open/Save + Cancel buttons are real VexUI widgets painted on top of
    //     this band (created in build_widgets()); here we only draw the backdrop,
    //     its top divider and — in SAVE mode — the "Name:" caption.
    if (dialog_mode_) {
        int bar_y = content_bottom();
        fill_rect(0, bar_y, win_w_, DIALOG_BAR_H, 0x001b3147u);
        draw_line(0, bar_y, win_w_, bar_y, 0x00294058u);
        if (dialog_save_)
            draw_text(232, bar_y + 17, "Name:", 0x009aa7bau);
    }

    // 8. Bottom Status Bar (status_y .. win_h_)
    int status_y = win_h_ - STATUS_BAR_H;
    fill_rect(0, status_y, win_w_, STATUS_BAR_H, 0x0020384fu);
    draw_line(0, status_y, win_w_, status_y, 0x00294058u);

    // Left status summary
    char status_left[128];
    if (selected_index_ >= 0) {
        int act_idx = filtered_indices_[selected_index_];
        FileEntry &fe = entries_[act_idx];
        if (fe.kind == 2) {
            snprintf(status_left, sizeof(status_left), "%d items | 1 item selected", filtered_count_);
        } else {
            snprintf(status_left, sizeof(status_left), "%d items | 1 item selected (%s)", filtered_count_, fe.formatted_size);
        }
    } else {
        snprintf(status_left, sizeof(status_left), "%d items", filtered_count_);
    }
    draw_text(16, status_y + 5, status_left, 0x009aa7bau);

    // Right status metrics & progress bar
    char status_right[64];
    snprintf(status_right, sizeof(status_right), "42.8 GB free of 128 GB");
    int size_text_w = get_text_width(status_right);
    int label_x = win_w_ - size_text_w - 130;
    draw_text(label_x, status_y + 5, status_right, 0x009aa7bau);

    // Draw nice sleek progress bar on the right side
    int bar_x = win_w_ - 116;
    int bar_y = status_y + 8;
    int bar_w = 100;
    int bar_h = 8;
    
    // Track background
    fill_rect(bar_x, bar_y, bar_w, bar_h, 0x001a2636u);
    
    // Filled progress pill (42.8 free => 85.2 used => 66% progress)
    int fill_w = (bar_w * 66) / 100;
    fill_rect(bar_x, bar_y, fill_w, bar_h, 0x003b82f6u);

    // ---- Right-click context menu overlay ----
    if (context_menu_visible_ && context_item_idx_ >= 0) {
        int mx = context_menu_x_, my = context_menu_y_;
        int mw = 160, mh = 108; /* 3 items + padding */
        /* Clamp to window bounds. */
        if (mx + mw > win_w_) mx = win_w_ - mw - 4;
        if (my + mh > win_h_) my = win_h_ - mh - 4;
        if (mx < 4) mx = 4;
        if (my < 4) my = 4;

        fill_rect(mx, my, mw, mh, 0x001b3147u);
        draw_rect(mx, my, mw, mh, 0x003b82f6u);

        int item_y = my + 6;
        draw_text(mx + 12, item_y + 3, "Open", 0x00e6edf7u);    item_y += 32;
        draw_text(mx + 12, item_y + 3, "Rename", 0x00e6edf7u);  item_y += 32;
        draw_text(mx + 12, item_y + 3, "Delete", 0x00f87171u);

        /* Store menu item positions for hit-testing (in on_click we check
         * against these when context_menu_visible_ is true). */
        context_menu_x_ = mx;
        context_menu_y_ = my;
    }
}

// Canvas click dispatch. The toolbar and sidebar are handled by their own VexUI
// widget callbacks; this only covers the hand-rendered file list + scrollbar.
void FileBrowser::on_click(int x, int y, uint32_t buttons) {
    bool right_click = (buttons & 2u) != 0;

    // ---- Dismiss context menu on any click outside its bounds ----
    if (context_menu_visible_) {
        int mx = context_menu_x_, my = context_menu_y_;
        int mw = 160, mh = 108;
        if (x >= mx && x <= mx + mw && y >= my && y <= my + mh) {
            // Click inside menu — which item?
            int rel_y = y - my;
            int item = rel_y / 32; /* 0=Open, 1=Rename, 2=Delete */
            context_menu_visible_ = false;
            if (context_item_idx_ >= 0 && context_item_idx_ < filtered_count_) {
                int act_idx = filtered_indices_[context_item_idx_];
                FileEntry &fe = entries_[act_idx];
                char target_path[512];
                if (strcmp(current_path_, "/") == 0)
                    snprintf(target_path, sizeof(target_path), "/%s", fe.name);
                else
                    snprintf(target_path, sizeof(target_path), "%s/%s", current_path_, fe.name);

                if (item == 0) { /* Open */
                    if (fe.kind == 2) navigate(target_path, true);
                    else vos_spawn_arg("/bin/edit", target_path);
                } else if (item == 1) { /* Rename */
                    /* TODO: in-place rename UI */
                } else if (item == 2) { /* Delete */
                    unlink(target_path);
                    refresh_files();
                    selected_index_ = -1;
                    update_preview();
                }
            }
            render();
            vui_request_repaint(win_);
            return;
        }
        context_menu_visible_ = false;
        render();
        vui_request_repaint(win_);
        if (right_click) return; /* right-click outside menu = dismiss only */
    }

    // ---- Breadcrumb navigation ----
    int bc_y = 55;
    int bc_h = 20;
    for (int i = 0; i < breadcrumb_count_; ++i) {
        if (x >= breadcrumb_x_[i] && x <= breadcrumb_x_[i] + breadcrumb_w_[i] &&
            y >= bc_y && y <= bc_y + bc_h) {
            navigate(breadcrumb_path_[i], true);
            return;
        }
    }

    // Check Main Listing Area Clicks & Scrollbar Grab
    int list_x, list_y, list_w, list_h;
    get_listing_rect(list_x, list_y, list_w, list_h);

    if (x >= list_x && x <= list_x + list_w && y >= list_y && y <= list_y + list_h) {
        // Check Scrollbar Track grab first
        int sb_col_x = list_x + list_w - 12;
        if (x >= sb_col_x && x <= sb_col_x + 8) {
            // ... scrollbar handling (unchanged) ...
            int total_rows = (filtered_count_ + (grid_view_ ? 3 : 0)) / (grid_view_ ? 4 : 1);
            int item_h = grid_view_ ? 96 : 36;
            int content_h = total_rows * item_h;
            int visible_h = list_h - (grid_view_ ? 0 : 36);
            if (content_h > visible_h) {
                int sb_h = list_h;
                int thumb_h = (sb_h * visible_h) / content_h;
                if (thumb_h < 20) thumb_h = 20;
                int thumb_y = list_y + (scroll_y_ * (sb_h - thumb_h)) / (content_h - visible_h);

                if (y >= thumb_y && y <= thumb_y + thumb_h) {
                    scroll_dragging_ = true;
                    scroll_drag_start_y_ = y;
                    scroll_drag_start_scroll_ = scroll_y_;
                } else {
                    int target_scroll = ((y - list_y - thumb_h / 2) * (content_h - visible_h)) / (sb_h - thumb_h);
                    scroll_y_ = target_scroll;
                    if (scroll_y_ < 0) scroll_y_ = 0;
                    if (scroll_y_ > max_scroll_y_) scroll_y_ = max_scroll_y_;
                    render();
                }
            }
            return;
        }

        // ---- Right-click context menu on file ----
        if (right_click) {
            int clicked_item = -1;
            if (!grid_view_) {
                int start_row_y = list_y + 36;
                clicked_item = (y - start_row_y) / 36 + (scroll_y_ / 36);
            } else {
                int row = (y - list_y - 16) / 96 + (scroll_y_ / 96);
                int col = (x - list_x - 16) / 108;
                if (col >= 0 && col < 4) clicked_item = row * 4 + col;
            }
            if (clicked_item >= 0 && clicked_item < filtered_count_) {
                selected_index_ = clicked_item;
                update_preview();
                context_menu_visible_ = true;
                context_menu_x_ = x;
                context_menu_y_ = y;
                context_item_idx_ = clicked_item;
                render();
                vui_request_repaint(win_);
            }
            return;
        }

        // List Item Clicks (unchanged)
        if (!grid_view_) {
            int start_row_y = list_y + 36;
            int clicked_item = start_row_y ? (y - start_row_y) / 36 + (scroll_y_ / 36) : -1;
            if (clicked_item >= 0 && clicked_item < filtered_count_) {
                if (clicked_item == selected_index_) {
                    int act_idx = filtered_indices_[clicked_item];
                    FileEntry &fe = entries_[act_idx];

                    char target_path[512];
                    if (strcmp(current_path_, "/") == 0)
                        snprintf(target_path, sizeof(target_path), "/%s", fe.name);
                    else
                        snprintf(target_path, sizeof(target_path), "%s/%s", current_path_, fe.name);

                    if (fe.kind == 2) navigate(target_path, true);
                    else if (dialog_mode_) {
                        if (dialog_save_) { if (filename_input_) vui_set_text(filename_input_, fe.name); }
                        else dialog_finish(target_path);
                    } else {
                        const char *ext = strrchr(fe.name, '.');
                        if (ext && strcmp(ext, ".mp3") == 0) vos_spawn_arg("/bin/audioplayer", target_path);
                        else vos_spawn_arg("/bin/edit", target_path);
                    }
                } else {
                    selected_index_ = clicked_item;
                    if (dialog_mode_ && dialog_save_ && filename_input_ &&
                        entries_[filtered_indices_[clicked_item]].kind != 2)
                        vui_set_text(filename_input_, entries_[filtered_indices_[clicked_item]].name);
                    update_preview();
                    render();
                }
            }
        } else {
            // Grid Clicks
            int row = (y - list_y - 16) / 96 + (scroll_y_ / 96);
            int col = (x - list_x - 16) / 108;
            if (col >= 0 && col < 4) {
                int clicked_item = row * 4 + col;
                if (clicked_item >= 0 && clicked_item < filtered_count_) {
                    if (clicked_item == selected_index_) {
                        int act_idx = filtered_indices_[clicked_item];
                        FileEntry &fe = entries_[act_idx];

                        char target_path[512];
                        if (strcmp(current_path_, "/") == 0)
                            snprintf(target_path, sizeof(target_path), "/%s", fe.name);
                        else
                            snprintf(target_path, sizeof(target_path), "%s/%s", current_path_, fe.name);

                        if (fe.kind == 2) navigate(target_path, true);
                        else if (dialog_mode_) {
                            if (dialog_save_) { if (filename_input_) vui_set_text(filename_input_, fe.name); }
                            else dialog_finish(target_path);
                        } else {
                            const char *ext = strrchr(fe.name, '.');
                            if (ext && strcmp(ext, ".mp3") == 0) vos_spawn_arg("/bin/audioplayer", target_path);
                            else vos_spawn_arg("/bin/edit", target_path);
                        }
                    } else {
                        selected_index_ = clicked_item;
                        if (dialog_mode_ && dialog_save_ && filename_input_ &&
                            entries_[filtered_indices_[clicked_item]].kind != 2)
                            vui_set_text(filename_input_, entries_[filtered_indices_[clicked_item]].name);
                        update_preview();
                        render();
                    }
                }
            }
        }
        return;
    }

    // ---- Preview toggle (detail panel header click) ----
    int detail_x, detail_y, detail_w, detail_h;
    get_detail_rect(detail_x, detail_y, detail_w, detail_h);
    if (has_preview_ && selected_index_ >= 0 &&
        x >= detail_x && x <= detail_x + detail_w) {
        int prev_y = detail_y + 270; /* approximate: tags_y + 32 after metadata */
        if (y >= prev_y && y <= prev_y + 20) {
            preview_expanded_ = !preview_expanded_;
            render();
            vui_request_repaint(win_);
            return;
        }
    }
}

void FileBrowser::on_mouse_move(int x, int y) {
    bool dirty = false;

    // A. Hover state check for scrollbar thumb
    int list_x, list_y, list_w, list_h;
    get_listing_rect(list_x, list_y, list_w, list_h);
    int sb_col_x = list_x + list_w - 12;
    
    bool old_hover = scroll_hovered_;
    scroll_hovered_ = (x >= sb_col_x && x <= sb_col_x + 8 && y >= list_y && y <= list_y + list_h);
    if (scroll_hovered_ != old_hover) dirty = true;

    // B. Handle dragging of scrollbar thumb
    if (scroll_dragging_) {
        int sb_h = list_h - 8;
        
        int items_h = grid_view_ ? 96 : 36;
        int total_rows = (filtered_count_ + (grid_view_ ? 3 : 0)) / (grid_view_ ? 4 : 1);
        int content_h = total_rows * items_h;
        int visible_h = list_h - (grid_view_ ? 0 : 36);
        
        if (content_h > visible_h) {
            int thumb_h = (sb_h * visible_h) / content_h;
            if (thumb_h < 20) thumb_h = 20;
            
            int dy = y - scroll_drag_start_y_;
            int delta_scroll = (dy * (content_h - visible_h)) / (sb_h - thumb_h);
            
            int new_scroll = scroll_drag_start_scroll_ + delta_scroll;
            if (new_scroll < 0) new_scroll = 0;
            if (new_scroll > max_scroll_y_) new_scroll = max_scroll_y_;
            
            if (scroll_y_ != new_scroll) {
                scroll_y_ = new_scroll;
                dirty = true;
            }
        }
    }

    if (dirty) {
        render();
        vui_request_repaint(win_);
    }
}

void FileBrowser::on_mouse_release(int x, int y, uint32_t /*buttons*/) {
    (void)x; (void)y;
    if (scroll_dragging_) {
        scroll_dragging_ = false;
        render();
        vui_request_repaint(win_);
    }
}

void FileBrowser::on_key(uint32_t key) {
    // Keyboard input for the search field is delivered to the focused VexUI
    // input widget, not here. Kept as a hook for future shortcuts.
    (void)key;
}

void FileBrowser::on_scroll(int delta) {
    int old_scroll = scroll_y_;
    
    // Scroll speed modifier (3 rows / units per notch)
    scroll_y_ -= delta * (grid_view_ ? 48 : 18);
    
    if (scroll_y_ < 0) scroll_y_ = 0;
    if (scroll_y_ > max_scroll_y_) scroll_y_ = max_scroll_y_;
    
    if (scroll_y_ != old_scroll) {
        render();
        vui_request_repaint(win_);
    }
}

void FileBrowser::on_resize(int w, int h) {
    if (w < 400) w = 400;
    if (h < 300) h = 300;
    if (w > BROWSER_MAX_W) w = BROWSER_MAX_W;
    if (h > BROWSER_MAX_H) h = BROWSER_MAX_H;

    win_w_ = w;
    win_h_ = h;
    
    refresh_files();
    render();
    vui_request_repaint(win_);
}

void FileBrowser::on_tick() {
    // Currently idle tick handler, can be used for animations later
}

// ---------------------------------------------------------------------------
// VexUI event-callback adapters
//
// VexUI uses C function-pointer callbacks. These thin adapters forward each
// event to the singleton FileBrowser instance so all real logic stays inside
// the class (clean separation between framework glue and application code).
// ---------------------------------------------------------------------------
static void fb_on_click_cb(vui_window *w, int x, int y, vui_u32 buttons) {
    (void)w;
    FileBrowser::instance()->on_click(x, y, buttons);
}

static void fb_on_mouse_move_cb(vui_window *w, int x, int y, vui_u32 /*buttons*/) {
    (void)w;
    FileBrowser::instance()->on_mouse_move(x, y);
}

static void fb_on_mouse_release_cb(vui_window *w, int x, int y, vui_u32 buttons) {
    (void)w;
    FileBrowser::instance()->on_mouse_release(x, y, buttons);
}

static void fb_on_scroll_cb(vui_window *w, int delta) {
    (void)w;
    FileBrowser::instance()->on_scroll(delta);
}

static void fb_on_resize_cb(vui_window *w, int width, int height) {
    (void)w;
    FileBrowser::instance()->on_resize(width, height);
}

static void fb_on_tick_cb(vui_window *w) {
    (void)w;
    FileBrowser::instance()->on_tick();
}

// ---------------------------------------------------------------------------
// Toolbar / sidebar widget callbacks. Each forwards to the singleton instance;
// sidebar rows carry their sidebar_items_ index in the widget's user pointer.
// ---------------------------------------------------------------------------
static void fb_nav_back_cb(vui_widget *)    { FileBrowser::instance()->nav_back(); }
static void fb_nav_fwd_cb(vui_widget *)     { FileBrowser::instance()->nav_forward(); }
static void fb_nav_up_cb(vui_widget *)      { FileBrowser::instance()->nav_up(); }
static void fb_refresh_cb(vui_widget *)     { FileBrowser::instance()->do_refresh(); }
static void fb_newfolder_cb(vui_widget *)   { FileBrowser::instance()->create_new_folder(); }
static void fb_view_list_cb(vui_widget *)   { FileBrowser::instance()->set_grid_view(false); }
static void fb_view_grid_cb(vui_widget *)   { FileBrowser::instance()->set_grid_view(true); }
static void fb_search_cb(vui_widget *self)  { FileBrowser::instance()->search_changed(vui_input_text(self)); }
static void fb_sidebar_cb(vui_widget *self) {
    FileBrowser::instance()->select_sidebar((int)(intptr_t)vui_get_user(self));
}
static void fb_dialog_accept_cb(vui_widget *) { FileBrowser::instance()->dialog_accept(); }
static void fb_dialog_cancel_cb(vui_widget *) { FileBrowser::instance()->dialog_cancel(); }

// ---------------------------------------------------------------------------
// Toolbar + sidebar construction. These are real VexUI controls layered on top
// of the full-window canvas (which still renders the list, preview and chrome).
// ---------------------------------------------------------------------------
void FileBrowser::build_widgets() {
    // --- Toolbar: flat, chrome-free navigation icons (like the mockup) ---
    struct ToolBtn { int x; const char *icon; vui_callback cb; };
    const ToolBtn tools[] = {
        { TB_NAV_X + 0 * TB_NAV_STEP, SVG_ARROW_LEFT,  fb_nav_back_cb },
        { TB_NAV_X + 1 * TB_NAV_STEP, SVG_ARROW_RIGHT, fb_nav_fwd_cb },
        { TB_NAV_X + 2 * TB_NAV_STEP, SVG_ARROW_UP,    fb_nav_up_cb },
        { TB_NAV_X + 3 * TB_NAV_STEP, SVG_REFRESH,     fb_refresh_cb },
    };
    for (const ToolBtn &t : tools) {
        vui_widget *b = vui_image(win_, t.x, 9, 30);
        vui_set_color(b, 0x00cdd8e6u);
        vui_set_icon_svg(b, t.icon);
        vui_on_click(b, t.cb);
    }

    // "New Folder" labelled button with a leading plus icon. A neutral slate
    // tint (instead of the default accent) keeps it flat like the mockup.
    vui_widget *newf = vui_button(win_, TB_NEWFOLDER_X, TB_ROW_Y, "New Folder");
    vui_set_bounds(newf, TB_NEWFOLDER_X, TB_ROW_Y, TB_NEWFOLDER_W, TB_ROW_H);
    vui_set_color(newf, 0x009fb4ccu);
    vui_set_icon_svg(newf, SVG_PLUS);
    vui_on_click(newf, fb_newfolder_cb);

    // List / grid segmented toggle: two flat icons over a canvas-drawn pill
    // whose active segment is highlighted in render() (driven by grid_view_).
    int tg_iy = TB_ROW_Y + (TB_ROW_H - 24) / 2;
    vui_widget *vlist = vui_image(win_, TB_TOGGLE_X + (TB_TOGGLE_SEG - 24) / 2, tg_iy, 24);
    vui_set_color(vlist, 0x00e6edf7u);
    vui_set_icon_svg(vlist, SVG_LIST_VIEW);
    vui_on_click(vlist, fb_view_list_cb);

    vui_widget *vgrid = vui_image(win_, TB_TOGGLE_X + TB_TOGGLE_SEG + (TB_TOGGLE_SEG - 24) / 2, tg_iy, 24);
    vui_set_color(vgrid, 0x00e6edf7u);
    vui_set_icon_svg(vgrid, SVG_GRID_VIEW);
    vui_on_click(vgrid, fb_view_grid_cb);

    // Search field, right-aligned and stretching with the window.
    int search_w = 280;
    int search_x = win_w_ - search_w - 16;
    search_input_ = vui_input(win_, search_x, TB_ROW_Y, search_w, "Search Assets...");
    vui_set_icon_svg(search_input_, SVG_SEARCH);
    vui_set_anchor(search_input_, VUI_ANCHOR_TOP | VUI_ANCHOR_RIGHT);
    vui_on_click(search_input_, fb_search_cb);   /* fires per keystroke */

    // --- Sidebar: one list-item widget per non-header entry ---
    for (int i = 0; i < sidebar_count_; ++i) {
        if (sidebar_items_[i].is_header) continue;
        int item_y = 90 + i * 32;
        vui_widget *row = vui_listitem(win_, 10, item_y, 200, 28, sidebar_items_[i].label);
        vui_set_icon_svg(row, sidebar_items_[i].svg_icon);
        vui_set_user(row, (void *)(intptr_t)i);
        vui_on_click(row, fb_sidebar_cb);
        sidebar_widgets_[i] = row;
    }
    sync_sidebar_selection();

    // --- Dialog action bar: filename field (SAVE) + accept/cancel buttons ---
    if (dialog_mode_) {
        int bar_y   = content_bottom();
        int btn_h   = 28;
        int btn_w   = 88;
        int btn_y   = bar_y + (DIALOG_BAR_H - btn_h) / 2;
        int cancel_x = win_w_ - 16 - btn_w;
        int accept_x = cancel_x - 8 - btn_w;

        cancel_btn_ = vui_button(win_, cancel_x, btn_y, "Cancel");
        vui_set_bounds(cancel_btn_, cancel_x, btn_y, btn_w, btn_h);
        vui_set_color(cancel_btn_, 0x009fb4ccu);
        vui_set_anchor(cancel_btn_, VUI_ANCHOR_RIGHT | VUI_ANCHOR_BOTTOM);
        vui_on_click(cancel_btn_, fb_dialog_cancel_cb);

        accept_btn_ = vui_button(win_, accept_x, btn_y, dialog_save_ ? "Save" : "Open");
        vui_set_bounds(accept_btn_, accept_x, btn_y, btn_w, btn_h);
        vui_set_anchor(accept_btn_, VUI_ANCHOR_RIGHT | VUI_ANCHOR_BOTTOM);
        vui_on_click(accept_btn_, fb_dialog_accept_cb);

        if (dialog_save_) {
            // "Name:" caption is painted on the canvas (render); the field starts
            // just right of it and stretches up to the accept button.
            int name_x = 280;
            int name_w = accept_x - 16 - name_x;
            if (name_w < 120) name_w = 120;
            filename_input_ = vui_input(win_, name_x, btn_y, name_w, "filename.txt");
            vui_set_anchor(filename_input_, VUI_ANCHOR_LEFT | VUI_ANCHOR_RIGHT | VUI_ANCHOR_BOTTOM);
        }
    }
}

// Reflect the active directory on the sidebar rows (highlighted selection).
void FileBrowser::sync_sidebar_selection() {
    for (int i = 0; i < sidebar_count_; ++i) {
        if (sidebar_widgets_[i]) {
            vui_set_running(sidebar_widgets_[i], i == selected_sidebar_idx_ ? 1 : 0);
        }
    }
}

// ---- Toolbar / sidebar actions ------------------------------------------- //
void FileBrowser::nav_back() {
    if (history_pos_ > 0) {
        history_pos_--;
        navigate(history_[history_pos_], false);
    }
}

void FileBrowser::nav_forward() {
    if (history_pos_ < history_count_ - 1) {
        history_pos_++;
        navigate(history_[history_pos_], false);
    }
}

void FileBrowser::nav_up() {
    if (strcmp(current_path_, "/") == 0) return;
    char parent[256];
    strcpy(parent, current_path_);
    int len = strlen(parent);
    while (len > 1 && parent[len - 1] != '/') { parent[--len] = '\0'; }
    if (len > 1 && parent[len - 1] == '/') parent[len - 1] = '\0';
    navigate(parent, true);
}

void FileBrowser::do_refresh() {
    refresh_files();
    render();
    vui_request_repaint(win_);
}

void FileBrowser::set_grid_view(bool grid) {
    if (grid_view_ == grid) return;
    grid_view_ = grid;
    scroll_y_ = 0;
    refresh_files();
    render();
    vui_request_repaint(win_);
}

void FileBrowser::select_sidebar(int index) {
    if (index < 0 || index >= sidebar_count_) return;
    selected_sidebar_idx_ = index;
    navigate(sidebar_items_[index].path, true);   // also syncs + repaints
}

void FileBrowser::search_changed(const char *text) {
    strncpy(search_query_, text ? text : "", sizeof(search_query_) - 1);
    search_query_[sizeof(search_query_) - 1] = '\0';
    refresh_files();
    render();
    vui_request_repaint(win_);
}

// ---------------------------------------------------------------------------
// Application entry point: open the window, build the canvas + widgets, wire
// up event callbacks, then hand control to the VexUI loop (which never returns).
// ---------------------------------------------------------------------------
void FileBrowser::run() {
    instance_ = this;
    canvas_pixels_ = g_canvas_pixels;

    if (dialog_mode_) {
        // As a file picker, open a shorter, centred window biased toward the top
        // of the screen so the action bar at its bottom clears the always-on-top
        // dock (which floats over the lower screen edge).
        uint32_t mode = vos_display_mode_get();
        int sw = (int)((mode >> 16) & 0xffffu);
        int sh = (int)(mode & 0xffffu);
        if (sw <= 0) sw = 1024;
        if (sh <= 0) sh = 768;
        int dw = 900, dh = 500;
        if (dw > sw - 40) dw = sw - 40;
        if (dh > sh - 200) dh = sh - 200;
        int dx = (sw - dw) / 2;
        int dy = (sh - dh) / 2 - 50;        // lift away from the dock
        if (dy < 40) dy = 40;
        win_w_ = dw;
        win_h_ = dh;
        win_ = vui_window_open_ex(dialog_save_ ? "Save File" : "Open File",
                                  win_w_, win_h_, VUI_WINDOW_POSITIONED, dx, dy);
    } else {
        win_ = vui_window_open("Files", win_w_, win_h_);
    }
    if (!win_) return;

    // Adopt the real (possibly clamped) window size before laying anything out.
    win_w_ = vui_window_width(win_);
    win_h_ = vui_window_height(win_);
    if (win_w_ > BROWSER_MAX_W) win_w_ = BROWSER_MAX_W;
    if (win_h_ > BROWSER_MAX_H) win_h_ = BROWSER_MAX_H;

    // A full-window canvas hosts the hand-rendered file list, breadcrumbs,
    // preview and status bar. Created first so the toolbar/sidebar widgets,
    // added afterwards, paint on top of it.
    vui_widget *canvas = vui_canvas_ex(win_, 0, 0, win_w_, win_h_,
                                       canvas_pixels_, BROWSER_MAX_W);
    vui_set_anchor(canvas, VUI_ANCHOR_LEFT | VUI_ANCHOR_RIGHT |
                           VUI_ANCHOR_TOP | VUI_ANCHOR_BOTTOM);

    build_widgets();

    // Canvas-region input (file rows, scrollbar, wheel) routes to the instance.
    vui_on_mouse_click(win_, fb_on_click_cb);
    vui_on_mouse_move(win_, fb_on_mouse_move_cb);
    vui_on_mouse_release(win_, fb_on_mouse_release_cb);
    vui_on_scroll(win_, fb_on_scroll_cb);
    vui_on_resize(win_, fb_on_resize_cb);
    vui_on_tick(win_, fb_on_tick_cb);

    // Paint the initial frame and enter the (non-returning) event loop.
    render();
    vui_request_repaint(win_);
    vui_run(win_);
}
