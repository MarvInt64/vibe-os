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
#include "../../include/cpu.h"
#include "../../include/spinlock.h"

extern uint64_t timer_tick_count(void);

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
static spinlock_t    g_process_lock = SPINLOCK_INIT;

static inline int get_current_slot(void) {
    struct cpu *c = this_cpu();
    if (!c || !c->current) return -1;
    return (int)(c->current - g_procs);
}

static inline void set_current_slot(int slot) {
    struct cpu *c = this_cpu();
    if (c) c->current = (slot >= 0) ? &g_procs[slot] : (struct process *)0;
}

/* Per-slot private address-space bookkeeping (freed on exit/kill).
 * Kept arch-local rather than bloating the shared struct process. */
struct arm64_aspace {
    uint64_t ttbr0;    /* PA of the private L1 (0 = none / shared model) */
    void    *pa_raw;   /* raw kmalloc ptr of the private user PA region   */
    uint64_t pa_base;  /* 2 MB-aligned PA the user slot is backed by      */
    void    *l1_raw;   /* raw kmalloc ptr of the L1 page                  */
    void    *l2_raw;   /* raw kmalloc ptr of the L2 page                  */
    int      has_saved;          /* 1 = resume from saved_frame, 0 = fresh entry */
    uint64_t saved_frame[34];    /* suspended EL0 state (272 B: x0-x30,sp,elr,spsr) */
};
static struct arm64_aspace g_aspaces[ARM64_MAX_PROCS];
static int g_rr_next = 0; /* remove this */

/* Release a slot's private address space (page tables + backing RAM). */
static void aspace_free(int slot) {
    struct arm64_aspace *a = &g_aspaces[slot];
    if (a->l1_raw) kfree(a->l1_raw);
    if (a->l2_raw) kfree(a->l2_raw);
    if (a->pa_raw) kfree(a->pa_raw);
    a->ttbr0 = 0; a->pa_raw = 0; a->pa_base = 0; a->l1_raw = 0; a->l2_raw = 0;
    a->has_saved = 0;
}

/* Suspend the running process from its EL0 exception frame and return to the
 * kernel scheduler loop; the process stays READY and resumes where it left off
 * (right after the svc) on a later slice.  Called from the syscall handler for
 * cooperative yield points (SYS_YIELD, an empty SYS_EVENT_POLL, SYS_TIMER_SLEEP).
 * `frame` is the 272-byte saved EL0 frame (the handler's regs pointer). */
extern void arm64_return_to_kernel(uint64_t code) __attribute__((noreturn));
void arm64_yield_current(void *frame) {
    int g_curr = get_current_slot();
    if (g_curr < 0) { arm64_return_to_kernel(0); }
    struct arm64_aspace *a = &g_aspaces[g_curr];
    const uint64_t *src = (const uint64_t *)frame;
    for (int i = 0; i < 34; i++) a->saved_frame[i] = src[i];
    a->has_saved = 1;
    g_procs[g_curr].state = PROCESS_STATE_READY;
    set_current_slot(-1);
    arm64_aspace_switch_boot();
    arm64_return_to_kernel(0);
}

/* Like arm64_yield_current but does NOT change the process state — the caller
 * must have already set it (e.g. PROCESS_STATE_SLEEPING for timed waits). */
void arm64_sleep_current(void *frame) {
    int g_curr = get_current_slot();
    if (g_curr < 0) { arm64_return_to_kernel(0); }
    struct arm64_aspace *a = &g_aspaces[g_curr];
    const uint64_t *src = (const uint64_t *)frame;
    for (int i = 0; i < 34; i++) a->saved_frame[i] = src[i];
    a->has_saved = 1;
    set_current_slot(-1);
    arm64_aspace_switch_boot();
    arm64_return_to_kernel(0);
}


/* ---- Init ------------------------------------------------------------- */
void process_init(void) {
    memset(g_procs, 0, sizeof(g_procs));
    g_next_pid = 1;
    
    serial_write("[process] init done\r\n");
}

/* ---- Find free slot --------------------------------------------------- */
static int find_free_slot(void) {
    for (int i = 0; i < ARM64_MAX_PROCS; i++)
        if (!g_procs[i].loaded) return i;
    return -1;
}

