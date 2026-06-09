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
#include "../../include/window.h"
#include "../../include/app.h"
#include "../../include/framebuffer.h"
#include "../../include/render.h"

/* ---- Globals from arch.c ---------------------------------------------- */
extern struct ext2_filesystem g_fs;
extern int g_fs_ready;

/* ---- Timer ------------------------------------------------------------ */
uint64_t g_timer_ticks = 0;

uint64_t timer_tick_count(void) {
    uint64_t cnt;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(cnt));
    return cnt;
}
uint32_t timer_frequency_hz(void) {
    uint64_t freq;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    return (uint32_t)freq;
}

void timer_tick(void) { g_timer_ticks++; }

int timer_handle_interrupt(struct interrupt_frame *frame) {
    (void)frame;
    timer_tick();
    return 0;
}

/* ---- Journal (no-op) -------------------------------------------------- */
void journal_log(enum journal_level level, uint32_t pid, const char *msg) {
    (void)level; (void)pid;
    if (msg) {
        serial_write("[journal] ");
        serial_write(msg);
        serial_write("\r\n");
    }
}
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
    stat->uid  = node->uid;
    stat->gid  = node->gid;
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
    if (!g_fs_ready || !entry) return 0;
    uint32_t dir_ino = ext2_lookup_inode(&g_fs, path);
    if (!dir_ino) return 0;

    /* Read the directory in blocks, skip entries up to `index`. */
    uint32_t block_size = 1024 << g_fs.superblock.log_block_size;
    struct ext2_inode *node = &g_fs.inode_table[dir_ino - 1];
    uint32_t remaining = node->size;
    uint32_t seen = 0;

    for (uint32_t blk_off = 0; remaining > 0; blk_off += block_size) {
        uint8_t block[1024]; /* max block size for simplicity */
        uint32_t read_size = remaining < block_size ? remaining : block_size;
        ssize_t got = ext2_read(&g_fs, dir_ino, blk_off, read_size, block);
        if (got <= 0) return 0;

        uint32_t offset = 0;
        while (offset < (uint32_t)got) {
            struct ext2_dir_entry *e = (struct ext2_dir_entry *)(block + offset);
            if (e->inode == 0) { offset += e->rec_len; continue; }
            if (seen == index) {
                /* Found the requested entry — copy name + stat */
                static char name_buf[256];
                size_t name_len = e->name_len;
                if (name_len > sizeof(name_buf) - 1)
                    name_len = sizeof(name_buf) - 1;
                for (size_t i = 0; i < name_len; i++)
                    name_buf[i] = e->name[i];
                name_buf[name_len] = '\0';
                entry->name = name_buf;

                /* Stat the entry to get kind and size. */
                struct ext2_inode *child = &g_fs.inode_table[e->inode - 1];
                entry->kind = (child->mode & 0x4000) ? VFS_NODE_DIRECTORY : VFS_NODE_FILE;
                entry->size = child->size;
                return 1;
            }
            seen++;
            offset += e->rec_len;
        }
        remaining -= (uint32_t)got;
    }
    return 0; /* index out of range */
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
int vfs_chmod(const char *path, uint16_t mode) {
    if (!g_fs_ready || !path) return -1;
    uint32_t ino = ext2_lookup_inode(&g_fs, path);
    if (!ino) return -2;  /* ENOENT */

    uint32_t caller_uid = process_current_uid();
    struct ext2_inode *ip = &g_fs.inode_table[ino - 1];

    /* Only the file owner or root may change permission bits. */
    if (caller_uid != 0 && caller_uid != (uint32_t)ip->uid)
        return -1;  /* EPERM */

    ip->mode = (ip->mode & ~(uint16_t)0777u) | (mode & 0777u);
    ext2_write_inode(&g_fs, ino);
    return 0;
}

int vfs_chown(const char *path, uint16_t uid, uint16_t gid) {
    if (!g_fs_ready || !path) return -1;
    uint32_t ino = ext2_lookup_inode(&g_fs, path);
    if (!ino) return -2;  /* ENOENT */

    uint32_t caller_uid = process_current_uid();

    /* Only root may reassign file ownership. */
    if (caller_uid != 0) return -1;  /* EPERM */

    struct ext2_inode *ip = &g_fs.inode_table[ino - 1];
    ip->uid = uid;
    ip->gid = gid;
    ext2_write_inode(&g_fs, ino);
    return 0;
}

/* ---- Builtin app stubs (arm64 has no kernel-builtin apps) -------------- */
struct app_instance;
int app_needs_redraw(struct app_instance *app) { (void)app; return 0; }
struct app_draw_context;
void app_draw(const struct app_instance *app, const struct app_draw_context *ctx) {
    (void)app;
    /* Draw placeholder text in the content area so demo windows aren't blank.
     * The window frame (title bar) is already drawn by the compositor. */
    if (!ctx || !ctx->fb) return;
    int cx = ctx->content_x, cy = ctx->content_y;
    const char *title = ctx->window ? ctx->window->title : "arm64";
    draw_text(ctx->fb, cx + 16, cy + 24, title, 0xffc0d0e0, 2);
    draw_text(ctx->fb, cx + 16, cy + 56, "shared window.c compositor", 0xff90b0e0, 1);
}

/* ---- More builtin app stubs ------------------------------------------- */
int app_menu_items(struct app_instance *app, struct winsys_menu_item *out, int max) {
    (void)app; (void)out; (void)max; return 0;
}
void app_menu_action(struct app_instance *app, uint32_t index) { (void)app; (void)index; }
int app_handle_keyboard(struct app_instance *app, const struct keyboard_state *keyboard) {
    (void)app; (void)keyboard; return 0;
}
int app_consume_damage(struct app_instance *app, const struct app_draw_context *ctx, struct rect *damage_rect) {
    (void)app; (void)ctx; (void)damage_rect; return 0;
}
void app_window_closed(struct app_instance *app) { (void)app; }
uint32_t app_window_owner_pid(struct app_instance *app) { (void)app; return 0; }
/* Builtin app constructors — no-ops on arm64 */
void app_init_text(struct app_instance *app, struct text_app_state *state, const char *const *lines, size_t line_count) { (void)app; (void)state; (void)lines; (void)line_count; }
void app_init_terminal(struct app_instance *app, struct terminal_app_state *state) { (void)app; (void)state; }
void app_init_task_manager(struct app_instance *app, struct task_manager_app_state *state, const struct desktop_state *desktop) { (void)app; (void)state; (void)desktop; }

/* ---- g_desktop_uid (global, set when GUI starts) ---------------------- */
uint32_t g_desktop_uid = 0;
