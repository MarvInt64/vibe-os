/*
 * filedialog/filedialog.cpp — Standard File Dialog implementation.
 */

#include "filedialog.h"
#include <vibeos.h>
#include <sys/syscall.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* System call numbers (if not already defined in libc headers) */
#ifndef SYS_READDIR
#define SYS_READDIR 10
#endif
#ifndef SYS_WRITE_FILE
#define SYS_WRITE_FILE 16
#endif

FileDialog *FileDialog::instance_ = nullptr;

/* Helper: retrieve directory entry with kind/size info in one pass. */
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

FileDialog::FileDialog() {
    instance_ = this;
    title_[0] = '\0';
    current_path_[0] = '\0';
}

FileDialog::~FileDialog() {
    instance_ = nullptr;
}

void FileDialog::init(const char *title, const char *initial_path, bool save_mode, const char *result_file) {
    strncpy(title_, title ? title : "Select File", sizeof(title_) - 1);
    strncpy(current_path_, initial_path ? initial_path : "/", sizeof(current_path_) - 1);
    strncpy(result_file_, result_file ? result_file : "/tmp/fd_result", sizeof(result_file_) - 1);
    save_mode_ = save_mode;
}

void FileDialog::run() {
    win_ = vui_window_open_ex(title_, win_w_, win_h_, VUI_WINDOW_POSITIONED, 200, 150);
    
    /* Layout:
     * [ Path: /home/user/        ] [ Up ]
     * [---------------------------------]
     * [ Folder A                       ]
     * [ File B                         ]
     * [ ...                            ]
     * [---------------------------------]
     * [ Name: [ filename.txt ]         ]
     * [              [ Cancel ] [ Save ] ]
     */

    vui_widget *root = vui_vbox(win_, 0, 0, win_w_, win_h_);
    vui_set_padding(root, 12);
    vui_set_gap(root, 10);

    /* Header: Path + Up button */
    vui_widget *header = vui_hbox(win_, 0, 0, 0, 32);
    vui_box_add(root, header);
    vui_set_fill(header);
    
    path_label_ = vui_label(win_, 0, 0, current_path_);
    vui_box_add(header, path_label_);
    vui_set_expand(path_label_);
    
    vui_widget *up_btn = vui_button(win_, 0, 0, "Up");
    vui_box_add(header, up_btn);
    vui_on_click(up_btn, cb_up);

    /* Main Area: File List */
    vui_widget *list_panel = vui_panel(win_, 0, 0, 0, 0, "");
    vui_box_add(root, list_panel);
    vui_set_expand(list_panel);
    vui_set_fill(list_panel);
    
    list_container_ = vui_vbox(win_, 0, 0, 0, 0);
    vui_box_add(list_panel, list_container_);
    vui_set_padding(list_container_, 4);
    vui_set_gap(list_container_, 2);
    vui_set_fill(list_container_);
    vui_set_expand(list_container_);

    /* Footer: Input + Buttons */
    vui_widget *footer = vui_vbox(win_, 0, 0, 0, 0);
    vui_box_add(root, footer);
    vui_set_fill(footer);
    vui_set_gap(footer, 8);

    if (save_mode_) {
        vui_widget *name_row = vui_hbox(win_, 0, 0, 0, 32);
        vui_box_add(footer, name_row);
        vui_set_fill(name_row);
        vui_set_gap(name_row, 8);
        
        vui_label(win_, 0, 0, "Name:");
        filename_input_ = vui_input(win_, 0, 0, 0, "filename.txt");
        vui_box_add(name_row, filename_input_);
        vui_set_expand(filename_input_);
    }

    vui_widget *btn_row = vui_hbox(win_, 0, 0, 0, 32);
    vui_box_add(footer, btn_row);
    vui_set_fill(btn_row);
    vui_set_gap(btn_row, 8);
    
    /* Spacer to push buttons to the right */
    vui_widget *spacer = vui_label(win_, 0, 0, "");
    vui_box_add(btn_row, spacer);
    vui_set_expand(spacer);

    vui_widget *cancel_btn = vui_button(win_, 0, 0, "Cancel");
    vui_box_add(btn_row, cancel_btn);
    vui_on_click(cancel_btn, cb_cancel);

    action_button_ = vui_button(win_, 0, 0, save_mode_ ? "Save" : "Open");
    vui_box_add(btn_row, action_button_);
    vui_on_click(action_button_, cb_action);

    refresh_files();
    vui_run(win_);
}

