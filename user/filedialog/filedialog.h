/*
 * filedialog/filedialog.h — Standard File Dialog (Open/Save) for VibeOS.
 *
 * This utility provides a reusable dialog for selecting or naming files.
 * It is launched as a separate process to maintain isolation.
 *
 * Clean code, English comments, VibeOS state-of-the-art.
 */

#ifndef FILEDIALOG_H
#define FILEDIALOG_H

#include "vexui.h"
#include <stdint.h>
#include <stddef.h>

struct FileEntry {
    char name[128];
    uint64_t kind;  // 1 = Regular File, 2 = Directory
    uint64_t size;
};

class FileDialog {
public:
    static FileDialog *instance() { return instance_; }

    FileDialog();
    ~FileDialog();

    void init(const char *title, const char *initial_path, bool save_mode, const char *result_file);
    void run();

private:
    static FileDialog *instance_;

    // VexUI state
    vui_window *win_ = nullptr;
    int win_w_ = 520;
    int win_h_ = 420;

    // Configuration
    char title_[64];
    char current_path_[256];
    char result_file_[128];
    bool save_mode_ = false;

    // UI Widgets
    vui_widget *path_label_ = nullptr;
    vui_widget *file_list_ = nullptr; // We'll use a vbox of listitems
    vui_widget *filename_input_ = nullptr;
    vui_widget *action_button_ = nullptr;
    vui_widget *list_container_ = nullptr;

    // File listing
    FileEntry entries_[64];
    int entry_count_ = 0;
    int selected_entry_ = -1;

    // Internal logic
    void refresh_files();
    void navigate(const char *path);
    void on_entry_clicked(int index);
    void on_action();
    void on_cancel();
    void finish(const char *result_path);

    // Static callbacks for VexUI
    static void cb_action(vui_widget *w) { instance_->on_action(); }
    static void cb_cancel(vui_widget *w) { instance_->on_cancel(); }
    static void cb_up(vui_widget *w);
};

#endif
