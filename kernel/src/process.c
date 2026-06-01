#include "alloc.h"
#include "elf.h"
#include "ext2_fs.h"
#include "net.h"
#include "paging.h"
#include "process.h"
#include "serial.h"
#include "syscall.h"
#include "journal.h"
#include "timer.h"
#include "tty.h"
#include "vfs.h"
#include "window.h"
#include "winsys.h"

/* Provided by kernel.c: the live compositor, or 0 if the GUI isn't running. */
struct desktop_state *desktop_active(void);
void kernel_request_resolution(uint32_t width, uint32_t height);
uint32_t kernel_current_resolution(void);

uintptr_t g_kernel_resume_rsp;
uint64_t g_kernel_resume_result;

static struct process g_processes[PROCESS_MAX_COUNT];
static struct process *g_current_process;
static uint32_t g_next_pid;
static uint32_t g_scheduler_cursor;
static uint8_t g_window_manager_requested;
static uint8_t g_user_stacks[PROCESS_MAX_COUNT][PROCESS_USER_STACK_SIZE] __attribute__((aligned(4096)));
static uint64_t g_user_page_tables[PROCESS_MAX_COUNT][PROCESS_USER_PAGE_TABLE_COUNT][512] __attribute__((aligned(4096)));

/* Per-process ring-0 kernel stacks. Each process handles its syscalls/traps on
 * its own stack (TSS.rsp0 is repointed before the process runs) so a process
 * suspended mid-syscall keeps its kernel frames intact while others run. */
#define PROCESS_KERNEL_STACK_SIZE 16384u
static uint8_t g_kernel_stacks[PROCESS_MAX_COUNT][PROCESS_KERNEL_STACK_SIZE] __attribute__((aligned(16)));

static const struct fd_ops PROCESS_STDIO_FD_OPS;
static const struct fd_ops PROCESS_VFS_FD_OPS;

static void process_wake_ready(uint64_t current_tick) {
    size_t i;

    for (i = 0; i < PROCESS_MAX_COUNT; ++i) {
        struct process *process = &g_processes[i];

        if (process->state == PROCESS_STATE_SLEEPING && process->wake_tick <= current_tick) {
            process->wake_tick = 0;
            process->state = PROCESS_STATE_READY;
        }
    }
}

static void memory_zero(uint8_t *dest, size_t count) {
    size_t i;

    for (i = 0; i < count; ++i) {
        dest[i] = 0;
    }
}

static void memory_copy(uint8_t *dest, const uint8_t *src, size_t count) {
    size_t i;

    for (i = 0; i < count; ++i) {
        dest[i] = src[i];
    }
}

static size_t align_up_size(size_t value, size_t alignment) {
    return (value + alignment - 1u) & ~(alignment - 1u);
}

static uint8_t *align_ptr_4k(void *ptr) {
    uintptr_t value = (uintptr_t)ptr;
    value = (value + 0xfffu) & ~((uintptr_t)0xfffu);
    return (uint8_t *)value;
}

static uintptr_t process_slot_base(uint32_t slot) {
    /* Every process is linked (non-PIE) at PROCESS_USER_BASE and therefore
     * must run at that same virtual address. Each slot keeps its own physical
     * backing pages and its own page table; the kernel swaps the live page
     * table in pd_table0 on every context switch (see
     * paging_activate_user_table), so the slots never collide even though
     * they share one virtual base. This is what lets more than one process
     * (e.g. the desktop terminal's shell) address its globals correctly. */
    (void)slot;
    return PROCESS_USER_BASE;
}

static uintptr_t process_slot_stack_top(uint32_t slot) {
    return process_slot_base(slot) + PROCESS_USER_STACK_TOP_OFFSET;
}

static int process_find_free_slot(void) {
    uint32_t slot;

    for (slot = 0; slot < PROCESS_MAX_COUNT; ++slot) {
        if (!g_processes[slot].loaded || g_processes[slot].state == PROCESS_STATE_EXITED || g_processes[slot].state == PROCESS_STATE_EMPTY) {
            return (int)slot;
        }
    }

    return -1;
}

static void process_copy_fd_table(struct fd_table *dest, const struct fd_table *src) {
    int fd;

    for (fd = 0; fd < FD_TABLE_CAPACITY; ++fd) {
        dest->entries[fd] = src->entries[fd];
    }
}

static void process_reset_vfs_handles(struct process *process) {
    size_t i;

    for (i = 0; i < PROCESS_VFS_HANDLE_CAPACITY; ++i) {
        process->vfs_handles[i].file = 0;
        process->vfs_handles[i].offset = 0;
        process->vfs_handles[i].used = 0;
    }
}

static struct process_vfs_handle *process_alloc_vfs_handle(struct process *process) {
    size_t i;

    for (i = 0; i < PROCESS_VFS_HANDLE_CAPACITY; ++i) {
        if (!process->vfs_handles[i].used) {
            process->vfs_handles[i].used = 1;
            process->vfs_handles[i].file = 0;
            process->vfs_handles[i].offset = 0;
            return &process->vfs_handles[i];
        }
    }

    return 0;
}

static void process_release_vfs_handle(struct process_vfs_handle *handle) {
    if (handle == 0) {
        return;
    }

    handle->file = 0;
    handle->offset = 0;
    handle->used = 0;
    handle->kind = 0;
    handle->path[0] = '\0';
}

static int process_copy_user_string(char *dest, size_t capacity, const char *src) {
    size_t i = 0;

    if (dest == 0 || src == 0 || capacity == 0) {
        return 0;
    }

    while (i + 1 < capacity) {
        char c = src[i];
        dest[i] = c;
        if (c == '\0') {
            return 1;
        }
        ++i;
    }

    dest[capacity - 1] = '\0';
    return 0;
}

static int process_write_user_string(char *dest, size_t capacity, const char *src) {
    size_t i = 0;

    if (dest == 0 || src == 0 || capacity == 0) {
        return 0;
    }

    while (src[i] != '\0' && i + 1 < capacity) {
        dest[i] = src[i];
        ++i;
    }
    dest[i] = '\0';
    return 1;
}

static struct process *process_find_by_pid(uint32_t pid) {
    size_t i;

    if (pid == 0) {
        return 0;
    }

    for (i = 0; i < PROCESS_MAX_COUNT; ++i) {
        if (g_processes[i].loaded && g_processes[i].pid == pid) {
            return &g_processes[i];
        }
    }

    return 0;
}

static struct process *process_find_exited_child(uint32_t parent_pid, uint32_t wait_target_pid) {
    size_t i;

    for (i = 0; i < PROCESS_MAX_COUNT; ++i) {
        struct process *process = &g_processes[i];

        if (!process->loaded || process->state != PROCESS_STATE_EXITED || process->parent_pid != parent_pid) {
            continue;
        }

        if (wait_target_pid == 0 || process->pid == wait_target_pid) {
            return process;
        }
    }

    return 0;
}

static void process_reap(struct process *process) {
    if (process == 0) {
        return;
    }

    process->loaded = 0;
    process->state = PROCESS_STATE_EMPTY;
    process->pid = 0;
    process->parent_pid = 0;
    process->wait_target_pid = 0;
    process->exit_code = 0;
    process->entry = 0;
    process->user_stack_top = 0;
    process->user_virtual_base = 0;
    process->code_size = 0;
    /* Threads borrow the owning process's image + page tables; only the owner
     * frees them. Freeing here would pull the address space out from under the
     * still-running parent (and its other threads). */
    if (!process->is_thread && process->user_image_allocation != 0) {
        kfree(process->user_image_allocation);
    }
    process->is_thread = 0;
    process->user_image_allocation = 0;
    process->user_image_pages = 0;
    process->user_image_capacity = 0;
    process->user_page_tables = 0;
    process->runtime_ticks = 0;
    process->switch_count = 0;
    process->preempt_count = 0;
    process->wake_tick = 0;
    process->pending_read_fd = -1;
    process->pending_read_buffer = 0;
    process->pending_read_count = 0;
    process_reset_vfs_handles(process);
    process->cwd[0] = '/';
    process->cwd[1] = '\0';
    process->spawn_arg[0] = '\0';
    process->name[0] = '\0';
}

static void process_set_name(struct process *process, const char *name) {
    size_t i;

    if (process == 0) {
        return;
    }

    if (name == 0 || name[0] == '\0') {
        name = "process";
    }

    for (i = 0; i + 1 < sizeof(process->name) && name[i] != '\0'; ++i) {
        process->name[i] = name[i];
    }
    process->name[i] = '\0';
}

