/* VibeOS arm64 — Process management.
 *
 * Provides the process.h API that window.c and other shared kernel code
 * expect.  The arm64 implementation uses a simple process table with
 * one EL0 context per slot.  Virtual address layout per process:
 *
 *   VA 0x90000000 + (slot * 0x04000000)  — ELF image (up to 60 MB)
 *   VA 0x90000000 + (slot * 0x04000000) + 0x03F00000 — user stack (1 MB)
 *
 * Physical backing is VA - 0x40000000 (the EL0 alias mapping in mmu.c).
 * All processes share the same TTBR0_EL1 (cooperative single-address-space
 * model); the scheduler only saves/restores registers.  This is sufficient
 * for the window compositor + GUI apps.
 *
 * Scheduling: cooperative.  Each process runs until it calls SYS_YIELD or
 * SYS_EXIT.  The compositor (kernel main loop) calls process_run_ready_slice()
 * which picks the next READY process and switches to it.
 */
#include "arch.h"
#include "../../include/alloc.h"
#include "../../include/arch_frame.h"
#include "../../include/elf.h"
#include "../../include/ext2_fs.h"
#include "../../include/process.h"
#include "../../include/serial.h"
#include "../../include/string.h"

/* ---- Constants -------------------------------------------------------- */
#define ARM64_MAX_PROCS     16
#define ARM64_SLOT_SIZE     0x04000000ULL   /* 64 MB per process */
#define ARM64_USER_BASE     0x90000000ULL   /* first process VA */
#define ARM64_STACK_SIZE    0x00100000ULL   /* 1 MB stack at top of slot */
#define ARM64_ALIAS_OFF     0x40000000ULL   /* user VA to kernel PA */

/* ---- Saved user context (what we save/restore on switch) -------------- */
struct arm64_user_context {
    uint64_t x[31];      /* x0–x30 */
    uint64_t sp_el0;
    uint64_t elr_el1;
    uint64_t spsr_el1;
};

/* ---- Process table ---------------------------------------------------- */
struct process g_procs[ARM64_MAX_PROCS];
static uint32_t      g_next_pid = 1;
int           g_current  = -1;   /* index of currently-running process, -1 = kernel */

/* ---- Helpers ---------------------------------------------------------- */
static uint64_t slot_va(uint32_t slot) {
    return ARM64_USER_BASE + (uint64_t)slot * ARM64_SLOT_SIZE;
}

static uint64_t slot_stack_top(uint32_t slot) {
    return slot_va(slot) + ARM64_SLOT_SIZE - 16;  /* 16-byte aligned */
}

/* Convert a user VA to physical address (kernel view) */
static uint8_t *user_to_pa(uint64_t va) {
    return (uint8_t *)(uintptr_t)(va - ARM64_ALIAS_OFF);
}

/* ---- Init ------------------------------------------------------------- */
void process_init(void) {
    memset(g_procs, 0, sizeof(g_procs));
    g_next_pid = 1;
    g_current  = -1;
    serial_write("[process] init done\r\n");
}

/* ---- Find free slot --------------------------------------------------- */
static int find_free_slot(void) {
    for (int i = 0; i < ARM64_MAX_PROCS; i++)
        if (!g_procs[i].loaded) return i;
    return -1;
}

static struct process *process_by_pid(uint32_t pid) {
    for (int i = 0; i < ARM64_MAX_PROCS; i++)
        if (g_procs[i].loaded && g_procs[i].pid == pid)
            return &g_procs[i];
    return 0;
}