/* ---- Create a thread sharing the parent's address space ---------------- */
int process_create_thread(struct process *parent, uintptr_t entry,
                          uintptr_t stack_top, uint64_t arg) {
    if (!parent || !entry || !stack_top) return -1;

    spin_lock(&g_process_lock);
    int slot = find_free_slot();
    if (slot < 0) {
        spin_unlock(&g_process_lock);
        return -1;
    }

    /* Share the parent's address space: borrow its TTBR0, PA backing, and
     * page tables.  The thread's g_aspaces[slot] mirrors the parent's so the
     * scheduler switches to the same mapping. */
    int parent_slot = (int)(parent - g_procs);
    g_aspaces[slot] = g_aspaces[parent_slot];
    /* But the thread needs its OWN saved_frame — clear has_saved, we'll pre-fill it. */
    g_aspaces[slot].has_saved = 0;

    /* Thread borrows the parent's image allocation (don't double-free). */
    struct process *proc = &g_procs[slot];
    memset(proc, 0, sizeof(*proc));
    proc->pid        = g_next_pid++;
    proc->parent_pid = parent->pid;
    proc->loaded     = 1;
    proc->state      = PROCESS_STATE_READY;
    proc->entry      = entry;
    proc->user_stack_top = stack_top;
    proc->user_virtual_base = parent->user_virtual_base;
    proc->code_size  = 0;
    proc->user_image_allocation = 0;  /* parent owns this */
    proc->user_image_pages       = 0;
    proc->cr3        = parent->cr3;
    proc->heap_start = parent->heap_start;
    proc->heap_break = parent->heap_break;
    proc->heap_chunk_count = 0;
    proc->is_thread  = 1;
    /* copy name from parent */
    for (int j = 0; j < 32; j++) { proc->name[j] = parent->name[j]; if (!parent->name[j]) break; }
    proc->uid = parent->uid;
    proc->gid = parent->gid;

    /* Pre-fill the saved EL0 register state so the scheduler enters the
     * thread function directly with the argument in x0. */
    uint64_t *f = g_aspaces[slot].saved_frame;
    for (int i = 0; i < 34; i++) f[i] = 0;
    f[0]  = arg;           /* x0 = argument */
    f[31] = stack_top;     /* sp_el0 */
    f[32] = entry;         /* elr_el1 = entry point */
    g_aspaces[slot].has_saved = 1;

    spin_unlock(&g_process_lock);

    serial_write("[process] thread pid=");
    { char b[16]; int i=0; uint32_t t=proc->pid; if(!t)b[i++]='0';
      else while(t){b[i++]=(char)('0'+t%10);t/=10;}
      while(i--) serial_write_char(b[i]); }
    serial_write(" parent=");
    { char b[16]; int i=0; uint32_t t=parent->pid; if(!t)b[i++]='0';
      else while(t){b[i++]=(char)('0'+t%10);t/=10;}
      while(i--) serial_write_char(b[i]); }
    serial_write("\r\n");

    return (int)proc->pid;
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

    spin_lock(&g_process_lock);
    int slot = find_free_slot();
    if (slot < 0) {
        spin_unlock(&g_process_lock);
        serial_write("[process] no free slot\r\n");
        return -1;
    }
    
    /* Mark as loading to reserve slot */
    g_procs[slot].loaded = 1;
    spin_unlock(&g_process_lock);

    /* Look up the ELF in ext2 */
    extern struct ext2_filesystem g_fs;
    extern int g_fs_ready;  /* from arch.c */
    if (!g_fs_ready) return -1;

    uint32_t ino = ext2_lookup_inode(&g_fs, path);
    if (!ino) {
        serial_write("[process] not found: ");
        serial_write(path);
        serial_write("\r\n");
        g_procs[slot].loaded = 0;
        return -1;
    }

    struct ext2_inode *node = &g_fs.inode_table[ino - 1];
    uint32_t fsize = node->size;
    if (fsize == 0 || fsize > (60u << 20)) {
        serial_write("[process] bad size\r\n");
        g_procs[slot].loaded = 0;
        return -1;
    }

    uint8_t *elf = (uint8_t *)kmalloc(fsize);
    if (!elf) { serial_write("[process] OOM\r\n"); g_procs[slot].loaded = 0; return -1; }

    ssize_t got = ext2_read(&g_fs, ino, 0, fsize, elf);
    if (got <= 0) { serial_write("[process] read error\r\n"); kfree(elf); g_procs[slot].loaded = 0; return -1; }

    if (!elf64_validate(elf, (size_t)got)) {
        serial_write("[process] not valid aarch64 ELF\r\n");
        kfree(elf); return -1;
    }

    struct elf64_header *eh = (struct elf64_header *)elf;
    struct elf64_program_header *ph =
        (struct elf64_program_header *)(elf + eh->program_header_offset);

    /* All arm64 apps are linked at fixed VA 0x90000000 (non-PIE).  Each
     * process gets its OWN private physical region behind that VA via a
     * per-process address space (private L1+L2, see arm64_aspace_create), so
     * several apps can be resident at once instead of overwriting each other
     * at a single shared PA. */
    void    *pa_raw = kmalloc(ARM64_ASPACE_SLOT_BYTES + 0x200000UL);  /* +2 MB align slack */
    if (!pa_raw) { serial_write("[process] OOM (user region)\r\n"); kfree(elf); return -1; }
    uint64_t pa_base = ((uint64_t)(uintptr_t)pa_raw + 0x1FFFFFUL) & ~0x1FFFFFULL;

    void    *l1_raw = 0, *l2_raw = 0;
    uint64_t ttbr0  = arm64_aspace_create(pa_base, &l1_raw, &l2_raw);
    if (!ttbr0) {
        serial_write("[process] aspace create failed\r\n");
        kfree(pa_raw); kfree(elf); return -1;
    }

    /* Copy each PT_LOAD segment.  The app's VA 0x90000000 is backed by
     * pa_base, so a segment at vaddr lands at PA = pa_base + (vaddr - VA_BASE).
     * The kernel reaches that PA through its identity map. */
    uint64_t image_end = 0;
    for (int i = 0; i < eh->program_header_count; i++) {
        if (ph[i].type != ELF_PROGRAM_TYPE_LOAD) continue;
        uint64_t vaddr = ph[i].virtual_address;
        uint64_t memsz = ph[i].memory_size;
        if (vaddr < ARM64_USER_BASE ||
            vaddr + memsz > ARM64_USER_BASE + ARM64_ASPACE_SLOT_BYTES) {
            serial_write("[process] segment out of slot range\r\n");
            kfree(pa_raw); kfree(l1_raw); kfree(l2_raw); kfree(elf); return -1;
        }
        uint64_t seg_end = (vaddr - ARM64_USER_BASE) + memsz;
        if (seg_end > image_end) image_end = seg_end;
        uint8_t *dst = (uint8_t *)(uintptr_t)(pa_base + (vaddr - ARM64_USER_BASE));
        size_t fsz = (size_t)ph[i].file_size;
        for (size_t b = 0; b < fsz; b++)
            dst[b] = elf[ph[i].offset + b];
        for (size_t b = fsz; b < (size_t)memsz; b++)
            dst[b] = 0;
    }

    /* I-cache flush for the newly written code */
    __asm__ volatile("dsb ish; ic ialluis; dsb ish; isb" ::: "memory");

    /* Record the private address space for cleanup on exit/kill. */
    g_aspaces[slot].ttbr0   = ttbr0;
    g_aspaces[slot].pa_raw  = pa_raw;
    g_aspaces[slot].pa_base = pa_base;
    g_aspaces[slot].l1_raw  = l1_raw;
    g_aspaces[slot].l2_raw  = l2_raw;

    /* Set up process struct */
    struct process *proc = &g_procs[slot];
    proc->pid        = g_next_pid++;
    proc->parent_pid = 0;
    proc->loaded     = 1;
    proc->state      = PROCESS_STATE_READY;
    proc->runtime_ticks = 0;   /* fresh process, no runtime yet */
    proc->uid        = uid;
    proc->gid        = gid;
    proc->entry      = eh->entry;  /* at 0x90000000 in the process's own aspace */
    proc->code_size  = image_end;
    /* Stack at the top of the 32 MB private slot (grows down). */
    proc->user_stack_top = ARM64_USER_BASE + ARM64_ASPACE_SLOT_BYTES - 16;
    proc->user_virtual_base = ARM64_USER_BASE;
    /* Heap: starts just after the loaded image (rounded up to page). */
    proc->heap_start = (proc->user_virtual_base + proc->code_size + 0xFFF) & ~0xFFF;
    proc->heap_break = proc->heap_start;
    proc->user_image_allocation = elf;  /* freed on kill */
    proc->cr3        = ttbr0;  /* per-process TTBR0 */

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
    proc->user_image_allocation = 0;

    int kslot = (int)(proc - g_procs);
    int g_curr2 = get_current_slot();
    if (g_curr2 >= 0 && &g_procs[g_curr2] == proc) {
        arm64_aspace_switch_boot();
        set_current_slot(-1);
    }
    if (!proc->is_thread) aspace_free(kslot);

    proc->loaded = 0;
    proc->state  = PROCESS_STATE_EXITED;
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
        snap->runtime_ticks = p->runtime_ticks;
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
void     arm64_resume_user(void *frame);  /* resume from a saved EL0 frame */

/* ---- Run one process slice (cooperative, round-robin) ----------------- *
 * Picks the next READY process after the round-robin cursor, switches to its
 * private address space, and either resumes it from a saved frame (it yielded
 * earlier) or enters it fresh at its entry point.  Control returns here when
 * the process yields (arm64_yield_current) or exits (process_handle_exit), so
 * several long-running apps can interleave one slice per call. */
int process_run_ready_slice(void) {
    struct cpu *c = this_cpu();
    if (!c) return 0;
    
    spin_lock(&g_process_lock);
    
    /* Wake any sleeping process whose wake_tick has arrived. */
    {
        uint64_t now = timer_tick_count();
        static int wake_dbg = 0;
        for (int i = 0; i < ARM64_MAX_PROCS; i++) {
            if (g_procs[i].state == PROCESS_STATE_SLEEPING &&
                now >= g_procs[i].wake_tick) {
                g_procs[i].state = PROCESS_STATE_READY;
                if (++wake_dbg <= 3 && g_procs[i].name[0]=='d' && g_procs[i].name[1]=='o' && g_procs[i].name[2]=='o' && g_procs[i].name[3]=='m') {
                    serial_write("[wake] doom pid=");
                    serial_write_hex_u64(g_procs[i].pid);
                    serial_write(" now="); serial_write_hex_u64(now);
                    serial_write(" wake="); serial_write_hex_u64(g_procs[i].wake_tick);
                    serial_write("\r\n");
                }
            }
        }
    }

    /* Round-robin: scan starting just after the last process we ran on this CPU. */
    int slot = -1;
    for (int k = 0; k < ARM64_MAX_PROCS; k++) {
        int i = (c->sched_cursor + k) % ARM64_MAX_PROCS;
        if (g_procs[i].loaded && g_procs[i].state == PROCESS_STATE_READY) {
            slot = i; break;
        }
    }
    if (slot < 0) {
        spin_unlock(&g_process_lock);
        return 0;  /* nothing ready */
    }
    c->sched_cursor = (slot + 1) % ARM64_MAX_PROCS;

    struct process *proc = &g_procs[slot];
    proc->state = PROCESS_STATE_RUNNING;
    set_current_slot(slot);

    /* Activate the process's private address space (its own memory behind
     * VA 0x90000000) before entering EL0. */
    if (g_aspaces[slot].ttbr0)
        arm64_aspace_switch(g_aspaces[slot].ttbr0);

    /* Resume a suspended process from its saved frame, or enter fresh. */
    int has_saved = g_aspaces[slot].has_saved;
    uint64_t saved_frame[34];
    if (has_saved) {
        for (int i=0; i<34; i++) saved_frame[i] = g_aspaces[slot].saved_frame[i];
        g_aspaces[slot].has_saved = 0;
    }
    uint64_t entry = proc->entry;
    uint64_t user_stack_top = proc->user_stack_top;
    
    spin_unlock(&g_process_lock);

    uint64_t t0;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(t0));

    if (has_saved) {
        arm64_resume_user(saved_frame);
    } else {
        arm64_enter_user(entry, user_stack_top, 0, 0);
    }

    uint64_t t1;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(t1));
    uint64_t dt = t1 - t0;
    proc->runtime_ticks += dt;
    return 1;  /* ran a process slice */
}