static void process_copy_name_to_snapshot(struct process_snapshot *snapshot, const char *name) {
    size_t i;

    if (snapshot == 0) {
        return;
    }

    if (name == 0) {
        name = "";
    }

    for (i = 0; i + 1 < sizeof(snapshot->name) && name[i] != '\0'; ++i) {
        snapshot->name[i] = name[i];
    }
    snapshot->name[i] = '\0';
}

static void process_wake_waiter(struct process *child) {
    struct process *parent;

    if (child == 0 || child->parent_pid == 0) {
        return;
    }

    parent = process_find_by_pid(child->parent_pid);
    if (parent == 0 || parent->state != PROCESS_STATE_WAITING) {
        return;
    }

    if (parent->wait_target_pid != 0 && parent->wait_target_pid != child->pid) {
        return;
    }

    parent->wait_target_pid = 0;
    parent->context.rax = child->pid;
    parent->context.rdx = (uint64_t)(uint32_t)child->exit_code;
    parent->state = PROCESS_STATE_READY;
    process_reap(child);
}

static void process_setup_context(struct process *process) {
    /* A freshly set-up process is never mid-syscall: clear any stale parked
     * kernel stack left over from a previous tenant of this slot. */
    process->kresume_rsp = 0;
    process->context.rax = 0;
    process->context.rbx = 0;
    process->context.rcx = 0;
    process->context.rdx = 0;
    process->context.rbp = 0;
    process->context.rdi = 0;
    process->context.rsi = 0;
    process->context.r8 = 0;
    process->context.r9 = 0;
    process->context.r10 = 0;
    process->context.r11 = 0;
    process->context.r12 = 0;
    process->context.r13 = 0;
    process->context.r14 = 0;
    process->context.r15 = 0;
    process->context.rip = process->entry;
    process->context.cs = USER_CODE_SELECTOR | 0x03u;
    process->context.rflags = 0x202u;
    /* SysV x86-64 ABI: at a function's entry point rsp must be ≡ 8 (mod 16),
     * i.e. (rsp+8) is 16-aligned — that's the state right after a CALL pushes
     * the 8-byte return address. We jump to _start directly (no CALL), and
     * user_stack_top is 16-aligned, so subtract 8 to honour the invariant.
     * Without this, SSE `movdqa` on rbp-relative slots (e.g. stb_truetype's
     * rasterizer) faults with #GP once userspace uses floating point. */
    process->context.rsp = (process->user_stack_top & ~(uintptr_t)15) - 8u;
    process->context.ss = USER_DATA_SELECTOR | 0x03u;

    /* Clean initial FPU/SSE state for FXRSTOR: default control words, regs 0. */
    {
        size_t i;
        for (i = 0; i < sizeof(process->fpu_state); ++i) process->fpu_state[i] = 0;
        process->fpu_state[0] = 0x7f; process->fpu_state[1] = 0x03;   /* FCW = 0x037F */
        process->fpu_state[24] = 0x80; process->fpu_state[25] = 0x1f; /* MXCSR = 0x1F80 */
    }
}

static ssize_t process_stdio_read(void *object, void *buffer, size_t count) {
    (void)object;
    (void)buffer;
    (void)count;
    return 0;
}

static ssize_t process_stdio_write(void *object, const void *buffer, size_t count) {
    (void)object;

    if (buffer == 0) {
        return -SYSCALL_EINVAL;
    }

    serial_write_buffer((const char *)buffer, count);
    return (ssize_t)count;
}

static int process_stdio_ioctl(void *object, uint32_t request, uintptr_t arg) {
    (void)object;
    (void)request;
    (void)arg;
    return -SYSCALL_ENOSYS;
}

static ssize_t process_vfs_read(void *object, void *buffer, size_t count) {
    struct process_vfs_handle *handle = (struct process_vfs_handle *)object;

    if (handle == 0 || buffer == 0 || !handle->used) {
        return -SYSCALL_EINVAL;
    }

    if (handle->kind == 1) {
        /* path-based: user files or ext2 */
        ssize_t n = vfs_read(handle->path, handle->offset, buffer, count);
        if (n > 0) handle->offset += (size_t)n;
        return n;
    } else {
        /* embedded vfs_file pointer */
        const struct vfs_file *file;
        size_t file_size;
        size_t remaining;
        size_t to_copy;
        size_t i;
        uint8_t *out = (uint8_t *)buffer;

        if (handle->file == 0) return -SYSCALL_EINVAL;

        file = (const struct vfs_file *)handle->file;
        file_size = vfs_file_size(file);
        if (handle->offset >= file_size) return 0;

        remaining = file_size - handle->offset;
        to_copy = count < remaining ? count : remaining;
        for (i = 0; i < to_copy; ++i) {
            out[i] = file->data[handle->offset + i];
        }
        handle->offset += to_copy;
        return (ssize_t)to_copy;
    }
}

static ssize_t process_vfs_write(void *object, const void *buffer, size_t count) {
    (void)object;
    (void)buffer;
    (void)count;
    return -SYSCALL_ENOSYS;
}

static int process_vfs_ioctl(void *object, uint32_t request, uintptr_t arg) {
    (void)object;
    (void)request;
    (void)arg;
    return -SYSCALL_ENOSYS;
}

static const struct fd_ops PROCESS_STDIO_FD_OPS = {
    process_stdio_read,
    process_stdio_write,
    process_stdio_ioctl
};

static const struct fd_ops PROCESS_VFS_FD_OPS = {
    process_vfs_read,
    process_vfs_write,
    process_vfs_ioctl
};