void FileDialog::refresh_files() {
    entry_count_ = 0;
    char name[128];
    uint64_t kind, size;

    /* Read up to 64 entries from current directory */
    for (uint32_t i = 0; i < 64; ++i) {
        int res = readdir_entry_ex(current_path_, i, name, sizeof(name), &kind, &size);
        if (res <= 0) break;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;

        strncpy(entries_[entry_count_].name, name, 127);
        entries_[entry_count_].kind = kind;
        entries_[entry_count_].size = size;
        entry_count_++;
    }

    /* Clear and rebuild the list widgets */
    /* VexUI doesn't support removing widgets easily, so we reuse listitem slots
     * or hide unused ones. For simplicity in this 'state-of-the-art' demo,
     * we'll assume the list container is cleared or we just update the first N.
     * Note: libvexui is currently a bit static in widget management.
     * I'll implement a clean way to update labels. */
    
    /* Actually, since we can't delete widgets, we'll allocate a fixed pool
     * of listitems in init() and just update their text/visibility here. */
    static vui_widget *list_pool[64] = {nullptr};
    static bool pool_inited = false;
    
    if (!pool_inited) {
        for (int i = 0; i < 64; ++i) {
            list_pool[i] = vui_listitem(win_, 0, 0, 0, 24, "");
            vui_box_add(list_container_, list_pool[i]);
            vui_set_fill(list_pool[i]);
            vui_set_visible(list_pool[i], 0);
            
            /* Capture index in user data */
            vui_set_user(list_pool[i], (void*)(size_t)i);
            vui_on_click(list_pool[i], [](vui_widget *w){
                int idx = (int)(size_t)vui_get_user(w);
                FileDialog::instance()->on_entry_clicked(idx);
            });
        }
        pool_inited = true;
    }

    for (int i = 0; i < 64; ++i) {
        if (i < entry_count_) {
            char display[140];
            if (entries_[i].kind == 2) snprintf(display, sizeof(display), "[DIR] %s", entries_[i].name);
            else snprintf(display, sizeof(display), "%s", entries_[i].name);
            
            vui_set_text(list_pool[i], display);
            vui_set_visible(list_pool[i], 1);
            vui_set_running(list_pool[i], (i == selected_entry_));
        } else {
            vui_set_visible(list_pool[i], 0);
        }
    }

    vui_set_text(path_label_, current_path_);
}

void FileDialog::navigate(const char *path) {
    strncpy(current_path_, path, sizeof(current_path_) - 1);
    selected_entry_ = -1;
    refresh_files();
}

void FileDialog::on_entry_clicked(int index) {
    if (index < 0 || index >= entry_count_) return;
    
    if (entries_[index].kind == 2) {
        /* Navigate into folder */
        char next[512];
        if (strcmp(current_path_, "/") == 0) snprintf(next, sizeof(next), "/%s", entries_[index].name);
        else snprintf(next, sizeof(next), "%s/%s", current_path_, entries_[index].name);
        navigate(next);
    } else {
        /* Select file */
        selected_entry_ = index;
        if (save_mode_ && filename_input_) {
            vui_set_text(filename_input_, entries_[index].name);
        }
        refresh_files();
    }
}

void FileDialog::on_action() {
    char final_path[512];
    if (save_mode_) {
        const char *name = vui_input_text(filename_input_);
        if (!name || !name[0]) return;
        if (strcmp(current_path_, "/") == 0) snprintf(final_path, sizeof(final_path), "/%s", name);
        else snprintf(final_path, sizeof(final_path), "%s/%s", current_path_, name);
    } else {
        if (selected_entry_ < 0) return;
        if (strcmp(current_path_, "/") == 0) snprintf(final_path, sizeof(final_path), "/%s", entries_[selected_entry_].name);
        else snprintf(final_path, sizeof(final_path), "%s/%s", current_path_, entries_[selected_entry_].name);
    }
    finish(final_path);
}

void FileDialog::on_cancel() {
    exit(1);
}

void FileDialog::finish(const char *result_path) {
    /* Write result to the temporary file passed via arg. */
    /* Using SYS_WRITE_FILE (atomic write-all) for simplicity */
    __sc3(SYS_WRITE_FILE, (uint64_t)(size_t)result_file_, (uint64_t)(size_t)result_path, (uint64_t)strlen(result_path));
    
    exit(0);
}

void FileDialog::cb_up(vui_widget *w) {
    char *path = instance_->current_path_;
    if (strcmp(path, "/") == 0) return;
    
    char *last = strrchr(path, '/');
    if (last == path) strcpy(path, "/");
    else if (last) *last = '\0';
    
    instance_->selected_entry_ = -1;
    instance_->refresh_files();
}