/* ---- Spawn a process from an ELF on disk ------------------------------ */
int process_spawn_path(const char *path,
                       const struct fd_ops *stdio_ops, void *stdio_object,
                       uint32_t uid, uint32_t gid) {
    (void)stdio_ops; (void)stdio_object;
    if (!path || !*path) return -1;

    int slot = find_free_slot();
    if (slot < 0) {
        serial_write("[process] no free slot\r\n");
        return -1;
    }

    /* Look up the ELF in ext2 */
    extern struct ext2_filesystem g_fs;
    extern int g_fs_ready;  /* from arch.c */
    if (!g_fs_ready) return -1;

    uint32_t ino = ext2_lookup_inode(&g_fs, path);
    if (!ino) {
        serial_write("[process] not found: ");
        serial_write(path);
        serial_write("\r\n");
        return -1;
    }

    struct ext2_inode *node = &g_fs.inode_table[ino - 1];
    uint32_t fsize = node->size;
    if (fsize == 0 || fsize > (60u << 20)) {
        serial_write("[process] bad size\r\n");
        return -1;
    }

    uint8_t *elf = (uint8_t *)kmalloc(fsize);
    if (!elf) { serial_write("[process] OOM\r\n"); return -1; }

    ssize_t got = ext2_read(&g_fs, ino, 0, fsize, elf);
    if (got <= 0) { serial_write("[process] read error\r\n"); kfree(elf); return -1; }

    if (!elf64_validate(elf, (size_t)got)) {
        serial_write("[process] not valid aarch64 ELF\r\n");
        kfree(elf); return -1;
    }

    struct elf64_header *eh = (struct elf64_header *)elf;
    struct elf64_program_header *ph =
        (struct elf64_program_header *)(elf + eh->program_header_offset);

    /* All arm64 apps are linked at fixed VA 0x90000000 (non-PIE).
     * Since only one EL0 process runs at a time (cooperative model),
     * we reuse the same VA for all processes — each spawn overwrites
     * the previous one's memory at PA 0x50000000. */
    uint64_t base_va = ARM64_USER_BASE;

    /* Copy each PT_LOAD segment to PA = vaddr - ALIAS_OFF.
     * The ELF is linked for VA 0x90000000 (see user/arm64/link.ld).
     * We need to relocate: the segments claim VA 0x90000000, but the
     * actual slot VA is base_va.  Compute the delta. */
    /* No relocation needed: base_va == elf_base == 0x90000000.
     * Apps are linked at this exact address. */
    uint64_t delta = 0;

    for (int i = 0; i < eh->program_header_count; i++) {
        if (ph[i].type != ELF_PROGRAM_TYPE_LOAD) continue;
        uint64_t vaddr = ph[i].virtual_address + delta;
        uint64_t memsz = ph[i].memory_size;
        if (vaddr < base_va || vaddr + memsz > base_va + ARM64_SLOT_SIZE) {
            serial_write("[process] segment out of slot range\r\n");
            kfree(elf); return -1;
        }
        uint8_t *dst = user_to_pa(vaddr);
        size_t fsz = (size_t)ph[i].file_size;
        for (size_t b = 0; b < fsz; b++)
            dst[b] = elf[ph[i].offset + b];
        for (size_t b = fsz; b < (size_t)memsz; b++)
            dst[b] = 0;
    }

    /* I-cache flush for the newly written code */
    __asm__ volatile("dsb ish; ic ialluis; dsb ish; isb" ::: "memory");

    /* Set up process struct */
    struct process *proc = &g_procs[slot];
    proc->pid        = g_next_pid++;
    proc->parent_pid = 0;
    proc->loaded     = 1;
    proc->state      = PROCESS_STATE_READY;
    proc->uid        = uid;
    proc->gid        = gid;
    proc->entry      = eh->entry;  /* already at 0x90000000 */
    proc->user_stack_top = ARM64_USER_BASE + ARM64_SLOT_SIZE - 16;  /* top of slot 0 VA space */
    proc->user_virtual_base = ARM64_USER_BASE;
    proc->code_size  = fsize;
    proc->user_image_allocation = elf;  /* freed on kill */
    proc->cr3        = ARM64_USER_BASE;  /* all share slot 0 VA */

    /* Extract name from path (last component) */
    {
        const char *name = path;
        for (const char *s = path; *s; s++)
            if (*s == '/') name = s + 1;
        size_t ni = 0;
        for (; ni < sizeof(proc->name) - 1 && name[ni] && name[ni] != ' '; ni++)
            proc->name[ni] = name[ni];
        proc->name[ni] = '\0';
    }

    /* Clear saved context */
    memset(&proc->context, 0, sizeof(proc->context));

    serial_write("[process] spawn pid=");
    { char b[16]; int i=0; uint32_t t=proc->pid; if(!t)b[i++]='0';
      else while(t){b[i++]=(char)('0'+t%10);t/=10;}
      while(i--) serial_write_char(b[i]); }
    serial_write(" slot=");
    { char b[16]; int i=0; uint32_t t=(uint32_t)slot; if(!t)b[i++]='0';
      else while(t){b[i++]=(char)('0'+t%10);t/=10;}
      while(i--) serial_write_char(b[i]); }
    serial_write(" '"); serial_write(proc->name); serial_write("'\r\n");

    return (int)proc->pid;
}

/* ---- Kill a process --------------------------------------------------- */
int process_kill(uint32_t pid) {
    struct process *proc = process_by_pid(pid);
    if (!proc) return -1;

    serial_write("[process] kill pid=");
    { char b[16]; int i=0; uint32_t t=pid; if(!t)b[i++]='0';
      else while(t){b[i++]=(char)('0'+t%10);t/=10;}
      while(i--) serial_write_char(b[i]); }
    serial_write("\r\n");

    if (proc->user_image_allocation)
        kfree(proc->user_image_allocation);
    proc->loaded = 0;
    proc->state  = PROCESS_STATE_EXITED;

    if (g_current >= 0 && &g_procs[g_current] == proc)
        g_current = -1;

    return 0;
}

/* ---- Check if PID is alive -------------------------------------------- */
int process_pid_alive(uint32_t pid) {
    struct process *proc = process_by_pid(pid);
    return (proc && proc->loaded && proc->state != PROCESS_STATE_EXITED) ? 1 : 0;
}

/* ---- Snapshot API (for task manager) ---------------------------------- */
uint32_t process_snapshot_count(void) {
    uint32_t n = 0;
    for (int i = 0; i < ARM64_MAX_PROCS; i++)
        if (g_procs[i].loaded) n++;
    return n;
}