static int process_load_image_slot(struct process *process, uint32_t slot, const uint8_t *image, size_t image_size, const struct fd_table *inherit_fds, const struct fd_ops *stdio_ops, void *stdio_object, uint32_t parent_pid, const char *process_name) {
    const struct elf64_header *header = (const struct elf64_header *)image;
    const struct elf64_program_header *program_header;
    uintptr_t slot_base = process_slot_base(slot);
    uintptr_t stack_top = process_slot_stack_top(slot);
    size_t image_capacity = 0;
    void *image_allocation;
    uint8_t *image_pages;
    size_t i;

    serial_write("VIBEOS: process_load_image_slot slot=");
    serial_write_hex_u64(slot);
    serial_write(" process=");
    serial_write_hex_u64((uint64_t)(uintptr_t)process);
    serial_write(" image=");
    serial_write_hex_u64((uint64_t)(uintptr_t)image);
    serial_write(" size=");
    serial_write_hex_u64(image_size);
    serial_write("\n");

    if (process == 0) {
        serial_write("VIBEOS: process is NULL\n");
        return 0;
    }
    
    if (slot >= PROCESS_MAX_COUNT) {
        serial_write("VIBEOS: slot >= PROCESS_MAX_COUNT\n");
        return 0;
    }
    
    if (!elf64_validate(image, image_size)) {
        serial_write("VIBEOS: elf64_validate failed\n");
        serial_write("VIBEOS: First 4 bytes: ");
        if (image) {
            serial_write_hex_u64(*(uint32_t*)image);
        }
        serial_write("\n");
        return 0;
    }
    
    serial_write("VIBEOS: ELF validated OK\n");
    serial_write("VIBEOS: ELF entry=0x");
    serial_write_hex_u64(header->entry);
    serial_write(" ph_offset=");
    serial_write_hex_u64(header->program_header_offset);
    serial_write(" ph_count=");
    serial_write_hex_u64(header->program_header_count);
    serial_write("\n");

    program_header = (const struct elf64_program_header *)(image + header->program_header_offset);
    
    serial_write("VIBEOS: Processing ");
    serial_write_hex_u64(header->program_header_count);
    serial_write(" program headers\n");
    
    for (i = 0; i < header->program_header_count; ++i) {
        uintptr_t segment_offset;

        serial_write("VIBEOS: PH[");
        serial_write_hex_u64(i);
        serial_write("] type=");
        serial_write_hex_u64(program_header[i].type);
        serial_write(" vaddr=");
        serial_write_hex_u64(program_header[i].virtual_address);
        serial_write(" file_size=");
        serial_write_hex_u64(program_header[i].file_size);
        serial_write("\n");

        if (program_header[i].type != ELF_PROGRAM_TYPE_LOAD) {
            serial_write("VIBEOS:   -> Not LOAD type, skipping\n");
            continue;
        }

        serial_write("VIBEOS:   -> LOAD segment found\n");
        serial_write("VIBEOS:   Checking vaddr 0x");
        serial_write_hex_u64(program_header[i].virtual_address);
        serial_write(" vs PROCESS_USER_TEMPLATE_BASE 0x");
        serial_write_hex_u64(PROCESS_USER_TEMPLATE_BASE);
        serial_write("\n");
        
        if (program_header[i].virtual_address < PROCESS_USER_TEMPLATE_BASE) {
            serial_write("VIBEOS:   -> Virtual address below user base!\n");
            return 0;
        }

        segment_offset = program_header[i].virtual_address - PROCESS_USER_TEMPLATE_BASE;

        serial_write("VIBEOS:   Checking file_size ");
        serial_write_hex_u64(program_header[i].file_size);
        serial_write(" mem_size=");
        serial_write_hex_u64(program_header[i].memory_size);
        serial_write(" vs image max ");
        serial_write_hex_u64(PROCESS_USER_IMAGE_SIZE);
        serial_write("\n");
        
        if (program_header[i].file_size > program_header[i].memory_size) {
            serial_write("VIBEOS:   -> ELF file_size exceeds memory_size!\n");
            return 0;
        }

        if (segment_offset + program_header[i].memory_size > PROCESS_USER_IMAGE_SIZE) {
            serial_write("VIBEOS:   -> Program image/BSS too large for user slot!\n");
            return 0;
        }

        if (segment_offset + program_header[i].memory_size > image_capacity) {
            image_capacity = (size_t)(segment_offset + program_header[i].memory_size);
        }
        
        if (program_header[i].offset + program_header[i].file_size > image_size) {
            serial_write("VIBEOS:   -> Offset+size exceeds image!\n");
            return 0;
        }

    }

    if (image_capacity == 0 || image_capacity > PROCESS_USER_IMAGE_SIZE) {
        return 0;
    }

    image_capacity = align_up_size(image_capacity, 0x1000u);
    if (process->user_image_allocation != 0) {
        kfree(process->user_image_allocation);
        process->user_image_allocation = 0;
        process->user_image_pages = 0;
        process->user_image_capacity = 0;
    }
    image_allocation = kmalloc(image_capacity + 0x1000u);
    if (image_allocation == 0) {
        serial_write("VIBEOS:   -> Out of heap for user image\n");
        return 0;
    }
    image_pages = align_ptr_4k(image_allocation);
    memory_zero(image_pages, image_capacity);
    memory_zero(g_user_stacks[slot], PROCESS_USER_STACK_SIZE);

    for (i = 0; i < header->program_header_count; ++i) {
        uintptr_t segment_offset;

        if (program_header[i].type != ELF_PROGRAM_TYPE_LOAD) {
            continue;
        }
        segment_offset = program_header[i].virtual_address - PROCESS_USER_TEMPLATE_BASE;

        serial_write("VIBEOS:   Copying ");
        serial_write_hex_u64(program_header[i].file_size);
        serial_write(" bytes to image pages\n");

        memory_copy(image_pages + segment_offset, image + program_header[i].offset, (size_t)program_header[i].file_size);
    }

    serial_write("VIBEOS:   Mapping process memory...\n");
    paging_map_user_process(slot_base, stack_top, (uintptr_t)image_pages, image_capacity, (uintptr_t)g_user_stacks[slot], PROCESS_USER_STACK_SIZE, &g_user_page_tables[slot][0][0], PROCESS_USER_PAGE_TABLE_COUNT);
    serial_write("VIBEOS:   Memory mapping complete\n");

    process->pid = ++g_next_pid;
    process->parent_pid = parent_pid;
    process->wait_target_pid = 0;
    process->entry = slot_base + (header->entry - PROCESS_USER_TEMPLATE_BASE);
    process->user_stack_top = stack_top;
    process->user_virtual_base = slot_base;
    process->user_image_allocation = image_allocation;
    process->user_image_pages = image_pages;
    process->user_image_capacity = image_capacity;
    process->user_page_tables = &g_user_page_tables[slot][0][0];
    process->code_size = image_capacity;
    process->loaded = 1;
    process->state = PROCESS_STATE_READY;
    process->exit_code = 0;
    process->runtime_ticks = 0;
    process->switch_count = 0;
    process->preempt_count = 0;
    process->wake_tick = 0;
    process->pending_read_fd = -1;
    process->pending_read_buffer = 0;
    process->pending_read_count = 0;
    process_reset_vfs_handles(process);
    process->cwd[0] = '/';
    process->cwd[1] = '\0';
    process->spawn_arg[0] = '\0';
    process_set_name(process, process_name);

    if (inherit_fds != 0) {
        process_copy_fd_table(&process->fd_table, inherit_fds);
    } else {
        if (stdio_ops == 0) {
            stdio_ops = &PROCESS_STDIO_FD_OPS;
            stdio_object = 0;
        }

        fd_table_init(&process->fd_table);
        (void)fd_bind(&process->fd_table, 0, stdio_ops, stdio_object);
        (void)fd_bind(&process->fd_table, 1, stdio_ops, stdio_object);
        (void)fd_bind(&process->fd_table, 2, stdio_ops, stdio_object);
    }
    process->syscalls.fd_table = &process->fd_table;

    process_setup_context(process);
    return 1;
}

/* A process is runnable if it is freshly READY, or if it parked itself in a
 * blocking syscall (kresume_rsp != 0) and is now eligible to re-check its wait
 * condition. Round-robin from the cursor keeps things fair. */
static struct process *process_pick_next_ready(void) {
    uint32_t attempt;

    for (attempt = 0; attempt < PROCESS_MAX_COUNT; ++attempt) {
        uint32_t slot = (g_scheduler_cursor + attempt) % PROCESS_MAX_COUNT;
        struct process *p = &g_processes[slot];

        if (p->state == PROCESS_STATE_READY ||
            (p->state == PROCESS_STATE_WAITING_IO && p->kresume_rsp != 0)) {
            g_scheduler_cursor = (slot + 1u) % PROCESS_MAX_COUNT;
            return p;
        }
    }

    return 0;
}

void process_init(void) {
    size_t i;

    g_current_process = 0;
    g_next_pid = 0;
    g_scheduler_cursor = 0;
    g_kernel_resume_rsp = 0;
    g_kernel_resume_result = PROCESS_RUN_NONE;
    g_window_manager_requested = 0;

    for (i = 0; i < PROCESS_MAX_COUNT; ++i) {
        g_processes[i].pid = 0;
        g_processes[i].parent_pid = 0;
        g_processes[i].wait_target_pid = 0;
        g_processes[i].entry = 0;
        g_processes[i].user_stack_top = 0;
        g_processes[i].user_virtual_base = 0;
        g_processes[i].user_image_allocation = 0;
        g_processes[i].user_image_pages = 0;
        g_processes[i].user_image_capacity = 0;
        g_processes[i].user_page_tables = 0;
        g_processes[i].code_size = 0;
        g_processes[i].loaded = 0;
        g_processes[i].state = PROCESS_STATE_EMPTY;
        g_processes[i].exit_code = 0;
        g_processes[i].runtime_ticks = 0;
        g_processes[i].switch_count = 0;
        g_processes[i].preempt_count = 0;
        g_processes[i].wake_tick = 0;
        g_processes[i].pending_read_fd = -1;
        g_processes[i].pending_read_buffer = 0;
        g_processes[i].pending_read_count = 0;
        /* 16-byte aligned top of this process's private kernel stack. */
        g_processes[i].kernel_stack_top =
            (uintptr_t)(g_kernel_stacks[i] + PROCESS_KERNEL_STACK_SIZE);
        g_processes[i].kresume_rsp = 0;
        g_processes[i].is_thread = 0;
        process_reset_vfs_handles(&g_processes[i]);
        g_processes[i].cwd[0] = '/';
        g_processes[i].cwd[1] = '\0';
        g_processes[i].spawn_arg[0] = '\0';
        g_processes[i].name[0] = '\0';
    }
}

static int process_try_blocking_tty_read(struct process *process, struct interrupt_frame *frame) {
    struct fd_entry *entry;
    struct tty *tty;

    if (process == 0 || frame == 0) {
        return 0;
    }
    if ((int)frame->rdi < 0 || (int)frame->rdi >= FD_TABLE_CAPACITY) {
        return 0;
    }

    entry = &process->fd_table.entries[(int)frame->rdi];
    if (!entry->used || entry->ops != &TTY_FD_OPS || entry->object == 0) {
        return 0;
    }

    tty = (struct tty *)entry->object;
    if (tty->line_ready || tty->raw_length > 0) {
        return 0;
    }

    process->context = *frame;
    process->pending_read_fd = (int)frame->rdi;
    process->pending_read_buffer = (void *)(uintptr_t)frame->rsi;
    process->pending_read_count = (size_t)frame->rdx;
    process->state = PROCESS_STATE_WAITING_IO;
    tty->waiter_pid = process->pid;
    g_kernel_resume_result = PROCESS_RUN_YIELDED;
    g_current_process = 0;
    return 1;
}

