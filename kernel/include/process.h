#ifndef VIBEOS_PROCESS_H
#define VIBEOS_PROCESS_H

#include "fd.h"
#include "interrupts.h"
#include "syscall.h"
#include "types.h"

struct tty;

#define PROCESS_MAX_COUNT 4
#define PROCESS_USER_BASE 0x20000000u
#define PROCESS_USER_TEMPLATE_BASE PROCESS_USER_BASE
#define PROCESS_USER_REGION_BYTES 0x02000000u
#define PROCESS_USER_STACK_SIZE 0x100000u
/* 32 MB virtual user slot per process. The ELF image/BSS grows upward from
 * PROCESS_USER_BASE; the stack owns the top 1 MB. Keep this well above the
 * identity-mapped kernel/BSS/heap area so activating a process page table does
 * not replace mappings the kernel still touches during syscalls. Physical
 * memory for the image is still allocated to fit the ELF p_memsz instead of
 * reserving the full slot. */
#define PROCESS_USER_IMAGE_SIZE (PROCESS_USER_REGION_BYTES - PROCESS_USER_STACK_SIZE)
#define PROCESS_USER_STACK_TOP_OFFSET PROCESS_USER_REGION_BYTES
#define PROCESS_USER_PAGE_TABLE_COUNT (PROCESS_USER_REGION_BYTES / 0x200000u)
#define PROCESS_VFS_HANDLE_CAPACITY 8

enum process_state {
    PROCESS_STATE_EMPTY = 0,
    PROCESS_STATE_READY = 1,
    PROCESS_STATE_RUNNING = 2,
    PROCESS_STATE_SLEEPING = 3,
    PROCESS_STATE_WAITING = 4,
    PROCESS_STATE_EXITED = 5,
    PROCESS_STATE_WAITING_IO = 6
};

enum process_run_result {
    PROCESS_RUN_NONE = 0,
    PROCESS_RUN_YIELDED = 1,
    PROCESS_RUN_EXITED = 2
};

struct process_snapshot {
    uint32_t pid;
    uint32_t parent_pid;
    uint64_t runtime_ticks;
    uint64_t switch_count;
    uint64_t preempt_count;
    uint64_t wake_tick;
    uint8_t state;
    uint8_t loaded;
    char name[32];
};

/* kind 0 = embedded vfs_file pointer, kind 1 = path-based (user/ext2) */
struct process_vfs_handle {
    const void *file;
    size_t offset;
    uint8_t used;
    uint8_t kind;
    char path[64];
};

struct process {
    uint32_t pid;
    uint32_t parent_pid;
    uint32_t wait_target_pid;
    uintptr_t entry;
    uintptr_t user_stack_top;
    uintptr_t user_virtual_base;
    uintptr_t code_size;
    void *user_image_allocation;
    uint8_t *user_image_pages;
    size_t user_image_capacity;
    uint64_t *user_page_tables;
    uint8_t loaded;
    uint8_t state;
    int32_t exit_code;
    uint64_t runtime_ticks;
    uint64_t switch_count;
    uint64_t preempt_count;
    uint64_t wake_tick;
    int pending_read_fd;
    void *pending_read_buffer;
    size_t pending_read_count;
    struct process_vfs_handle vfs_handles[PROCESS_VFS_HANDLE_CAPACITY];
    struct fd_table fd_table;
    struct syscall_context syscalls;
    struct interrupt_frame context;
    char cwd[256];
    char spawn_arg[64];
    char name[32];
    /* x87+SSE state, saved/restored across context switches (FXSAVE/FXRSTOR).
     * 16-byte aligned as the instructions require. */
    uint8_t fpu_state[512] __attribute__((aligned(16)));
};

void process_init(void);
int process_spawn_demo_processes(uint32_t count);
int process_spawn_embedded_elf(const uint8_t *image, size_t image_size, const struct fd_ops *stdio_ops, void *stdio_object);
int process_spawn_path(const char *path, const struct fd_ops *stdio_ops, void *stdio_object);
int process_run_ready_slice(void);
int timer_handle_interrupt(struct interrupt_frame *frame);
uint32_t process_loaded_count(void);
uint32_t process_ready_count(void);
uint32_t process_running_count(void);
uint32_t process_sleeping_count(void);
uint32_t process_snapshot_count(void);
int process_get_snapshot(uint32_t slot, struct process_snapshot *snapshot);
int process_kill(uint32_t pid);
int process_pid_alive(uint32_t pid);
int process_handle_user_fault(uint64_t vector, uint64_t rip, uint64_t cr2, uint64_t error_code);
void process_wake_tty_reader(struct tty *tty);
int process_take_window_manager_request(void);

#endif