int process_get_snapshot(uint32_t idx, struct process_snapshot *snap) {
    uint32_t seen = 0;
    for (int i = 0; i < ARM64_MAX_PROCS; i++) {
        if (!g_procs[i].loaded) continue;
        if (seen != idx) { seen++; continue; }
        struct process *p = &g_procs[i];
        snap->pid          = p->pid;
        snap->parent_pid   = p->parent_pid;
        snap->state        = p->state;
        snap->loaded       = p->loaded;
        snap->uid          = p->uid;
        snap->runtime_ticks = 0;
        snap->switch_count  = 0;
        snap->preempt_count = 0;
        snap->wake_tick     = 0;
        snap->mem_bytes     = p->code_size;
        snap->thread_count  = 1;
        snap->is_thread     = 0;
        /* Copy name */
        for (int j = 0; j < 32; j++) {
            snap->name[j] = p->name[j];
            if (!p->name[j]) break;
        }
        return 1;
    }
    return 0;
}

/* ---- Syscall handler (delegates to the shared kernel dispatch) -------- */
/* Forward declare — actual handler is in arch.c, will be refactored later */
void arm64_syscall_dispatch(struct interrupt_frame *frame, struct process *proc);

/* ---- Enter user mode for a process ------------------------------------ */
/* Defined in usermode.S */
uint64_t arm64_enter_user(uint64_t entry, uint64_t sp, uint64_t arg0, uint64_t arg1);

/* ---- Run one process slice (cooperative) ------------------------------ */
int process_run_ready_slice(void) {
    /* Find the first READY process */
    int slot = -1;
    for (int i = 0; i < ARM64_MAX_PROCS; i++) {
        if (g_procs[i].loaded && g_procs[i].state == PROCESS_STATE_READY) {
            slot = i; break;
        }
    }
    if (slot < 0) return 0;  /* nothing ready */

    struct process *proc = &g_procs[slot];
    proc->state = PROCESS_STATE_RUNNING;
    g_current = slot;

    serial_write("[sched] run slot=");
    { char b[16]; int i=0; uint32_t t=(uint32_t)slot; if(!t)b[i++]='0';
      else while(t){b[i++]=(char)('0'+t%10);t/=10;}
      while(i--) serial_write_char(b[i]); }
    serial_write(" '"); serial_write(proc->name);
    serial_write("' entry=");
    serial_write_hex_u64(proc->entry);
    serial_write("\r\n");

    /* Enter EL0.  This function does not return — the process runs until
     * it makes a syscall that triggers a context switch or exits.
     * In the cooperative model, arm64_enter_user switches to EL0 and the
     * process runs; when it returns via SVC, the exception handler in
     * exceptions.S calls arm64_sync_handler_el0 which processes the syscall
     * and either returns to the process or switches to another. */
    arm64_enter_user(proc->entry, proc->user_stack_top, 0, 0);
}

/* ---- Called from the exception handler when a process exits ----------- */
void process_handle_exit(uint64_t code) {
    if (g_current >= 0) {
        struct process *proc = &g_procs[g_current];
        proc->exit_code = (int32_t)code;
        proc->state = PROCESS_STATE_EXITED;
        if (proc->user_image_allocation)
            kfree(proc->user_image_allocation);
        proc->loaded = 0;
        serial_write("[process] exit pid=");
        { char b[16]; int i=0; uint32_t t=proc->pid; if(!t)b[i++]='0';
          else while(t){b[i++]=(char)('0'+t%10);t/=10;}
          while(i--) serial_write_char(b[i]); }
        serial_write(" code=");
        serial_write_hex_u64(code);
        serial_write("\r\n");
        g_current = -1;
    }
    /* Return to kernel main loop */
    extern void arm64_return_to_kernel(uint64_t code) __attribute__((noreturn));
    arm64_return_to_kernel(code);
}

/* ---- Stubs for functions not yet implemented on arm64 ----------------- */
int process_has_current(void) { return g_current >= 0; }
uint32_t process_current_pid(void) {
    if (g_current >= 0) return g_procs[g_current].pid;
    return 0;
}
uint32_t process_current_uid(void) {
    if (g_current >= 0) return g_procs[g_current].uid;
    return 0;
}
uint32_t process_current_gid(void) {
    if (g_current >= 0) return g_procs[g_current].gid;
    return 0;
}
uint32_t process_loaded_count(void) { return process_snapshot_count(); }
uint32_t process_ready_count(void) {
    uint32_t n = 0;
    for (int i = 0; i < ARM64_MAX_PROCS; i++)
        if (g_procs[i].loaded && g_procs[i].state == PROCESS_STATE_READY) n++;
    return n;
}
uint32_t process_running_count(void) {
    uint32_t n = 0;
    for (int i = 0; i < ARM64_MAX_PROCS; i++)
        if (g_procs[i].loaded && g_procs[i].state == PROCESS_STATE_RUNNING) n++;
    return n;
}
uint32_t process_sleeping_count(void) { return 0; }