int process_spawn_demo_processes(uint32_t count) {
    uint32_t created = 0;
    uint32_t slot;
    const struct vfs_file *program = vfs_lookup("/bin/sh");

    if (program == 0) {
        return 0;
    }

    for (slot = 0; slot < PROCESS_MAX_COUNT && created < count; ++slot) {
        if (process_load_image_slot(&g_processes[slot], slot, program->data, (size_t)(program->end - program->data), 0, &PROCESS_STDIO_FD_OPS, 0, 0, "/bin/sh")) {
            ++created;
        }
    }

    if (created > 0u) {
        serial_write("VIBEOS: demo processes ready\n");
    }
    return (int)created;
}

int process_spawn_embedded_elf(const uint8_t *image, size_t image_size, const struct fd_ops *stdio_ops, void *stdio_object) {
    int slot;

    serial_write("VIBEOS: process_spawn_embedded_elf called\n");
    serial_write("VIBEOS: image=");
    serial_write_hex_u64((uint64_t)(uintptr_t)image);
    serial_write(" size=");
    serial_write_hex_u64(image_size);
    serial_write("\n");

    if (image == 0 || image_size == 0) {
        serial_write("VIBEOS: image or size is 0\n");
        return 0;
    }

    slot = process_find_free_slot();
    serial_write("VIBEOS: process_find_free_slot returned ");
    serial_write_hex_u64(slot);
    serial_write("\n");
    
    if (slot < 0) {
        serial_write("VIBEOS: no free slot\n");
        return 0;
    }

    serial_write("VIBEOS: calling process_load_image_slot...\n");
    if (!process_load_image_slot(&g_processes[slot], (uint32_t)slot, image, image_size, 0, stdio_ops, stdio_object, 0, "process")) {
        serial_write("VIBEOS: process_load_image_slot failed\n");
        return 0;
    }

    serial_write("VIBEOS: process spawned with PID ");
    serial_write_hex_u64(g_processes[slot].pid);
    serial_write("\n");

    journal_log(JOURNAL_INFO, g_processes[slot].pid, g_processes[slot].name[0] ? g_processes[slot].name : "spawned");
    return (int)g_processes[slot].pid;
}

int process_spawn_path(const char *path, const struct fd_ops *stdio_ops, void *stdio_object) {
    const struct vfs_file *file = vfs_lookup(path);

    if (file != 0) {
        int slot = process_find_free_slot();
        if (slot < 0) {
            return -SYSCALL_EINVAL;
        }
        if (!process_load_image_slot(&g_processes[slot], (uint32_t)slot, file->data, (size_t)(file->end - file->data), 0, stdio_ops, stdio_object, 0, path)) {
            return -SYSCALL_EINVAL;
        }
        return (int)g_processes[slot].pid;
    }

    /* Try ext2 */
    {
        struct ext2_filesystem *fs = vfs_get_ext2();
        uint32_t ino;
        struct ext2_inode inode;
        uint8_t *buf;
        ssize_t n;
        int result;

        if (fs == 0) return -SYSCALL_ENOENT;

        ino = ext2_lookup_inode(fs, path);
        if (ino == 0) return -SYSCALL_ENOENT;

        if (ext2_stat(fs, ino, &inode) < 0) return -SYSCALL_EIO;

        buf = (uint8_t *)kmalloc(inode.size);
        if (buf == 0) return -SYSCALL_ENOMEM;

        n = ext2_read(fs, ino, 0, inode.size, buf);
        if (n <= 0) { kfree(buf); return -SYSCALL_EIO; }

        {
            int slot = process_find_free_slot();
            if (slot < 0) {
                kfree(buf);
                return -SYSCALL_EINVAL;
            }
            result = process_load_image_slot(&g_processes[slot], (uint32_t)slot, buf, (size_t)n, 0, stdio_ops, stdio_object, 0, path)
                ? (int)g_processes[slot].pid
                : -SYSCALL_EINVAL;
        }
        kfree(buf);
        return result;
    }
}

/* Create a thread that shares `parent`'s address space (page tables + image).
 * The thread runs `entry(arg)` on `stack_top`, a stack the caller already owns
 * inside the shared address space (typically malloc'd from the user heap), so
 * no new kernel mapping is required. Returns the thread's tid (pid) or <0.
 *
 * Design note (SMP-ready): a thread is a full scheduler entity with its own
 * register context and kernel stack; only the address space is shared. When
 * VibeOS later gains multiple CPUs, two threads of one process can run on
 * different cores unchanged — the per-CPU work is in the scheduler/g_current,
 * not here. */
static int process_create_thread(struct process *parent, uintptr_t entry,
                                 uintptr_t stack_top, uint64_t arg) {
    struct process *thread;
    int slot;

    if (parent == 0 || entry == 0 || stack_top == 0) {
        return -SYSCALL_EINVAL;
    }

    slot = process_find_free_slot();
    if (slot < 0) {
        return -SYSCALL_EINVAL;   /* out of process/thread slots */
    }
    thread = &g_processes[slot];

    /* Share the parent's address space: same page tables, same virtual base,
     * same image allocation (borrowed — see is_thread handling in reap). */
    thread->user_page_tables       = parent->user_page_tables;
    thread->user_virtual_base      = parent->user_virtual_base;
    thread->user_image_allocation  = parent->user_image_allocation;
    thread->user_image_pages       = parent->user_image_pages;
    thread->user_image_capacity    = parent->user_image_capacity;
    thread->code_size              = parent->code_size;
    thread->is_thread              = 1;

    /* Independent execution context: entry point, caller-provided stack, arg. */
    thread->entry          = entry;
    thread->user_stack_top = stack_top;
    process_setup_context(thread);          /* sets rip=entry, rsp from stack_top */
    thread->context.rdi    = arg;           /* first argument to entry(arg)        */

    /* Own kernel stack for this slot (set once in process_init, kept across
     * reaps); re-assert it defensively. */
    thread->kernel_stack_top =
        (uintptr_t)(g_kernel_stacks[slot] + PROCESS_KERNEL_STACK_SIZE);
    thread->kresume_rsp = 0;

    /* Identity + scheduling state. */
    thread->pid             = ++g_next_pid;
    thread->parent_pid      = parent->pid;
    thread->wait_target_pid = 0;
    thread->loaded          = 1;
    thread->state           = PROCESS_STATE_READY;
    thread->exit_code       = 0;
    thread->runtime_ticks   = 0;
    thread->switch_count    = 0;
    thread->preempt_count   = 0;
    thread->wake_tick       = 0;
    thread->pending_read_fd = -1;
    thread->pending_read_buffer = 0;
    thread->pending_read_count  = 0;
    process_reset_vfs_handles(thread);

    /* Threads share the parent's working dir and inherit its open files. */
    {
        size_t k;
        for (k = 0; k + 1 < sizeof(thread->cwd) && parent->cwd[k]; ++k)
            thread->cwd[k] = parent->cwd[k];
        thread->cwd[k] = '\0';
    }
    thread->spawn_arg[0] = '\0';
    process_copy_fd_table(&thread->fd_table, &parent->fd_table);
    process_set_name(thread, parent->name);

    return (int)thread->pid;
}

static int process_spawn_named_from_parent(struct process *parent, const char *name) {
    const struct vfs_file *program;
    int slot;

    if (parent == 0 || name == 0) {
        return -SYSCALL_EINVAL;
    }

    slot = process_find_free_slot();
    if (slot < 0) {
        return -SYSCALL_EINVAL;
    }

    program = vfs_lookup(name);
    if (program != 0) {
        if (!process_load_image_slot(&g_processes[slot], (uint32_t)slot, program->data, (size_t)(program->end - program->data), &parent->fd_table, 0, 0, parent->pid, name)) {
            return -SYSCALL_EINVAL;
        }
        return (int)g_processes[slot].pid;
    }

    /* Try ext2 */
    {
        struct ext2_filesystem *fs = vfs_get_ext2();
        uint32_t ino;
        struct ext2_inode inode;
        uint8_t *buf;
        ssize_t n;
        int ok;

        if (fs == 0) return -SYSCALL_ENOENT;

        ino = ext2_lookup_inode(fs, name);
        if (ino == 0) return -SYSCALL_ENOENT;

        if (ext2_stat(fs, ino, &inode) < 0) return -SYSCALL_EIO;

        buf = (uint8_t *)kmalloc(inode.size);
        if (buf == 0) return -SYSCALL_ENOMEM;

        n = ext2_read(fs, ino, 0, inode.size, buf);
        if (n <= 0) { kfree(buf); return -SYSCALL_EIO; }

        ok = process_load_image_slot(&g_processes[slot], (uint32_t)slot, buf, (size_t)n, &parent->fd_table, 0, 0, parent->pid, name);
        kfree(buf);
        if (!ok) return -SYSCALL_EINVAL;
        return (int)g_processes[slot].pid;
    }
}

