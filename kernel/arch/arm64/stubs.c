/* VibeOS arm64 — Stubs for shared kernel code (window.c, etc.).
 *
 * Provides minimal implementations so window.c and other shared sources
 * compile on arm64.  Many functions are no-ops; real implementations can
 * replace them incrementally.
 */
#include "arch.h"
#include "../../include/alloc.h"
#include "../../include/ext2_fs.h"
#include "../../include/process.h"
#include "../../include/serial.h"
#include "../../include/string.h"
#include "../../include/timer.h"
#include "../../include/types.h"
#include "../../include/vfs.h"
#include "../../include/journal.h"

/* ---- Globals from arch.c ---------------------------------------------- */
extern struct ext2_filesystem g_fs;
extern int g_fs_ready;

/* ---- Timer ------------------------------------------------------------ */
uint64_t g_timer_ticks = 0;

uint64_t timer_tick_count(void) { return g_timer_ticks; }
uint32_t timer_frequency_hz(void) { return 100; }

void timer_tick(void) { g_timer_ticks++; }

int timer_handle_interrupt(struct interrupt_frame *frame) {
    (void)frame;
    timer_tick();
    return 0;
}

/* ---- Journal (no-op) -------------------------------------------------- */
void journal_log_hex(enum journal_level level, uint32_t pid, const char *msg, uint64_t value) {
    (void)level; (void)pid; (void)msg; (void)value;
}

/* ---- Interrupt stubs (for process.h declarations) ---------------------- */
void interrupt_restore_user_context(const struct interrupt_frame *frame) {
    (void)frame;
}

int process_ap_timer(struct interrupt_frame *frame) {
    (void)frame; return 0;
}

/* ---- FD stubs --------------------------------------------------------- */
void fd_table_init(struct fd_table *table) { (void)table; }

/* ---- VFS stubs — minimal wrappers around ext2 ------------------------ */

int vfs_stat_path(const char *path, struct vfs_stat *stat) {
    if (!g_fs_ready) return 0;
    uint32_t ino = ext2_lookup_inode(&g_fs, path);
    if (!ino) return 0;
    struct ext2_inode *node = &g_fs.inode_table[ino - 1];
    stat->size = node->size;
    stat->mode = node->mode;
    stat->kind = (node->mode & 0x4000) ? VFS_NODE_DIRECTORY : VFS_NODE_FILE;
    return 1;
}

ssize_t vfs_read(const char *path, size_t offset, void *buffer, size_t count) {
    if (!g_fs_ready) return -1;
    uint32_t ino = ext2_lookup_inode(&g_fs, path);
    if (!ino) return -1;
    return ext2_read(&g_fs, ino, (uint32_t)offset, count, buffer);
}

ssize_t vfs_write_all(const char *path, const void *data, size_t count) {
    if (!g_fs_ready) return -1;
    uint32_t ino = ext2_lookup_inode(&g_fs, path);
    if (!ino) {
        ino = ext2_create(&g_fs, path, 0100644);
        if (!ino) return -1;
    }
    return ext2_write(&g_fs, ino, 0, count, data);
}

int vfs_mkdir(const char *path) {
    if (!g_fs_ready) return -1;
    uint32_t ino = ext2_mkdir(&g_fs, path, 0040755);
    return ino ? 0 : -1;
}

int vfs_readdir(const char *path, uint32_t index, struct vfs_dir_entry *entry) {
    /* Simplified: just walk ext2 directory blocks */
    if (!g_fs_ready) return 0;
    uint32_t dir_ino = ext2_lookup_inode(&g_fs, path);
    if (!dir_ino) return 0;
    (void)index; (void)entry;
    return 0;  /* TODO: proper readdir */
}

int vfs_file_exists(const char *path) {
    if (!g_fs_ready) return 0;
    return ext2_lookup_inode(&g_fs, path) ? 1 : 0;
}

/* Stubs for VFS functions not yet needed by window.c */
ssize_t vfs_read_user(const char *path, size_t off, void *buf, size_t n) {
    (void)path; (void)off; (void)buf; (void)n; return -1;
}
ssize_t vfs_write(const char *path, size_t off, const void *data, size_t n) {
    (void)path; (void)off; (void)data; (void)n; return -1;
}
int vfs_create(const char *path) { (void)path; return -1; }
int vfs_unlink(const char *path) { (void)path; return -1; }
int vfs_open_ino(const char *path, uint32_t *ino) { (void)path; ino=0; return -1; }
int vfs_chmod(const char *path, uint16_t mode) { (void)path; (void)mode; return -1; }
int vfs_chown(const char *path, uint16_t uid, uint16_t gid) { (void)path; (void)uid; (void)gid; return -1; }

/* ---- Builtin app stubs (arm64 has no kernel-builtin apps) -------------- */
struct app_instance;
int app_needs_redraw(struct app_instance *app) { (void)app; return 0; }
struct app_draw_context;
void app_draw(const struct app_instance *app, const struct app_draw_context *ctx) {
    (void)app; (void)ctx;
}

/* ---- More builtin app stubs ------------------------------------------- */
struct app_menu_item;
uint32_t app_menu_items(const struct app_instance *app) { (void)app; return 0; }
void app_menu_action(struct app_instance *app, uint32_t index) { (void)app; (void)index; }
void app_handle_keyboard(struct app_instance *app, uint8_t scancode, int pressed) {
    (void)app; (void)scancode; (void)pressed;
}
void app_consume_damage(struct app_instance *app) { (void)app; }
int app_window_closed(const struct app_instance *app, uint8_t win_id) {
    (void)app; (void)win_id; return 0;
}
uint32_t app_window_owner_pid(const struct app_instance *app, uint8_t win_id) {
    (void)app; (void)win_id; return 0;
}
/* Builtin app constructors — no-ops on arm64 */
void app_init_text(void) {}
void app_init_terminal(void) {}
void app_init_task_manager(void) {}

/* ---- g_desktop_uid (global, set when GUI starts) ---------------------- */
uint32_t g_desktop_uid = 0;