/* ---- Called from the exception handler when a process exits ----------- */
void process_handle_exit(uint64_t code) {
    int g_curr3 = get_current_slot();
    if (g_curr3 >= 0) {
        struct process *proc = &g_procs[g_curr3];
        proc->exit_code = (int32_t)code;
        proc->state = PROCESS_STATE_EXITED;
        if (proc->user_image_allocation)
            kfree(proc->user_image_allocation);
        proc->user_image_allocation = 0;
        /* The active TTBR0 still points at this process's page tables; switch
         * back to the shared boot aspace BEFORE freeing them. */
        arm64_aspace_switch_boot();
        if (!proc->is_thread) aspace_free(g_curr3);
        proc->loaded = 0;
        serial_write("[process] exit pid=");
        { char b[16]; int i=0; uint32_t t=proc->pid; if(!t)b[i++]='0';
          else while(t){b[i++]=(char)('0'+t%10);t/=10;}
          while(i--) serial_write_char(b[i]); }
        serial_write(" code=");
        serial_write_hex_u64(code);
        serial_write("\r\n");
        set_current_slot(-1);
    }
    /* Return to kernel main loop */
    extern void arm64_return_to_kernel(uint64_t code) __attribute__((noreturn));
    arm64_return_to_kernel(code);
}

/* ---- Stubs for functions not yet implemented on arm64 ----------------- */
int process_has_current(void) { return get_current_slot() >= 0; }
uint32_t process_current_pid(void) {
    int c = get_current_slot(); if (c >= 0) return g_procs[c].pid;
    return 0;
}
uint32_t process_current_uid(void) {
    int c = get_current_slot(); if (c >= 0) return g_procs[c].uid;
    return 0;
}
uint32_t process_current_gid(void) {
    int c = get_current_slot(); if (c >= 0) return g_procs[c].gid;
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