static int process_open_vfs_from_parent(struct process *process, const char *path) {
    const struct vfs_file *file;
    struct process_vfs_handle *handle;
    int fd;
    size_t i;

    if (process == 0 || path == 0) {
        return -SYSCALL_EINVAL;
    }

    /* Try embedded files first */
    file = vfs_lookup(path);
    if (file != 0) {
        handle = process_alloc_vfs_handle(process);
        if (handle == 0) return -SYSCALL_EMFILE;
        handle->file = file;
        handle->kind = 0;
        handle->path[0] = '\0';
        fd = fd_alloc(&process->fd_table, &PROCESS_VFS_FD_OPS, handle, 3);
        if (fd < 0) { process_release_vfs_handle(handle); return fd; }
        return fd;
    }

    /* Try user files and ext2 via vfs_file_exists */
    if (vfs_file_exists(path)) {
        handle = process_alloc_vfs_handle(process);
        if (handle == 0) return -SYSCALL_EMFILE;
        handle->file = 0;
        handle->kind = 1;
        for (i = 0; i + 1 < sizeof(handle->path) && path[i]; ++i)
            handle->path[i] = path[i];
        handle->path[i] = '\0';
        fd = fd_alloc(&process->fd_table, &PROCESS_VFS_FD_OPS, handle, 3);
        if (fd < 0) { process_release_vfs_handle(handle); return fd; }
        return fd;
    }

    return -SYSCALL_ENOENT;
}

static int process_close_fd(struct process *process, int fd) {
    struct fd_entry *entry;
    int result;

    if (process == 0 || fd < 0 || fd >= FD_TABLE_CAPACITY) {
        return -SYSCALL_EINVAL;
    }

    entry = &process->fd_table.entries[fd];
    if (!entry->used) {
        return -SYSCALL_EBADF;
    }

    if (entry->ops == &PROCESS_VFS_FD_OPS) {
        process_release_vfs_handle((struct process_vfs_handle *)entry->object);
    }

    result = fd_close(&process->fd_table, fd);
    return result;
}

static int process_stat_path(const char *path, uint64_t *kind_out, uint64_t *size_out) {
    struct vfs_stat stat;

    if (!vfs_stat_path(path, &stat)) {
        return -SYSCALL_ENOENT;
    }

    *kind_out = stat.kind;
    *size_out = stat.size;
    return 0;
}

static int process_readdir_path(const char *path, uint32_t index, char *name_out, size_t name_capacity, uint64_t *kind_out, uint64_t *size_out) {
    struct vfs_dir_entry entry;
    serial_write("SYSCALL: readdir path=");
    serial_write(path);
    serial_write(" index=");
    serial_write_hex_u64(index);
    serial_write(" name_out=");
    serial_write_hex_u64((uint64_t)(uintptr_t)name_out);
    serial_write(" capacity=");
    serial_write_hex_u64(name_capacity);
    serial_write("\n");

    if (!vfs_readdir(path, index, &entry)) {
        serial_write("SYSCALL: readdir returns 0\n");
        return 0;
    }
    serial_write("SYSCALL: readdir got name=");
    serial_write(entry.name);
    serial_write(" kind=");
    serial_write_hex_u64(entry.kind);
    serial_write(" size=");
    serial_write_hex_u64(entry.size);
    serial_write("\n");

    serial_write("SYSCALL: calling process_write_user_string name_out=");
    serial_write_hex_u64((uint64_t)(uintptr_t)name_out);
    serial_write("\n");
    if (!process_write_user_string(name_out, name_capacity, entry.name)) {
        serial_write("SYSCALL: process_write_user_string FAILED\n");
        return -SYSCALL_EINVAL;
    }
    serial_write("SYSCALL: process_write_user_string succeeded\n");

    *kind_out = entry.kind;
    *size_out = entry.size;
    return 1;
}

/* FXSAVE/FXRSTOR the 512-byte legacy FP/SSE area. Inline asm so the kernel's
 * -mgeneral-regs-only build (which never touches XMM itself) can still preserve
 * each userspace process's floating-point/SSE registers across switches. */
static inline void fpu_save(void *area) { __asm__ volatile("fxsave (%0)" :: "r"(area) : "memory"); }
static inline void fpu_restore(const void *area) { __asm__ volatile("fxrstor (%0)" :: "r"(area) : "memory"); }

int process_run_ready_slice(void) {
    struct process *process = process_pick_next_ready();
    int result;

    if (process == 0) {
        return PROCESS_RUN_NONE;
    }

    g_current_process = process;
    process->state = PROCESS_STATE_RUNNING;
    ++process->switch_count;
    paging_activate_user_table(process->user_virtual_base, process->user_page_tables, PROCESS_USER_PAGE_TABLE_COUNT);

    /* Traps from this process land on its own kernel stack. */
    interrupt_set_kernel_stack(process->kernel_stack_top);

    fpu_restore(process->fpu_state);          /* load this process's SSE state */

    if (process->kresume_rsp != 0) {
        /* The process is parked in the middle of a blocking syscall: continue
         * it on its own kernel stack rather than re-entering user mode. */
        uintptr_t parked = process->kresume_rsp;
        process->kresume_rsp = 0;
        g_kernel_resume_result = PROCESS_RUN_NONE;
        result = process_resume_blocked(parked);
    } else {
        /* Fresh time slice: iretq into user mode at the saved context. */
        g_kernel_resume_result = PROCESS_RUN_NONE;
        result = process_run_slice(&process->context);
    }
    fpu_save(process->fpu_state);             /* save whatever it left behind */
    return result;
}

/* Suspend the running process from inside a blocking kernel operation, letting
 * the scheduler run the desktop and other processes, then resume here once the
 * process is rescheduled. The caller (e.g. a network wait loop) re-checks its
 * condition after this returns. Safe to call only while a process is current. */
void process_yield_blocking(void) {
    struct process *process = g_current_process;
    if (process == 0) {
        return;   /* no process context: caller must fall back to a busy poll */
    }
    process->state = PROCESS_STATE_WAITING_IO;
    g_kernel_resume_result = PROCESS_RUN_BLOCKED;
    /* Park our kernel stack and switch to the scheduler. Returns here when the
     * scheduler resumes us via process_run_ready_slice / process_resume_blocked. */
    process_block_current(&process->kresume_rsp);
    /* Resumed: the scheduler already restored RUNNING state and our page
     * tables / FPU. Re-assert current for any code that reads it. */
    g_current_process = process;
}

int process_has_current(void) {
    return g_current_process != 0;
}

uint32_t process_current_pid(void) {
    return g_current_process ? g_current_process->pid : 0u;
}

