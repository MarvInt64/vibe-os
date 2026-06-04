#ifndef VIBEOS_PROCESS_H
#define VIBEOS_PROCESS_H

#include "fd.h"
#include "interrupts.h"
#include "syscall.h"
#include "types.h"

struct tty;

/* Max concurrent processes/threads. Threads share their parent's address space
 * but still occupy a slot here, so this also bounds total threads. 48 leaves
 * room for sh + dock + several GUI apps + browser worker threads. Each slot
 * costs ~1.1 MB (1 MB user stack + 16 KB kernel stack + page tables). */
#define PROCESS_MAX_COUNT 48
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
    PROCESS_RUN_EXITED = 2,
    /* The process suspended itself mid-syscall (blocking I/O). Its kernel
     * stack is parked; the scheduler will resume it on a later round. */
    PROCESS_RUN_BLOCKED = 3
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
    /* Appended fields (keep in sync with vui_process_info in user/vexui.h).
     * mem_bytes  = physical RAM backing the process (image+BSS+heap + stacks).
     * thread_count = number of threads sharing this address space (>=1). */
    uint64_t mem_bytes;
    uint32_t thread_count;
    uint8_t  is_thread;     /* 1 = a worker thread, not a top-level app */
};

/* kind 0 = embedded vfs_file pointer, kind 1 = path-based (user/ext2) */
struct process_vfs_handle {
    const void *file;
    size_t offset;
    uint8_t used;
    uint8_t kind;
    char path[64];
    /* Cached ext2 inode number resolved at open() time.  Avoids re-running the
     * full directory-traversal on every read(), which can leave the IDE
     * controller in a state that makes the subsequent data-block read fail. */
    uint32_t cached_ino;
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
    /* CR3 (physical PML4 address) for this process's PRIVATE address space.
     * The scheduler loads it on every switch; threads borrow the parent's. */
    uintptr_t cr3;
    /* Demand-grown heap (SYS_SBRK). heap_start = first heap virtual address
     * (just above the loaded image); heap_break = current top of mapped heap.
     * Pages between them are backed by per-process physical chunks. Threads
     * share the owner's heap (they share the address space). */
    uintptr_t heap_start;
    uintptr_t heap_break;
    /* Physical backing for the demand-grown heap: each SYS_SBRK growth kmallocs
     * a chunk and records it here so it can be freed when the process exits.
     * Only the address-space owner (not threads) owns/frees these. */
#define PROCESS_HEAP_MAX_CHUNKS 32
    void *heap_chunks[PROCESS_HEAP_MAX_CHUNKS];
    uint32_t heap_chunk_count;
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
    /* Top of this process's private kernel stack, loaded into TSS.rsp0 before
     * the process runs so its syscalls/traps don't share one global stack. */
    uintptr_t kernel_stack_top;
    /* When the process is suspended in the middle of a blocking syscall, this
     * holds its parked kernel stack pointer (0 = not parked). The scheduler
     * resumes it via process_resume_blocked instead of a fresh user entry. */
    uintptr_t kresume_rsp;
    /* Thread flag: 1 if this is a thread sharing another process's address
     * space (user_page_tables + image are borrowed, not owned). On exit the
     * shared image must NOT be freed — only the owning process frees it. */
    uint8_t is_thread;
    char cwd[256];
    char spawn_arg[64];
    char name[32];
    /* Effective user/group ID. 0 = root (all access). */
    uint32_t uid;
    uint32_t gid;
    /* x87+SSE state, saved/restored across context switches (FXSAVE/FXRSTOR).
     * 16-byte aligned as the instructions require. */
    uint8_t fpu_state[512] __attribute__((aligned(16)));
};

/* Return uid/gid of the currently executing process (0 when idle). */
uint32_t process_current_uid(void);
uint32_t process_current_gid(void);

void process_init(void);
int process_spawn_demo_processes(uint32_t count);
int process_spawn_embedded_elf(const uint8_t *image, size_t image_size, const struct fd_ops *stdio_ops, void *stdio_object);
int process_spawn_path(const char *path, const struct fd_ops *stdio_ops, void *stdio_object);
int process_run_ready_slice(void);
int timer_handle_interrupt(struct interrupt_frame *frame);

/* Called from inside a blocking kernel operation (e.g. the network wait loops)
 * to suspend the current process and let the scheduler run the desktop and
 * other processes. Returns when the process is rescheduled; the caller then
 * re-checks its wait condition. No-op (returns immediately) if there is no
 * current process — callers should fall back to a busy poll in that case. */
void process_yield_blocking(void);

/* True when a process is currently running (i.e. we are inside a syscall on a
 * process's kernel stack), so process_yield_blocking() is usable. */
int process_has_current(void);
uint32_t process_loaded_count(void);
uint32_t process_ready_count(void);
uint32_t process_running_count(void);
uint32_t process_sleeping_count(void);
uint32_t process_snapshot_count(void);
int process_get_snapshot(uint32_t slot, struct process_snapshot *snapshot);
int process_kill(uint32_t pid);
/* Ctrl+C: kill the foreground job of the terminal whose stdin is stdin_object
 * (a struct tty * or struct pty *). Returns the number of children killed. */
int process_interrupt_terminal(const void *stdin_object);
int process_pid_alive(uint32_t pid);
/* PID of the process currently running (inside a syscall), or 0 if none. */
uint32_t process_current_pid(void);
int process_handle_user_fault(uint64_t vector, uint64_t rip, uint64_t cr2, uint64_t error_code);
void process_wake_tty_reader(struct tty *tty);
int process_take_window_manager_request(void);

#endif