int syscall_handle_interrupt(struct interrupt_frame *frame) {
    struct process *process = g_current_process;
    uint64_t number;

    if (process == 0 || frame == 0) {
        g_kernel_resume_result = PROCESS_RUN_EXITED;
        g_current_process = 0;
        return 1;
    }

    number = frame->rax;
    if (number == SYS_YIELD) {
        process->context = *frame;
        process->context.rax = 0;
        process->state = PROCESS_STATE_READY;
        g_kernel_resume_result = PROCESS_RUN_YIELDED;
        g_current_process = 0;
        return 1;
    }

    if (number == SYS_EXIT) {
        struct desktop_state *desktop = desktop_active();
        if (desktop != 0) {
            desktop_app_close_for_pid(desktop, process->pid);
        }
        process->exit_code = (int32_t)frame->rdi;
        process->state = PROCESS_STATE_EXITED;
        journal_log_hex(JOURNAL_INFO, process->pid, "process exited, code=", (uint64_t)frame->rdi);
        process_wake_waiter(process);
        g_kernel_resume_result = PROCESS_RUN_EXITED;
        g_current_process = 0;
        return 1;
    }

    if (number == SYS_PROCESS_SPAWN) {
        char name[32];
        int child_pid;

        if (!process_copy_user_string(name, sizeof(name), (const char *)(uintptr_t)frame->rdi)) {
            frame->rax = (uint64_t)(-SYSCALL_EINVAL);
            return 0;
        }

        child_pid = process_spawn_named_from_parent(process, name);

        /* Optional: pass spawn_arg via rsi */
        if (child_pid > 0 && frame->rsi != 0) {
            struct process *child = process_find_by_pid((uint32_t)child_pid);
            if (child != 0) {
                size_t k;
                char arg[64] = "";
                process_copy_user_string(arg, sizeof(arg), (const char *)(uintptr_t)frame->rsi);
                for (k = 0; k + 1 < sizeof(child->spawn_arg) && arg[k]; ++k)
                    child->spawn_arg[k] = arg[k];
                child->spawn_arg[k] = '\0';
            }
        }

        frame->rax = (uint64_t)child_pid;
        return 0;
    }

    if (number == SYS_THREAD_CREATE) {
        /* rdi = entry fn (user ptr), rsi = stack top (user ptr), rdx = arg. */
        int tid = process_create_thread(process, (uintptr_t)frame->rdi,
                                        (uintptr_t)frame->rsi, frame->rdx);
        frame->rax = (uint64_t)(int64_t)tid;
        return 0;
    }

    if (number == SYS_READ) {
        if (process_try_blocking_tty_read(process, frame)) {
            return 1;
        }
    }

    if (number == SYS_OPEN) {
        char path[64];

        if (!process_copy_user_string(path, sizeof(path), (const char *)(uintptr_t)frame->rdi)) {
            frame->rax = (uint64_t)(-SYSCALL_EINVAL);
            return 0;
        }

        frame->rax = (uint64_t)process_open_vfs_from_parent(process, path);
        return 0;
    }

    if (number == SYS_CLOSE) {
        frame->rax = (uint64_t)process_close_fd(process, (int)frame->rdi);
        return 0;
    }

    if (number == SYS_STAT) {
        char path[64];
        uint64_t kind = 0;
        uint64_t size = 0;
        int result;

        if (!process_copy_user_string(path, sizeof(path), (const char *)(uintptr_t)frame->rdi)) {
            frame->rax = (uint64_t)(-SYSCALL_EINVAL);
            return 0;
        }

        result = process_stat_path(path, &kind, &size);
        frame->rax = (uint64_t)result;
        frame->rdx = kind;
        frame->r8 = size;
        return 0;
    }

    if (number == SYS_READDIR) {
        char path[64];
        uint64_t kind = 0;
        uint64_t size = 0;
        int result;

        if (!process_copy_user_string(path, sizeof(path), (const char *)(uintptr_t)frame->rdi)) {
            frame->rax = (uint64_t)(-SYSCALL_EINVAL);
            return 0;
        }

        result = process_readdir_path(path, (uint32_t)frame->rsi, (char *)(uintptr_t)frame->rdx, (size_t)frame->r10, &kind, &size);
        frame->rax = (uint64_t)result;
        frame->rdx = kind;
        frame->r8 = size;
        return 0;
    }

    if (number == SYS_WAITPID) {
        uint32_t wait_pid = (uint32_t)frame->rdi;
        struct process *child = process_find_exited_child(process->pid, wait_pid);

        if (child != 0) {
            frame->rax = child->pid;
            frame->rdx = (uint64_t)(uint32_t)child->exit_code;
            process_reap(child);
            return 0;
        }

        process->context = *frame;
        process->context.rax = 0;
        process->context.rdx = 0;
        process->wait_target_pid = wait_pid;
        process->state = PROCESS_STATE_WAITING;
        g_kernel_resume_result = PROCESS_RUN_YIELDED;
        g_current_process = 0;
        return 1;
    }

    if (number == SYS_TIMER_SLEEP) {
        uint64_t sleep_ticks = frame->rdi;

        process->context = *frame;
        process->context.rax = 0;
        process->wake_tick = timer_tick_count() + (sleep_ticks == 0 ? 1u : sleep_ticks);
        process->state = PROCESS_STATE_SLEEPING;
        g_kernel_resume_result = PROCESS_RUN_YIELDED;
        g_current_process = 0;
        return 1;
    }

    if (number == SYS_WINDOWMGR_START) {
        g_window_manager_requested = 1;
        frame->rax = 0;
        return 0;
    }

    if (number == SYS_DISPLAY_MODE) {
        /* rdi=width, rsi=height. width==0 => return current packed (w<<16|h),
         * else request a resolution change. */
        uint32_t rw = (uint32_t)frame->rdi;
        uint32_t rh = (uint32_t)frame->rsi;
        if (rw == 0) {
            frame->rax = (uint64_t)kernel_current_resolution();
        } else {
            kernel_request_resolution(rw, rh);
            frame->rax = 0;
        }
        return 0;
    }

    if (number == SYS_PROCESS_SNAPSHOT) {
        struct process_snapshot *out = (struct process_snapshot *)(uintptr_t)frame->rsi;
        if (out == 0) {
            frame->rax = (uint64_t)(-SYSCALL_EINVAL);
            return 0;
        }
        frame->rax = (uint64_t)(int64_t)process_get_snapshot((uint32_t)frame->rdi, out);
        return 0;
    }

    if (number == SYS_PROCESS_KILL) {
        int result = process_kill((uint32_t)frame->rdi);
        frame->rax = (uint64_t)(int64_t)result;
        if (process == 0 || g_current_process == 0) {
            return 1;
        }
        return 0;
    }

    if (number == SYS_WINDOW_CREATE) {
        /* rdi = title, rsi = width, rdx = height. Returns window id or <0. */
        struct desktop_state *d = desktop_active();
        char title[64];
        if (d == 0) {
            frame->rax = (uint64_t)(-SYSCALL_EPERM); /* GUI not running */
            return 0;
        }
        if (!process_copy_user_string(title, sizeof(title), (const char *)(uintptr_t)frame->rdi)) {
            title[0] = '\0';
        }
        frame->rax = (uint64_t)(int64_t)desktop_app_create(d, process->pid, title, (int)frame->rsi, (int)frame->rdx);
        return 0;
    }

    if (number == SYS_WINDOW_PRESENT) {
        /* rdi = win id, rsi = pixel buffer (XRGB), rdx = w, r10 = h. */
        struct desktop_state *d = desktop_active();
        if (d == 0) {
            frame->rax = (uint64_t)(-SYSCALL_EPERM);
            return 0;
        }
        frame->rax = (uint64_t)(int64_t)desktop_app_present(d, process->pid, (int)frame->rdi,
            (const uint32_t *)(uintptr_t)frame->rsi, (int)frame->rdx, (int)frame->r10);
        return 0;
    }

    if (number == SYS_EVENT_POLL) {
        /* rdi = win id, rsi = struct winsys_event* (user). Returns 1/0. */
        struct desktop_state *d = desktop_active();
        struct winsys_event *out = (struct winsys_event *)(uintptr_t)frame->rsi;
        if (d == 0 || out == 0) {
            frame->rax = 0;
            return 0;
        }
        frame->rax = (uint64_t)(int64_t)desktop_app_poll_event(d, process->pid, (int)frame->rdi, out);
        return 0;
    }

    if (number == SYS_WINDOW_SET_MENU) {
        /* rdi = win id, rsi = struct winsys_menu_item* (user), rdx = count.
         * The calling app's address space is active, so the array is directly
         * readable; desktop_app_set_menu copies it into kernel storage. */
        struct desktop_state *d = desktop_active();
        const struct winsys_menu_item *items = (const struct winsys_menu_item *)(uintptr_t)frame->rsi;
        if (d == 0) {
            frame->rax = (uint64_t)(-SYSCALL_EPERM);
            return 0;
        }
        frame->rax = (uint64_t)(int64_t)desktop_app_set_menu(d, process->pid, (int)frame->rdi, items, (int)frame->rdx);
        return 0;
    }

    if (number == SYS_NET_INFO) {
        /* rdi = user pointer to struct net_info (filled in place). The calling
         * process's address space is active here, so the write lands correctly. */
        struct net_info *out = (struct net_info *)(uintptr_t)frame->rdi;
        if (out == 0) {
            frame->rax = (uint64_t)(-SYSCALL_EINVAL);
            return 0;
        }
        net_get_info(out);
        frame->rax = 0;
        return 0;
    }

    if (number == SYS_NET_HTTP_GET) {
        /* rdi -> user struct { u32 ip; u16 port; const char* host; const char*
         * path; char* out; int cap; }. Returns bytes received or negative. */
        struct http_req {
            uint32_t ip;
            uint16_t port;
            const char *host;
            const char *path;
            char *out;
            int cap;
        };
        struct http_req *r = (struct http_req *)(uintptr_t)frame->rdi;
        char host[128];
        char path[256];

        if (r == 0 || r->out == 0 || r->cap <= 0) {
            frame->rax = (uint64_t)(-SYSCALL_EINVAL);
            return 0;
        }
        if (!process_copy_user_string(host, sizeof(host), r->host) ||
            !process_copy_user_string(path, sizeof(path), r->path)) {
            frame->rax = (uint64_t)(-SYSCALL_EINVAL);
            return 0;
        }
        net_lock();
        frame->rax = (uint64_t)(int64_t)net_http_get(r->ip, r->port, host, path, r->out, r->cap, 5000);
        net_unlock();
        return 0;
    }

    if (number == SYS_NET_HTTPS_GET) {
        /* Same user struct as SYS_NET_HTTP_GET, but over TLS (BearSSL). */
        struct http_req {
            uint32_t ip;
            uint16_t port;
            const char *host;
            const char *path;
            char *out;
            int cap;
        };
        struct http_req *r = (struct http_req *)(uintptr_t)frame->rdi;
        char host[128];
        char path[256];

        if (r == 0 || r->out == 0 || r->cap <= 0) {
            frame->rax = (uint64_t)(-SYSCALL_EINVAL);
            return 0;
        }
        if (!process_copy_user_string(host, sizeof(host), r->host) ||
            !process_copy_user_string(path, sizeof(path), r->path)) {
            frame->rax = (uint64_t)(-SYSCALL_EINVAL);
            return 0;
        }
        net_lock();
        frame->rax = (uint64_t)(int64_t)net_https_get(r->ip, r->port, host, path, r->out, r->cap, 15000);
        net_unlock();
        return 0;
    }

    if (number == SYS_JOURNAL_READ) {
        /* rdi = seq, rsi = struct journal_entry* (user). Returns 1/0; if rsi==0,
         * returns the total record count so callers can find the tail. */
        struct journal_entry *out = (struct journal_entry *)(uintptr_t)frame->rsi;
        if (out == 0) {
            frame->rax = (uint64_t)journal_total();
            return 0;
        }
        frame->rax = (uint64_t)(int64_t)journal_get(frame->rdi, out);
        return 0;
    }

    if (number == SYS_LOG) {
        /* rdi = level, rsi = user message string. Logs under the caller's pid. */
        char msg[JOURNAL_MSG_MAX];
        enum journal_level lvl = (enum journal_level)frame->rdi;
        if (lvl > JOURNAL_APP) lvl = JOURNAL_APP;
        if (!process_copy_user_string(msg, sizeof(msg), (const char *)(uintptr_t)frame->rsi)) {
            frame->rax = (uint64_t)(-SYSCALL_EINVAL);
            return 0;
        }
        journal_log(lvl, process->pid, msg);
        frame->rax = 0;
        return 0;
    }

    if (number == SYS_NET_RESOLVE) {
        /* rdi = user hostname string, rsi = user uint32_t* for result ip.
         * Returns 0 on success, negative on failure. */
        char host[128];
        uint32_t ip = 0;
        uint32_t *out = (uint32_t *)(uintptr_t)frame->rsi;

        if (!process_copy_user_string(host, sizeof(host), (const char *)(uintptr_t)frame->rdi)) {
            frame->rax = (uint64_t)(-SYSCALL_EINVAL);
            return 0;
        }
        if (out == 0) {
            frame->rax = (uint64_t)(-SYSCALL_EINVAL);
            return 0;
        }
        net_lock();
        if (net_resolve(host, &ip, 3000)) {
            *out = ip;
            frame->rax = 0;
        } else {
            frame->rax = (uint64_t)(-SYSCALL_ENOENT);
        }
        net_unlock();
        return 0;
    }

    if (number == SYS_NET_PING) {
        /* rdi = dst ip (host order), rsi = count, rdx = timeout ms,
         * r10 = optional user int* for last RTT. Returns reply count. */
        uint32_t dst = (uint32_t)frame->rdi;
        int count = (int)frame->rsi;
        int timeout_ms = (int)frame->rdx;
        int *rtt_out = (int *)(uintptr_t)frame->r10;
        int rtt = -1;
        int replies;

        if (count <= 0) count = 1;
        if (timeout_ms <= 0) timeout_ms = 1000;
        net_lock();
        replies = net_ping(dst, count, timeout_ms, &rtt);
        net_unlock();
        if (rtt_out != 0) {
            *rtt_out = rtt;
        }
        frame->rax = (uint64_t)(int64_t)replies;
        return 0;
    }

    if (number == SYS_CHDIR) {
        char path[256];
        size_t i;
        struct vfs_stat stat;
        int stat_ok;

        serial_write("SYS_CHDIR: called rdi=");
        serial_write_hex_u64(frame->rdi);
        serial_write("\n");

        serial_write("SYS_CHDIR: first 16 bytes at rdi: ");
        for (i = 0; i < 16; i++) {
            char c = ((const char *)(uintptr_t)frame->rdi)[i];
            serial_write_hex_u64((uint8_t)c);
            serial_write(" ");
        }
        serial_write("\n");

        if (!process_copy_user_string(path, sizeof(path), (const char *)(uintptr_t)frame->rdi)) {
            serial_write("SYS_CHDIR: copy_user_string failed\n");
            frame->rax = (uint64_t)(-SYSCALL_EINVAL);
            return 0;
        }

        serial_write("SYS_CHDIR: path=");
        serial_write(path);
        serial_write("\n");

        if (path[0] != '/') {
            serial_write("SYS_CHDIR: not absolute, EINVAL\n");
            frame->rax = (uint64_t)(-SYSCALL_EINVAL);
            return 0;
        }

        stat_ok = vfs_stat_path(path, &stat);
        serial_write("SYS_CHDIR: stat_ok=");
        serial_write_hex_u64(stat_ok);
        serial_write(" kind=");
        serial_write_hex_u64(stat.kind);
        serial_write("\n");

        if (stat_ok && stat.kind == VFS_NODE_DIRECTORY) {
            for (i = 0; i < sizeof(process->cwd) - 1 && path[i] != '\0'; i++) {
                process->cwd[i] = path[i];
            }
            process->cwd[i] = '\0';
            serial_write("SYS_CHDIR: success, cwd=");
            serial_write(process->cwd);
            serial_write("\n");
            frame->rax = 0;
        } else {
            serial_write("SYS_CHDIR: ENOENT\n");
            frame->rax = (uint64_t)(-SYSCALL_ENOENT);
        }
        return 0;
    }

	if (number == SYS_GETCWD) {
		char *buf = (char *)(uintptr_t)frame->rdi;
		size_t size = (size_t)frame->rsi;
		size_t cwd_len = 0;
		size_t i;

		if (buf == 0 || size == 0) {
			frame->rax = (uint64_t)(-SYSCALL_EINVAL);
			return 0;
		}

		while (process->cwd[cwd_len] != '\0' && cwd_len < sizeof(process->cwd)) {
			cwd_len++;
		}

		if (cwd_len + 1 > size) {
			frame->rax = (uint64_t)(-SYSCALL_EINVAL);
			return 0;
		}

		for (i = 0; i <= cwd_len && i < size; i++) {
			buf[i] = process->cwd[i];
		}

		frame->rax = (uint64_t)(uintptr_t)buf;
		return 0;
	}

	if (number == SYS_CREAT) {
		char path[64];
		if (!process_copy_user_string(path, sizeof(path), (const char *)(uintptr_t)frame->rdi)) {
			frame->rax = (uint64_t)(-SYSCALL_EINVAL);
			return 0;
		}
		serial_write("SYS_CREAT: path=");
		serial_write(path);
		serial_write("\n");
		frame->rax = (uint64_t)vfs_create(path);
		return 0;
	}

	if (number == SYS_UNLINK) {
		char path[64];
		if (!process_copy_user_string(path, sizeof(path), (const char *)(uintptr_t)frame->rdi)) {
			frame->rax = (uint64_t)(-SYSCALL_EINVAL);
			return 0;
		}
		frame->rax = (uint64_t)vfs_unlink(path);
		return 0;
	}

	if (number == SYS_MKDIR) {
		char path[64];
		if (!process_copy_user_string(path, sizeof(path), (const char *)(uintptr_t)frame->rdi)) {
			frame->rax = (uint64_t)(-SYSCALL_EINVAL);
			return 0;
		}
		frame->rax = (uint64_t)vfs_mkdir(path);
		return 0;
	}

	if (number == SYS_GETARG) {
		char *buf = (char *)(uintptr_t)frame->rdi;
		size_t cap = (size_t)frame->rsi;
		if (buf != 0 && cap > 0) {
			process_write_user_string(buf, cap, process->spawn_arg);
		}
		frame->rax = 0;
		return 0;
	}

	if (number == SYS_WRITE_FILE) {
		char path[64];
		const void *buf;
		size_t size;
		if (!process_copy_user_string(path, sizeof(path), (const char *)(uintptr_t)frame->rdi)) {
			frame->rax = (uint64_t)(-SYSCALL_EINVAL);
			return 0;
		}
		buf = (const void *)(uintptr_t)frame->rsi;
		size = (size_t)frame->rdx;
		frame->rax = (uint64_t)vfs_write_all(path, buf, size);
		return 0;
	}

	frame->rax = (uint64_t)syscall_dispatch(&process->syscalls, number, frame->rdi, frame->rsi, frame->rdx, frame->r10, frame->r8, frame->r9);
	return 0;
}

int timer_handle_interrupt(struct interrupt_frame *frame) {
    struct process *process = g_current_process;

    timer_tick();
    process_wake_ready(timer_tick_count());
    timer_acknowledge_irq();

    if (frame == 0 || process == 0) {
        return 0;
    }

    if ((frame->cs & 0x03u) != 0x03u) {
        return 0;
    }

    ++process->runtime_ticks;
    ++process->preempt_count;
    process->context = *frame;
    process->state = PROCESS_STATE_READY;
    g_kernel_resume_result = PROCESS_RUN_YIELDED;
    g_current_process = 0;
    return 1;
}

uint32_t process_loaded_count(void) {
    uint32_t count = 0;
    size_t i;

    for (i = 0; i < PROCESS_MAX_COUNT; ++i) {
        if (g_processes[i].loaded && g_processes[i].state != PROCESS_STATE_EXITED) {
            ++count;
        }
    }

    return count;
}

uint32_t process_ready_count(void) {
    uint32_t count = 0;
    size_t i;

    for (i = 0; i < PROCESS_MAX_COUNT; ++i) {
        if (g_processes[i].state == PROCESS_STATE_READY) {
            ++count;
        }
    }

    return count;
}

uint32_t process_running_count(void) {
    uint32_t count = 0;
    size_t i;

    for (i = 0; i < PROCESS_MAX_COUNT; ++i) {
        if (g_processes[i].state == PROCESS_STATE_RUNNING) {
            ++count;
        }
    }

    return count;
}

uint32_t process_sleeping_count(void) {
    uint32_t count = 0;
    size_t i;

    for (i = 0; i < PROCESS_MAX_COUNT; ++i) {
        if (g_processes[i].state == PROCESS_STATE_SLEEPING) {
            ++count;
        }
    }

    return count;
}

uint32_t process_snapshot_count(void) {
    return PROCESS_MAX_COUNT;
}

int process_get_snapshot(uint32_t slot, struct process_snapshot *snapshot) {
    const struct process *process;

    if (snapshot == 0 || slot >= PROCESS_MAX_COUNT) {
        return 0;
    }

    process = &g_processes[slot];
    snapshot->pid = process->pid;
    snapshot->parent_pid = process->parent_pid;
    snapshot->runtime_ticks = process->runtime_ticks;
    snapshot->switch_count = process->switch_count;
    snapshot->preempt_count = process->preempt_count;
    snapshot->wake_tick = process->wake_tick;
    snapshot->state = process->state;
    snapshot->loaded = process->loaded;
    process_copy_name_to_snapshot(snapshot, process->name);

    /* Physical RAM footprint: the allocated ELF image (code+data+BSS, which
     * includes the userspace umalloc heap) plus the 1 MB user stack and the
     * 16 KB kernel stack. */
    snapshot->mem_bytes = (uint64_t)process->user_image_capacity
                        + (uint64_t)PROCESS_USER_STACK_SIZE
                        + (uint64_t)PROCESS_KERNEL_STACK_SIZE;

    /* Threads = process slots sharing this one's address space (page tables).
     * Today every process has its own tables, so this is 1; once userspace
     * threads share their parent's tables it counts them automatically. */
    {
        uint32_t threads = 0;
        size_t i;
        if (process->user_page_tables != 0) {
            for (i = 0; i < PROCESS_MAX_COUNT; ++i) {
                if (g_processes[i].loaded &&
                    g_processes[i].state != PROCESS_STATE_EXITED &&
                    g_processes[i].user_page_tables == process->user_page_tables) {
                    ++threads;
                }
            }
        }
        snapshot->thread_count = threads ? threads : 1u;
    }
    return 1;
}

static void process_clear_pending_tty_wait(struct process *process) {
    struct fd_entry *entry;
    struct tty *tty;

    if (process == 0 || process->pending_read_fd < 0 || process->pending_read_fd >= FD_TABLE_CAPACITY) {
        return;
    }

    entry = &process->fd_table.entries[process->pending_read_fd];
    if (!entry->used || entry->ops != &TTY_FD_OPS || entry->object == 0) {
        return;
    }

    tty = (struct tty *)entry->object;
    if (tty->waiter_pid == process->pid) {
        tty->waiter_pid = 0;
    }
    process->pending_read_fd = -1;
    process->pending_read_buffer = 0;
    process->pending_read_count = 0;
}

int process_handle_user_fault(uint64_t vector, uint64_t rip, uint64_t cr2, uint64_t error_code) {
    struct process *process = g_current_process;
    struct desktop_state *desktop;

    if (process == 0 || process->state == PROCESS_STATE_EMPTY || process->state == PROCESS_STATE_EXITED) {
        return 0;
    }

    serial_write("VIBEOS: terminating user process pid=");
    serial_write_hex_u64(process->pid);
    serial_write(" name=");
    serial_write(process->name[0] ? process->name : "process");
    serial_write(" after fault vector=");
    serial_write_hex_u64(vector);
    serial_write(" rip=");
    serial_write_hex_u64(rip);
    serial_write(" cr2=");
    serial_write_hex_u64(cr2);
    serial_write(" err=");
    serial_write_hex_u64(error_code);
    serial_write("\n");

    process_clear_pending_tty_wait(process);
    desktop = desktop_active();
    if (desktop != 0) {
        desktop_app_close_for_pid(desktop, process->pid);
    }

    process->exit_code = -SYSCALL_EFAULT;
    process->state = PROCESS_STATE_EXITED;
    journal_log_hex(JOURNAL_FAULT, process->pid, "user fault, killed; rip=", rip);
    journal_log_hex(JOURNAL_FAULT, process->pid, "  cr2=", cr2);
    process_wake_waiter(process);
    g_kernel_resume_result = PROCESS_RUN_EXITED;
    g_current_process = 0;
    return 1;
}

int process_kill(uint32_t pid) {
    struct process *process = process_find_by_pid(pid);
    struct desktop_state *desktop;

    if (process == 0 || process->state == PROCESS_STATE_EMPTY) {
        return -SYSCALL_EINVAL;
    }
    if (process->state == PROCESS_STATE_EXITED) {
        return 0;
    }

    process_clear_pending_tty_wait(process);
    desktop = desktop_active();
    if (desktop != 0) {
        desktop_app_close_for_pid(desktop, pid);
    }

    process->exit_code = -1;
    process->state = PROCESS_STATE_EXITED;
    process_wake_waiter(process);
    if (g_current_process == process) {
        g_kernel_resume_result = PROCESS_RUN_EXITED;
        g_current_process = 0;
    }
    return 0;
}

int process_pid_alive(uint32_t pid) {
    struct process *process = process_find_by_pid(pid);

    if (process == 0) {
        return 0;
    }
    return process->state != PROCESS_STATE_EMPTY && process->state != PROCESS_STATE_EXITED;
}

void process_wake_tty_reader(struct tty *tty) {
    struct process *process;
    ssize_t read_result;

    serial_write("WAKE_TTY_READER: tty=");
    serial_write_hex_u64((uint64_t)(uintptr_t)tty);
    serial_write(" waiter_pid=");
    serial_write_hex_u64(tty ? tty->waiter_pid : 0);
    serial_write("\n");

    if (tty == 0 || tty->waiter_pid == 0) {
        return;
    }

    process = process_find_by_pid(tty->waiter_pid);
    tty->waiter_pid = 0;
    if (process == 0 || process->state != PROCESS_STATE_WAITING_IO) {
        serial_write("WAKE_TTY_READER: process not found or not WAITING_IO\n");
        return;
    }

    serial_write("WAKE_TTY_READER: waking PID=");
    serial_write_hex_u64(process->pid);
    serial_write(" read_buf=");
    serial_write_hex_u64((uint64_t)(uintptr_t)process->pending_read_buffer);
    serial_write("\n");

    /* pending_read_buffer is a user virtual address in this process's own
     * address space. Make that space live before writing the line into it,
     * otherwise the data would land in whichever process is currently mapped. */
    paging_activate_user_table(process->user_virtual_base, process->user_page_tables, PROCESS_USER_PAGE_TABLE_COUNT);

    read_result = fd_read(&process->fd_table, process->pending_read_fd, process->pending_read_buffer, process->pending_read_count);
    process->pending_read_fd = -1;
    process->pending_read_buffer = 0;
    process->pending_read_count = 0;
    process->context.rax = (uint64_t)read_result;
    process->state = PROCESS_STATE_READY;
}

int process_take_window_manager_request(void) {
    int requested = g_window_manager_requested != 0u;
    g_window_manager_requested = 0;
    return requested;
}
