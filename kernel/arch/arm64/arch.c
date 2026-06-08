/* VibeOS arm64 — kernel entry + interactive serial shell.
 *
 * On HVF (Apple Silicon), QEMU's GICv2 CPU-interface MMIO at 0x08010000
 * triggers "Assertion failed: (isv)" because the HVF trap handler cannot
 * always determine the access size from the exception syndrome (ISV=0 for
 * some load/store instructions to device memory).
 *
 * Work-around: skip GIC MMIO entirely.  The ARM generic timer exposes its
 * fired status directly via CNTP_CTL_EL0.ISTATUS (bit 2), readable without
 * any MMIO.  We poll this in a tight loop for the uptime counter and use
 * plain wfi for idle.  UART input is read by polling UART_FR (PL011 flag
 * register), which is device MMIO — but PL011 word-size reads do set ISV
 * correctly in QEMU's HVF backend (only GIC reads fail).
 *
 * This gives us: working shell, uptime, and timer ticks on HVF, TCG, and
 * (once the ISV issue is fixed upstream) full GIC.
 */
#include "arch.h"
#include "../../include/serial.h"
#include "../../include/alloc.h"
#include "../../include/ramdisk.h"
#include "../../include/ext2_fs.h"
#include "../../include/elf.h"
#include "../../include/cpu.h"
#include "../../include/smp.h"
#include "../../include/bkl.h"
#include "../../include/syscall.h"   /* shared SYS_* numbers (x86 + arm64) */
#include "../../include/framebuffer.h"
#include "../../include/process.h"
#include "../../include/timer.h"
#include "../../include/string.h"
#include "../../include/render.h"
#include "../../include/window.h"
#include "../../include/input.h"
#include "../../include/keymap.h"
#include "../../include/clipboard.h"
#include "../../include/winsys.h"
#include "../../include/audio.h"
#include "../../include/vfs.h"

/* ---- Timer tick counter (incremented without GIC) --------------------- */
static volatile uint64_t g_tick = 0;


/* ---- IRQ handler (exceptions.S → TCG/GIC path only) ----------------- */
void arm64_irq_handler(void) {
    /* Only reached on the TCG path where GIC IRQs are enabled. */
    unsigned irq = arm64_gic_ack();
    switch (irq) {
    case TIMER_IRQ_PHYS:
        arm64_timer_ack();
        g_tick++;
        break;
    case 1023: break;  /* spurious */
    default:
        serial_write("[irq] "); serial_write_hex_u64(irq); serial_write("\r\n");
        break;
    }
    arm64_gic_eoi(irq);
}

/* ---- Fault handlers --------------------------------------------------- */
void arm64_sync_handler_el1(uint64_t esr, uint64_t elr, uint64_t far,
                             void *sp) {
    (void)sp;
    serial_write("\r\n!!! arm64 KERNEL FAULT !!!\r\n");
    serial_write("  ESR = "); serial_write_hex_u64(esr); serial_write("\r\n");
    serial_write("  ELR = "); serial_write_hex_u64(elr); serial_write("\r\n");
    serial_write("  FAR = "); serial_write_hex_u64(far); serial_write("\r\n");
    serial_write("  EC  = "); serial_write_hex_u64((esr >> 26) & 0x3f);
    serial_write("\r\n");
    while (1) __asm__ volatile("wfi");
}

/* ---- Filesystem + block device (defined here, used by syscalls + shell) -
 * 64-byte aligned with guard padding: struct-copy STP pairs that overrun
 * would otherwise corrupt the device function pointers in the next struct. */
static struct ramdisk_device  g_blk_dev __attribute__((aligned(64)));
static uint8_t                _pad0[64];
struct ext2_filesystem g_fs      __attribute__((aligned(64)));
static uint8_t                _pad1[64];
int   g_fs_ready = 0;
static char  g_cwd[256] = "/";

/* ---- Minimal open-file table for EL0 syscalls ------------------------- *
 * Single-process model (no scheduler yet on arm64), so one global table is
 * enough. fd 0/1/2 are reserved for stdin/stdout/stderr (UART). */
#define ARM64_MAX_FD 16
#define ARM64_FD_TYPE_FILE 0
#define ARM64_FD_TYPE_PTY  1
#define PTY_BUF_SIZE 4096

struct arm64_pty {
    char   in_buf[PTY_BUF_SIZE];    /* master writes → slave reads (terminal→shell) */
    int    in_head, in_tail, in_count;
    char   out_buf[PTY_BUF_SIZE];   /* slave writes → master reads (shell→terminal) */
    int    out_head, out_tail, out_count;
    int    master_fd;
    uint32_t child_pid;              /* shell pid attached to slave */
};

struct arm64_file {
    int      used;
    int      type;       /* ARM64_FD_TYPE_* */
    int      writable;
    uint32_t ino;
    uint32_t offset;
    uint32_t size;
    struct arm64_pty *pty;  /* valid when type == ARM64_FD_TYPE_PTY */
};

static struct arm64_file g_files[ARM64_MAX_FD];
static struct arm64_pty  g_pty;     /* single global PTY for now */

/* Deferred resolution change — like x86's g_resolution_change_requested. */
static uint32_t g_res_w = 0, g_res_h = 0;
static int      g_res_change_pending = 0;

/* Argument string for the currently-exec'd program (SYS_GETARG returns it).
 * crt0.c reads this and splits it into argv — same mechanism as x86. */
static char g_spawn_arg[512];

/* EL0 exception frame, as laid out by SAVE_REGS in exceptions.S:
 * regs[0..30] = x0..x30, regs[31]=sp_el0, regs[32]=elr_el1, regs[33]=spsr. */
extern void arm64_return_to_kernel(uint64_t code) __attribute__((noreturn));
extern void arm64_yield_current(void *frame) __attribute__((noreturn));
extern void arm64_sleep_current(void *frame) __attribute__((noreturn));
extern struct process g_procs[];
extern int process_create_thread(struct process *parent, uintptr_t entry, uintptr_t stack_top, uint64_t arg);
void process_handle_exit(uint64_t code);
struct desktop_state *desktop_active(void);

/* Copy an EL0 user string (NUL-terminated) into a kernel buffer. EL0 pointers
 * are in the alias window (0x80000000+); they are readable from EL1 (AP=01). */
static void copy_user_str(char *dst, const char *user, size_t cap) {
    size_t i = 0;
    for (; i + 1 < cap && user[i]; i++) dst[i] = user[i];
    dst[i] = '\0';
}

void arm64_sync_handler_el0(uint64_t esr, uint64_t elr, uint64_t far,
                             void *sp) {
    uint64_t *regs = (uint64_t *)sp;
    unsigned ec = (unsigned)((esr >> 26) & 0x3f);

    if (ec == 0x15) {           /* SVC from AArch64 EL0 — a syscall */
        uint64_t num = regs[8]; /* x8 = syscall number */
        uint64_t a0  = regs[0];
        uint64_t a1  = regs[1];
        uint64_t a2  = regs[2];
        uint64_t a3  = regs[3];
        uint64_t a4  = regs[4];
        uint64_t a5  = regs[5];

        /* Shared SYS_* numbers (kernel/include/syscall.h), same ABI as x86 so
         * the common libc + apps work unmodified across arches.
         *
         * TODO: Unify with kernel/src/process.c syscall dispatch.
         * Many of these handlers (SYS_CHDIR, SYS_GETCWD, SYS_READDIR, SYS_STAT,
         * SYS_OPEN, SYS_CLOSE, etc.) duplicate the shared process.c logic.
         * The shared code uses x86-specific interrupt_frame/TSS/FPU types;
         * once abstracted, route through process_syscall_dispatch() directly.
         * Tracked in SYSCALL_UNIFICATION.md. */
        switch (num) {
        case SYS_WRITE: {       /* 1: write(fd, buf, len) */
            const char *buf = (const char *)(uintptr_t)a1;
            if (a0 <= 2) {      /* stdout/stderr */
                /* Check if this process is a PTY child (shell) → redirect to PTY */
                struct arm64_pty *pty = &g_pty;
                struct process *cur_proc = (this_cpu() && this_cpu()->current) ? this_cpu()->current : 0;
                static int pty_dbg = 0;
                if (pty_dbg < 5 && cur_proc && pty->child_pid) {
                    serial_write("[pty] write pid="); serial_write_hex_u64(cur_proc->pid);
                    serial_write(" child="); serial_write_hex_u64(pty->child_pid);
                    serial_write(" match="); serial_write_hex_u64(cur_proc->pid == pty->child_pid ? 1 : 0);
                    serial_write("\r\n");
                    pty_dbg++;
                }
                if (cur_proc && pty->child_pid && cur_proc->pid == pty->child_pid) {
                    /* Write to PTY output buffer (shell → terminal) */
                    uint64_t n = a2;
                    if (n > (uint64_t)(PTY_BUF_SIZE - pty->out_count))
                        n = (uint64_t)(PTY_BUF_SIZE - pty->out_count);
                    for (uint64_t i = 0; i < n; i++) {
                        pty->out_buf[pty->out_head] = buf[i];
                        pty->out_head = (pty->out_head + 1) % PTY_BUF_SIZE;
                    }
                    pty->out_count += (int)n;
                    regs[0] = n;
                    return;
                }
                /* Normal stdout/stderr → UART */
                for (uint64_t i = 0; i < a2; i++)
                    serial_write_char(buf[i]);
                regs[0] = a2;
            } else if (a0 < ARM64_MAX_FD && g_files[a0].used) {
                struct arm64_file *f = &g_files[a0];
                if (f->type == ARM64_FD_TYPE_PTY) {
                    /* PTY master write → input buffer (terminal → shell) */
                    struct arm64_pty *pty = f->pty;
                    if (!pty) { regs[0] = (uint64_t)-1; return; }

                    /* Auto-detect Ctrl+C (0x03): interrupt foreground job
                     * without waiting for the shell to read it. */
                    for (uint64_t i = 0; i < a2; i++) {
                        if (buf[i] == 0x03 && pty->child_pid) {
                            process_kill(pty->child_pid);
                            regs[0] = a2;  /* report all bytes written */
                            return;        /* drop the 0x03 — don't queue it */
                        }
                    }

                    uint64_t n = a2;
                    if (n > (uint64_t)(PTY_BUF_SIZE - pty->in_count))
                        n = (uint64_t)(PTY_BUF_SIZE - pty->in_count);
                    for (uint64_t i = 0; i < n; i++) {
                        pty->in_buf[pty->in_head] = buf[i];
                        pty->in_head = (pty->in_head + 1) % PTY_BUF_SIZE;
                    }
                    pty->in_count += (int)n;
                    regs[0] = n;
                } else if (f->writable) {
                    ssize_t w = ext2_write(&g_fs, f->ino, f->offset, a2, buf);
                    if (w > 0) { f->offset += (uint32_t)w; regs[0] = (uint64_t)w; }
                    else regs[0] = (uint64_t)-1;
                } else regs[0] = (uint64_t)-1;
            } else regs[0] = (uint64_t)-1;
            return;
        }
        case SYS_IOCTL:         /* 2: no-op stub */
            regs[0] = 0;
            return;
        case SYS_CHDIR: {       /* 11: change directory */
            char path[256];
            copy_user_str(path, (const char *)(uintptr_t)a0, sizeof(path));
            /* Simple chdir: just verify the path exists and is a directory */
            uint32_t ino = ext2_lookup_inode(&g_fs, path);
            if (!ino) { regs[0] = (uint64_t)-1; return; }
            struct ext2_inode *node = &g_fs.inode_table[ino - 1];
            if (!(node->mode & 0x4000)) { regs[0] = (uint64_t)-1; return; }
            /* Update global cwd */
            size_t i;
            for (i = 0; path[i] && i < sizeof(g_cwd) - 1; i++)
                g_cwd[i] = path[i];
            g_cwd[i] = '\0';
            regs[0] = 0;
            return;
        }
        case SYS_GETCWD: {      /* 12: get current working directory */
            char *buf = (char *)(uintptr_t)a0;
            uint64_t len = a1;
            if (!buf || !len) { regs[0] = (uint64_t)-1; return; }
            size_t i;
            for (i = 0; g_cwd[i] && i < len - 1; i++)
                buf[i] = g_cwd[i];
            buf[i] = '\0';
            regs[0] = 0;
            return;
        }
        case SYS_READ: {        /* 0: read(fd, buf, len) */
            /* fd 0 = stdin → UART (or PTY for shell) */
            if (a0 == 0) {
                char *dst = (char *)(uintptr_t)a1;
                if (a2 == 0 || !dst) { regs[0] = 0; return; }
                /* Check if this process is a PTY child (shell) — redirect to PTY */
                struct arm64_pty *pty = &g_pty;
                struct process *cur_proc = (this_cpu() && this_cpu()->current) ? this_cpu()->current : 0;
                if (cur_proc && pty->child_pid && cur_proc->pid == pty->child_pid) {
                    /* Read from PTY input buffer (terminal → shell).
                     * Return as many bytes as available up to a2. */
                    uint64_t n = a2;
                    if (n > (uint64_t)pty->in_count)
                        n = (uint64_t)pty->in_count;
                    if (n == 0) {
                        regs[32] -= 4;  /* re-execute SVC on resume */
                        arm64_yield_current(regs);
                    }
                    for (uint64_t i = 0; i < n; i++) {
                        dst[i] = pty->in_buf[pty->in_tail];
                        pty->in_tail = (pty->in_tail + 1) % PTY_BUF_SIZE;
                    }
                    pty->in_count -= (int)n;
                    regs[0] = n;
                    return;
                }
                if (!serial_can_read()) {
                    regs[32] -= 4;
                    arm64_yield_current(regs);
                }
                char c = serial_read_byte();
                dst[0] = c;
                regs[0] = 1;
                return;
            }
            if (a0 < 3 || a0 >= ARM64_MAX_FD || !g_files[a0].used) {
                regs[0] = (uint64_t)-1; return;
            }
            struct arm64_file *f = &g_files[a0];
            /* PTY master: read from shell output buffer */
            if (f->type == ARM64_FD_TYPE_PTY) {
                char *dst = (char *)(uintptr_t)a1;
                if (a2 == 0 || !dst) { regs[0] = 0; return; }
                struct arm64_pty *pty = f->pty;
                if (!pty) { regs[0] = (uint64_t)-1; return; }
                if (pty->out_count == 0) {
                    regs[0] = 0;   /* no data — return 0, caller will poll again */
                    return;
                }
                char c = pty->out_buf[pty->out_tail];
                pty->out_tail = (pty->out_tail + 1) % PTY_BUF_SIZE;
                pty->out_count--;
                dst[0] = c;
                regs[0] = 1;
                return;
            }
            uint32_t remain = (f->offset < f->size) ? (f->size - f->offset) : 0;
            uint32_t want   = (a2 < remain) ? (uint32_t)a2 : remain;
            if (want == 0) { regs[0] = 0; return; }
            ssize_t got = ext2_read(&g_fs, f->ino, f->offset, want,
                                    (void *)(uintptr_t)a1);
            if (got > 0) { f->offset += (uint32_t)got; regs[0] = (uint64_t)got; }
            else regs[0] = (uint64_t)-1;
            return;
        }
        case SYS_OPEN: {        /* 7: open(path) → fd (read-only) */
            if (!g_fs_ready) { regs[0] = (uint64_t)-1; return; }
            char path[256];
            copy_user_str(path, (const char *)(uintptr_t)a0, sizeof(path));
            uint32_t ino = ext2_lookup_inode(&g_fs, path);
            if (!ino) { regs[0] = (uint64_t)-1; return; }
            int fd = -1;
            for (int i = 3; i < ARM64_MAX_FD; i++)
                if (!g_files[i].used) { fd = i; break; }
            if (fd < 0) { regs[0] = (uint64_t)-1; return; }
            g_files[fd].used   = 1; g_files[fd].writable = 0;
            g_files[fd].ino    = ino; g_files[fd].offset = 0;
            g_files[fd].size   = g_fs.inode_table[ino - 1].size;
            regs[0] = (uint64_t)fd;
            return;
        }
        case SYS_CREAT: {       /* 14: creat(path) → writable fd */
            if (!g_fs_ready) { regs[0] = (uint64_t)-1; return; }
            char path[256];
            copy_user_str(path, (const char *)(uintptr_t)a0, sizeof(path));
            uint32_t ino = ext2_lookup_inode(&g_fs, path);
            if (!ino) ino = ext2_create(&g_fs, path, 0100644);
            if (!ino) { regs[0] = (uint64_t)-1; return; }
            int fd = -1;
            for (int i = 3; i < ARM64_MAX_FD; i++)
                if (!g_files[i].used) { fd = i; break; }
            if (fd < 0) { regs[0] = (uint64_t)-1; return; }
            g_files[fd].used = 1; g_files[fd].writable = 1;
            g_files[fd].ino  = ino; g_files[fd].offset = 0;
            g_files[fd].size = g_fs.inode_table[ino - 1].size;
            regs[0] = (uint64_t)fd;
            return;
        }
        case SYS_CLOSE:         /* 8 */
            if (a0 >= 3 && a0 < ARM64_MAX_FD && g_files[a0].used)
                g_files[a0].used = 0;
            regs[0] = 0;
            return;
        case SYS_MKDIR: {       /* 26: mkdir(path) → 0 or -1 */
            char path[256];
            copy_user_str(path, (const char *)(uintptr_t)a0, sizeof(path));
            int r = vfs_mkdir(path);
            regs[0] = (uint64_t)(int64_t)r;
            return;
        }
        case SYS_SEEK: {        /* 48: lseek(fd, offset, whence) → new_offset */
            if (a0 < 3 || a0 >= ARM64_MAX_FD || !g_files[a0].used) {
                regs[0] = (uint64_t)-1; return;
            }
            struct arm64_file *f = &g_files[a0];
            int64_t off = (int64_t)a1;
            switch (a2) {
                case 0: f->offset = (uint32_t)off; break;           /* SEEK_SET */
                case 1: f->offset = (uint32_t)((int64_t)f->offset + off); break; /* SEEK_CUR */
                case 2: f->offset = (uint32_t)((int64_t)f->size + off); break;  /* SEEK_END */
                default: regs[0] = (uint64_t)-1; return;
            }
            regs[0] = (uint64_t)f->offset;
            return;
        }
        case SYS_FB_INFO: {     /* 65: fill struct vos_fb_info* with the framebuffer */
            /* struct layout: u64 addr; u32 width,height,stride,bpp; */
            uint32_t *fb = ramfb_buffer();
            if (!fb) {          /* lazily bring up an 800x600 framebuffer */
                if (ramfb_init(1280, 720) != 0) { regs[0] = (uint64_t)-1; return; }
                fb = ramfb_buffer();
            }
            /* The fb lives in kernel RAM (PA 0x40000000+); EL0 reaches it via the
             * alias at +0x40000000. Hand userspace that EL0-visible address. */
            uint64_t *out = (uint64_t *)(uintptr_t)a0;   /* EL0 ptr, EL1-readable */
            uint32_t *o32 = (uint32_t *)(out + 1);
            out[0] = (uint64_t)(uintptr_t)fb + 0x40000000ULL;  /* EL0 alias VA */
            o32[0] = ramfb_width();
            o32[1] = ramfb_height();
            o32[2] = ramfb_stride_px() * 4;
            o32[3] = 32;
            regs[0] = 0;
            return;
        }
        case SYS_SLEEP_MS: {    /* 68: sleep for N milliseconds */
            uint64_t ms = a0;
            if (ms == 0) { regs[0] = 0; return; }
            struct process *cur = (this_cpu() && this_cpu()->current)
                                  ? this_cpu()->current : (struct process *)0;
            if (!cur) { regs[0] = (uint64_t)-1; return; }
            uint64_t hz = timer_frequency_hz();
            if (hz == 0) hz = 24000000;
            uint64_t ticks = ms * (hz / 1000);
            uint64_t now = timer_tick_count();
            cur->wake_tick = (ticks >= ~0ull - now) ? ~0ull : now + ticks;
            cur->state = PROCESS_STATE_SLEEPING;
            regs[0] = 0;
            arm64_sleep_current(regs);
            /* not reached */
        }
        case SYS_KEYMAP_SET: {  /* 67: switch keyboard layout at runtime */
            const char *name = (const char *)(uintptr_t)a0;
            int result = keymap_set(name);
            regs[0] = (uint64_t)(int64_t)result;
            return;
        }
        case SYS_INPUT_POLL: {  /* 66: fill struct vos_input_state* with mouse state */
            /* struct vos_input_state { int x, y, buttons, moved; }; */
            int *out = (int *)(uintptr_t)a0;   /* EL0 ptr, EL1-readable */
            out[0] = g_mouse_x;
            out[1] = g_mouse_y;
            out[2] = g_mouse_buttons;
            out[3] = g_mouse_moved;
            g_mouse_moved = 0;  /* clear after read */
            regs[0] = 0;
            return;
        }
        case SYS_GETARG: {      /* 15: getarg(buf, cap) → len; crt0 splits argv */
            char *ubuf = (char *)(uintptr_t)a0;
            uint64_t cap = a1;
            uint64_t i = 0;
            for (; i + 1 < cap && g_spawn_arg[i]; i++) ubuf[i] = g_spawn_arg[i];
            if (cap) ubuf[i] = '\0';
            regs[0] = i;
            return;
        }
        case SYS_EXIT:          /* 4: cleanup + longjmp back into the kernel */
            process_handle_exit(a0);
            /* not reached */
        case SYS_WINDOWMGR_START: { /* 21: mark window manager active */
            g_desktop_uid = (this_cpu() && this_cpu()->current) ? this_cpu()->current->uid : 0;
            regs[0] = 0;
            return;
        }
        case SYS_WINDOW_CREATE: {  /* 17: create a window */
            struct desktop_state *d = desktop_active();
            if (!d) { regs[0] = (uint64_t)-1; return; }
            const char *title = (const char *)(uintptr_t)a0;
            uint32_t pid = (this_cpu() && this_cpu()->current) ? this_cpu()->current->pid : 0;
            int result = desktop_app_create(d, pid, title, (int)a1, (int)a2);
            regs[0] = (uint64_t)(int64_t)result;
            return;
        }
        case SYS_WINDOW_CREATE_EX: { /* 35: create window with options */
            struct desktop_state *d = desktop_active();
            if (!d) { serial_write("[winex] no desktop\r\n"); regs[0] = (uint64_t)-1; return; }
            const struct winsys_window_options *opts =
                (const struct winsys_window_options *)(uintptr_t)a0;
            if (!opts) { serial_write("[winex] null opts\r\n"); regs[0] = (uint64_t)-1; return; }
            uint32_t pid = (this_cpu() && this_cpu()->current) ? this_cpu()->current->pid : 0;
            int result = desktop_app_create_ex(d, pid, opts);
            regs[0] = (uint64_t)(int64_t)result;
            return;
        }
        case SYS_WINDOW_PRESENT:    /* 18: present full window */
        case SYS_WINDOW_PRESENT_RECT: { /* 38: present window rect */
            struct desktop_state *d = desktop_active();
            if (!d) { regs[0] = (uint64_t)-1; return; }
            int win_id = (int)a0;
            const uint32_t *pixels = (const uint32_t *)(uintptr_t)a1;
            int w = (int)a2, h = (int)a3;
            int dx = 0, dy = 0, dw = 0, dh = 0;
            if (num == SYS_WINDOW_PRESENT_RECT) {
                dx = (int)((a4 >> 16) & 0xffffu);
                dy = (int)(a4 & 0xffffu);
                dw = (int)((a5 >> 16) & 0xffffu);
                dh = (int)(a5 & 0xffffu);
            }
            uint32_t pid = (this_cpu() && this_cpu()->current) ? this_cpu()->current->pid : 0;
            static int pres_dbg = 0;
            if (pres_dbg < 20) {
                serial_write("[present] win="); serial_write_hex_u64((uint64_t)win_id);
                serial_write(" pid="); serial_write_hex_u64(pid);
                serial_write(" w="); serial_write_hex_u64((uint64_t)w);
                serial_write(" h="); serial_write_hex_u64((uint64_t)h);
                serial_write("\r\n");
                pres_dbg++;
            }
            int result = desktop_app_present_rect(d, pid, win_id, pixels, w, h,
                                                   dx, dy, dw, dh);
            regs[0] = (uint64_t)(int64_t)result;
            return;
        }
        case SYS_DISPLAY_MODE: {    /* 27: get/set display resolution */
            uint32_t rw = (uint32_t)a0;
            if (rw == 0) {
                uint32_t w = ramfb_width(), h = ramfb_height();
                if (w == 0 || h == 0) { w = 1280; h = 720; }
                regs[0] = (uint64_t)(((uint64_t)(w & 0xffffu) << 16) | (h & 0xffffu));
            } else {
                uint32_t rh = (uint32_t)a1;
                if (rw < 320) rw = 320;
                if (rh < 200) rh = 200;
                if (rw > 2560) rw = 2560;
                if (rh > 1600) rh = 1600;
                g_res_w = rw;
                g_res_h = rh;
                g_res_change_pending = 1;
                regs[0] = 0;
            }
            return;
        }
        case SYS_SYSTEM_INFO: {     /* 39: fill system info snapshot */
            struct system_info_snapshot *out =
                (struct system_info_snapshot *)(uintptr_t)a0;
            if (!out) { regs[0] = (uint64_t)-1; return; }
            out->uptime_ticks = timer_tick_count();
            out->timer_hz = timer_frequency_hz();
            out->process_count = process_loaded_count();
            out->process_max = 16;
            out->app_window_max = 8;
            out->cpu_count = (uint32_t)smp_cpu_count();
            out->heap_used_bytes = kmalloc_get_physical_used();
            out->heap_total_bytes = kmalloc_get_physical_total();
            { const char *v = "0.1.0-arm64";
              for (int vi = 0; vi < 63 && v[vi]; vi++) out->version[vi] = v[vi];
              out->version[63] = '\0'; }
            { const char *b = __DATE__ " " __TIME__;
              for (int bi = 0; bi < 63 && b[bi]; bi++) out->build[bi] = b[bi];
              out->build[63] = '\0'; }
            regs[0] = 0;
            return;
        }
        case SYS_TEXT_DRAW: {       /* 40: rasterize text into an ARGB buffer */
            /* a0=buf, a1=text, a2=(buf_w<<16)|buf_h,
             * a3=((x&0xffff)<<16)|(y&0xffff), a4=color, a5=scale|flags.
             * If a5 has bit 31 set, monospace mode is used (each glyph
             * centred in a CELLW-wide cell — for terminal grids). */
            uint32_t *buf = (uint32_t *)(uintptr_t)a0;
            int buf_w = (int)((a2 >> 16) & 0xffffu);
            int buf_h = (int)(a2 & 0xffffu);
            int x = (int)(int16_t)((a3 >> 16) & 0xffffu);
            int y = (int)(int16_t)(a3 & 0xffffu);
            int scale = (int)(a5 & 0x7fffffffu);
            int mono  = (int)((a5 >> 31) & 1u);
            if (buf == 0 || scale < 1) { regs[0] = (uint64_t)-1; return; }
            char text[256];
            copy_user_str(text, (const char *)(uintptr_t)a1, sizeof(text));
            if (mono)
                regs[0] = (uint64_t)(int64_t)draw_text_mono_to_argb(buf, buf_w, buf_h,
                    x, y, text, (uint32_t)a4, scale);
            else
                regs[0] = (uint64_t)(int64_t)draw_text_to_argb(buf, buf_w, buf_h,
                    x, y, text, (uint32_t)a4, scale);
            return;
        }
        case SYS_TEXT_METRICS: {    /* 41: query atlas metrics / string width */
            if (a0 == 0) {
                regs[0] = (uint64_t)font_metrics_packed((int)a1);
            } else {
                char text[256];
                copy_user_str(text, (const char *)(uintptr_t)a0, sizeof(text));
                regs[0] = (uint64_t)(int64_t)text_width(text, (int)a1);
            }
            return;
        }
        case SYS_SET_WALLPAPER: {   /* 36: set wallpaper from pixel buffer */
            struct desktop_state *d = desktop_active();
            if (!d) { regs[0] = (uint64_t)-1; return; }
            const uint32_t *pixels = (const uint32_t *)(uintptr_t)a0;
            int w = (int)a1, h = (int)a2;
            int result = desktop_set_wallpaper(d, pixels, w, h);
            d->background_dirty = 1;
            regs[0] = (uint64_t)(int64_t)result;
            return;
        }
        case SYS_TIMER_SLEEP: { /* 20: sleep for N ticks (CNTVCT units on arm64) */
            uint64_t ticks = a0;
            if (ticks == 0) { regs[0] = 0; return; }
            struct process *cur = (this_cpu() && this_cpu()->current)
                                  ? this_cpu()->current : (struct process *)0;
            if (!cur) { regs[0] = (uint64_t)-1; return; }
            /* CNTVCT ticks directly — apps query timer_frequency_hz() and
             * compute sleep durations themselves.  No scaling needed. */
            uint64_t now = timer_tick_count();
            cur->wake_tick = (ticks >= ~0ull - now) ? ~0ull : now + ticks;
            cur->state = PROCESS_STATE_SLEEPING;
            static int sleep_dbg = 0;
            if (++sleep_dbg <= 3 && cur->name[0]=='d' && cur->name[1]=='o' && cur->name[2]=='o' && cur->name[3]=='m') {
                serial_write("[sleep] doom pid="); serial_write_hex_u64(cur->pid);
                serial_write(" ticks="); serial_write_hex_u64(ticks);
                serial_write(" now="); serial_write_hex_u64(now);
                serial_write(" wake="); serial_write_hex_u64(cur->wake_tick);
                serial_write("\r\n");
            }
            regs[0] = 0;
            arm64_sleep_current(regs);
            /* not reached */
        }
        case SYS_YIELD:         /* 3: suspend and reschedule (resumes after svc) */
            regs[0] = 0;
            arm64_yield_current(regs);
            /* not reached */
        case SYS_STAT: {         /* 9: stat path — returns kind, size, mode */
            const char *path = (const char *)(uintptr_t)a0;
            struct vfs_stat st;
            if (!vfs_stat_path(path, &st)) { regs[0] = (uint64_t)-1; return; }
            regs[0] = (uint64_t)(st.kind == VFS_NODE_DIRECTORY ? 2 : 1);
            regs[1] = st.size;
            regs[2] = st.mode;
            return;
        }
        case SYS_READDIR: {      /* 10: readdir(path, idx, name, cap) → kind, size */
            const char *path = (const char *)(uintptr_t)a0;
            uint32_t idx = (uint32_t)a1;
            char *name = (char *)(uintptr_t)a2;
            uint64_t cap = a3;
            struct vfs_dir_entry e;
            if (!vfs_readdir(path, idx, &e)) {
                regs[0] = 0;   /* end of directory, not an error */
                return;
            }
            /* Copy name to user buffer */
            size_t name_len = 0;
            while (e.name[name_len] && name_len + 1 < cap) name_len++;
            for (size_t i = 0; i < name_len; i++) name[i] = e.name[i];
            if (cap) name[name_len] = '\0';
            {
                static int rd_ok = 0;
                if (++rd_ok <= 3) {
                    serial_write("[readdir] OK path="); serial_write(path);
                    serial_write(" idx="); serial_write_hex_u64(idx);
                    serial_write(" name="); serial_write(name);
                    serial_write("\r\n");
                }
            }
            regs[0] = 1;                    /* 1 = entry found (matches x86) */
            regs[1] = (uint64_t)e.kind;     /* kind: 1=file, 2=dir */
            regs[2] = e.size;               /* size in bytes */
            return;
        }
        case SYS_AUDIO_WRITE:    /* 49: a0=buf, a1=byte count */
            /* Feed PCM to the virtio-sound device.  Returns bytes accepted (0
             * when its tx ring is full → caller yields and retries, which paces
             * playback to real time).  If no audio device is present, accept and
             * discard so blocking writers (DOOM's I_UpdateSound) don't hang. */
            if (virtio_snd_ready()) {
                uint32_t pid = (this_cpu() && this_cpu()->current)
                               ? this_cpu()->current->pid : 0;
                regs[0] = (uint64_t)(int64_t)virtio_snd_write(
                    pid, (const void *)(uintptr_t)a0, a1);
            } else {
                regs[0] = a1;   /* /dev/null sink */
            }
            return;
        case SYS_AUDIO_INFO: {   /* 50: fill struct audio_info */
            struct audio_info *out = (struct audio_info *)(uintptr_t)a0;
            if (!out) { regs[0] = (uint64_t)-1; return; }
            memset(out, 0, sizeof(*out));
            out->present     = virtio_snd_ready() ? 1u : 0u;
            out->channels    = 2;
            out->bits        = 16;
            out->sample_rate = 48000;
            regs[0] = 0;
            return;
        }
        case SYS_AUDIO_IOCTL:    /* 60: no rate/volume control on virtio-snd yet */
            regs[0] = 0;
            return;
        case SYS_CPU_INFO: {     /* 64: a0=struct cpu_info_snapshot* buf, a1=max */
            struct cpu_info_snapshot *buf = (struct cpu_info_snapshot *)(uintptr_t)a0;
            unsigned max = (unsigned)a1;
            if (!buf || max == 0) { regs[0] = (uint64_t)-1; return; }
            unsigned n = smp_cpu_count();
            if (n > max) n = max;
            for (unsigned i = 0; i < n; i++) {
                struct cpu *c = cpu_get(i);
                if (c) {
                    buf[i].index = c->index;
                    buf[i].apic_id = c->apic_id;
                    buf[i].ticks = c->ticks;
                    buf[i].busy = c->busy_ticks;
                }
            }
            regs[0] = (uint64_t)n;
            return;
        }
        case SYS_PROCESS_SNAPSHOT: { /* 28: a0=index, a1=struct process_snapshot* */
            struct process_snapshot *out = (struct process_snapshot *)(uintptr_t)a1;
            if (!out) { regs[0] = (uint64_t)-1; return; }
            regs[0] = (uint64_t)(int64_t)process_get_snapshot((uint32_t)a0, out);
            return;
        }
        case SYS_PROCESS_SPAWN: { /* 5: spawn a new process. a0=name, a1=arg (optional) */
            const char *user_name = (const char *)(uintptr_t)a0;
            if (!user_name) { regs[0] = (uint64_t)-1; return; }
            /* Resolve name: if it contains '/', use as-is; else prepend /bin/ */
            char path[256];
            int has_slash = 0;
            for (const char *s = user_name; *s && (size_t)(s - user_name) < 64; s++)
                if (*s == '/') { has_slash = 1; break; }
            if (has_slash) {
                int i = 0;
                for (; i < 254 && user_name[i]; i++)
                    path[i] = user_name[i];
                path[i] = '\0';   /* was missing → path ran into stack garbage */
            } else {
                path[0]='/'; path[1]='b'; path[2]='i'; path[3]='n'; path[4]='/';
                int i = 5;
                for (; i < 254 && user_name[i-5]; i++)
                    path[i] = user_name[i-5];
                path[i] = '\0';
            }
            uint32_t uid = (this_cpu() && this_cpu()->current) ? this_cpu()->current->uid : 0;
            int pid = process_spawn_path(path, 0, 0, uid, uid);

            /* Pass optional argument (a1) to the child, same as x86. */
            if (pid > 0 && a1 != 0) {
                const char *arg = (const char *)(uintptr_t)a1;
                size_t i = 0;
                for (; i + 1 < sizeof(g_spawn_arg) && arg[i]; i++)
                    g_spawn_arg[i] = arg[i];
                g_spawn_arg[i] = '\0';
            } else {
                g_spawn_arg[0] = '\0';
            }

            regs[0] = (uint64_t)(int64_t)pid;
            return;
        }
        case SYS_WAITPID: {      /* 6: wait for a child process. a0=pid */
            uint32_t wait_pid = (uint32_t)a0;
            /* Find child in process table */
            struct process *child = 0;
            for (int i = 0; i < 16; i++) {
                if (g_procs[i].loaded && g_procs[i].pid == wait_pid) {
                    child = &g_procs[i];
                    break;
                }
            }
            if (child && child->state == PROCESS_STATE_EXITED) {
                /* Child already exited — return its exit code immediately. */
                regs[0] = (uint64_t)(int64_t)child->exit_code;
            } else if (child && child->loaded) {
                /* Child still running — block until it exits. */
                struct process *cur = (this_cpu() && this_cpu()->current) ? this_cpu()->current : 0;
                if (!cur) { regs[0] = (uint64_t)-1; return; }
                cur->wait_target_pid = wait_pid;
                cur->state = PROCESS_STATE_WAITING;
                arm64_sleep_current(regs);
                /* arm64_sleep_current does not return — we come back via
                 * arm64_resume_user with x0 set by wake_waiters(). */
            } else {
                regs[0] = (uint64_t)-1;  /* no such child */
            }
            return;
        }
        case SYS_LOG: {          /* 33: journal log — write to serial */
            /* a0 = level (unused), a1 = message string */
            const char *msg = (const char *)(uintptr_t)a1;
            if (msg) { serial_write("[app] "); serial_write(msg); serial_write("\r\n"); }
            regs[0] = 0;
            return;
        }
        case SYS_GETPID:        /* 53: get current process ID */
            regs[0] = (uint64_t)((this_cpu() && this_cpu()->current) ? this_cpu()->current->pid : 0);
            return;
        case SYS_GETUID:        /* 55: get current user ID */
            regs[0] = (uint64_t)((this_cpu() && this_cpu()->current) ? this_cpu()->current->uid : 0);
            return;
        case SYS_REBOOT:        /* 51: reboot the system */
        case SYS_SHUTDOWN:      /* 52: shutdown/halt the system */
            serial_write("\r\n[shutdown] halting...\r\n");
            __asm__ volatile("msr daifset, #3");
            while (1) __asm__ volatile("wfi");
        case SYS_SBRK: {        /* 42: grow/shrink process heap */
            intptr_t inc = (intptr_t)a0;
            struct process *cur = (this_cpu() && this_cpu()->current)
                                  ? this_cpu()->current : (struct process *)0;
            if (!cur) { regs[0] = (uint64_t)-1; return; }
            uintptr_t old_break = cur->heap_break;
            static int sbrk_dbg;
            if (!sbrk_dbg) {
                sbrk_dbg = 1;
                serial_write("[sbrk] inc=0x"); serial_write_hex_u64((uint64_t)inc);
                serial_write(" old=0x"); serial_write_hex_u64(old_break);
                serial_write(" start=0x"); serial_write_hex_u64(cur->heap_start);
                serial_write(" top=0x"); serial_write_hex_u64(cur->user_stack_top);
                serial_write("\r\n");
            }
            if (inc == 0) { regs[0] = (uint64_t)old_break; return; }
            uintptr_t new_break = old_break + (uintptr_t)inc;
            if (new_break > cur->user_stack_top - 0x40000 ||
                new_break < cur->heap_start) {
                serial_write("[sbrk] FAILED new=0x"); serial_write_hex_u64(new_break);
                serial_write("\r\n");
                regs[0] = (uint64_t)-1; return;
            }
            cur->heap_break = new_break;
            regs[0] = (uint64_t)old_break;
            return;
        }
        case SYS_PROCESS_KILL: { /* 29: kill a process by PID. a0=pid */
            uint32_t target = (uint32_t)a0;
            struct process *cur = (this_cpu() && this_cpu()->current) ? this_cpu()->current : 0;
            int result = process_kill(target);
            regs[0] = (uint64_t)(int64_t)result;
            /* If we killed ourselves, our aspace is gone — don't ERET to it.
             * Yield to let the scheduler switch to another process. */
            if (cur && cur->state == PROCESS_STATE_EXITED) {
                arm64_yield_current(regs);
                /* not reached */
            }
            return;
        }
        case SYS_DESKTOP_STATUS: { /* 43: top-bar status snapshot */
            struct winsys_desktop_status *out =
                (struct winsys_desktop_status *)(uintptr_t)a0;
            struct desktop_state *d = desktop_active();
            if (!d || !out) { regs[0] = (uint64_t)-1; return; }
            desktop_fill_status(d, out);
            regs[0] = 0;
            return;
        }
        case SYS_WINDOW_SET_MENU: { /* 30: set context menu items for a window */
            uint32_t pid = (this_cpu() && this_cpu()->current)
                           ? this_cpu()->current->pid : 0;
            struct desktop_state *d = desktop_active();
            const struct winsys_menu_item *items = (const struct winsys_menu_item *)(uintptr_t)a1;
            if (!d) { regs[0] = (uint64_t)-1; return; }
            regs[0] = (uint64_t)(int64_t)desktop_app_set_menu(
                d, pid, (int)a0, items, (int)a2);
            return;
        }
        case SYS_THREAD_CREATE: { /* 34: create a thread sharing parent's address space */
            /* a0 = entry fn (user ptr), a1 = stack top (user ptr), a2 = arg */
            struct process *cur = (this_cpu() && this_cpu()->current)
                                  ? this_cpu()->current : (struct process *)0;
            if (!cur) { regs[0] = (uint64_t)-1; return; }
            int tid = process_create_thread(cur, (uintptr_t)a0, (uintptr_t)a1, a2);
            regs[0] = (uint64_t)(int64_t)tid;
            return;
        }
        case SYS_MENU_DISPATCH: { /* 44: deliver menu action to focused window */
            struct desktop_state *d = desktop_active();
            if (!d) { regs[0] = (uint64_t)-1; return; }
            desktop_dispatch_menu_action(d, (uint32_t)a0);
            regs[0] = 0;
            return;
        }
        case SYS_WINDOW_SET_MENUBAR: { /* 37: set menu bar items for a window */
            struct desktop_state *d = desktop_active();
            if (!d) { regs[0] = (uint64_t)-1; return; }
            const struct winsys_menubar_item *items =
                (const struct winsys_menubar_item *)(uintptr_t)a1;
            int count = (int)a2;
            uint32_t pid = (this_cpu() && this_cpu()->current) ? this_cpu()->current->pid : 0;
            int result = desktop_app_set_menubar(d, pid, (int)a0, items, count);
            regs[0] = (uint64_t)(int64_t)result;
            return;
        }
        case SYS_EVENT_POLL: {  /* 19: poll for window events */
            /* rdi = win_id, rsi = struct winsys_event* (out) */
            struct desktop_state *d = desktop_active();
            if (!d) { regs[0] = 0; return; }
            uint32_t pid = (this_cpu() && this_cpu()->current) ? this_cpu()->current->pid : 0;
            struct winsys_event *out = (struct winsys_event *)(uintptr_t)a1;
            int result = desktop_app_poll_event(d, pid, (int)a0, out);
            regs[0] = (uint64_t)result;
            /* No event pending → cooperatively yield: suspend the app and let
             * the compositor (and other apps) run.  The app resumes here on a
             * later slice and the svc returns 0, so its existing event loop
             * interleaves with no app-side changes.  When an event IS ready we
             * return immediately so the queue drains promptly. */
            if (result == 0)
                arm64_yield_current(regs);  /* does not return */
            return;
        }
        case SYS_PTY_OPEN: {    /* 45: allocate a pseudo-terminal master fd */
            /* Allocate a free fd slot for the PTY master endpoint.
             * Uses the global g_pty ring buffers for Terminal ↔ Shell I/O. */
            int fd = -1;
            for (int i = 3; i < ARM64_MAX_FD; i++) {
                if (!g_files[i].used) { fd = i; break; }
            }
            if (fd < 0) { regs[0] = (uint64_t)-1; return; }
            /* Reset PTY buffers */
            memset(&g_pty, 0, sizeof(g_pty));
            g_pty.master_fd = fd;
            g_files[fd].used = 1;
            g_files[fd].type = ARM64_FD_TYPE_PTY;
            g_files[fd].writable = 1;
            g_files[fd].pty = &g_pty;
            regs[0] = (uint64_t)(int64_t)fd;
            return;
        }
        case SYS_SPAWN_PTY: {   /* 46: spawn program with PTY slave on fd 0,1,2 */
            /* a0 = path, a1 = PTY master fd.
             * Spawn child with its fd 0,1,2 reading/writing the PTY buffers. */
            char path[256];
            copy_user_str(path, (const char *)(uintptr_t)a0, sizeof(path));
            if (a1 < 3 || a1 >= ARM64_MAX_FD || !g_files[a1].used ||
                g_files[a1].type != ARM64_FD_TYPE_PTY) {
                regs[0] = (uint64_t)-1; return;
            }
            struct arm64_pty *pty = g_files[a1].pty;
            int child_pid = process_spawn_path(path, 0, 0, 0, 0);
            if (child_pid >= 0) pty->child_pid = (uint32_t)child_pid;
            serial_write("[pty] spawn child_pid="); serial_write_hex_u64((uint64_t)child_pid);
            serial_write("\r\n");
            regs[0] = (uint64_t)(int64_t)child_pid;
            return;
        }
        case SYS_PTY_INTERRUPT: { /* 47: Ctrl+C to foreground job of PTY */
            struct arm64_pty *pty = &g_pty;
            if (pty->child_pid) {
                process_kill(pty->child_pid);
                regs[0] = 0;
            } else {
                regs[0] = (uint64_t)-1;  /* no foreground job */
            }
            return;
        }
        case SYS_CLIPBOARD_SET: { /* 61: set clipboard, a0=data, a1=len */
            const char *data = (const char *)(uintptr_t)a0;
            uint32_t   len   = (uint32_t)a1;
            if (!data || len > CLIPBOARD_MAX_BYTES) {
                regs[0] = (uint64_t)-1;
                return;
            }
            regs[0] = (uint64_t)clipboard_set(data, len);
            return;
        }
        case SYS_CLIPBOARD_GET: { /* 62: get clipboard, a0=buf, a1=cap */
            char    *buf = (char *)(uintptr_t)a0;
            uint32_t cap = (uint32_t)a1;
            regs[0] = (uint64_t)clipboard_get(buf, cap);
            return;
        }
        case SYS_CLIPBOARD_LEN:   /* 63: get clipboard length */
            regs[0] = (uint64_t)clipboard_len();
            return;
        default:
            serial_write("[svc] unknown syscall ");
            serial_write_hex_u64(num);
            serial_write("\r\n");
            regs[0] = (uint64_t)-1;
            return;
        }
    }

    /* Non-syscall synchronous exception from EL0 (e.g. user fault) */
    serial_write("\r\n!!! arm64 EL0 FAULT !!!\r\n");
    serial_write("  EC  = "); serial_write_hex_u64(ec);  serial_write("\r\n");
    serial_write("  ELR = "); serial_write_hex_u64(elr); serial_write("\r\n");
    serial_write("  FAR = "); serial_write_hex_u64(far); serial_write("\r\n");
    arm64_return_to_kernel((uint64_t)-1);
}

void arm64_unhandled_exception(uint64_t esr, uint64_t elr, uint64_t far) {
    serial_write("\r\n!!! arm64 UNHANDLED EXCEPTION !!!\r\n");
    serial_write("  ESR = "); serial_write_hex_u64(esr); serial_write("\r\n");
    serial_write("  ELR = "); serial_write_hex_u64(elr); serial_write("\r\n");
    serial_write("  FAR = "); serial_write_hex_u64(far); serial_write("\r\n");
    while (1) __asm__ volatile("wfi");
}

/* ---- Polling tick ---------------------------------------------------- */
static void poll_timer(void) {
    if (arm64_timer_poll())
        g_tick++;
}

/* ---- Print decimal ----------------------------------------------------- */
static void print_dec(uint64_t v) {
    char buf[21]; int i = 0;
    if (!v) { serial_write_char('0'); return; }
    while (v) { buf[i++] = (char)('0' + v % 10); v /= 10; }
    while (i--) serial_write_char(buf[i]);
}

static int str_eq(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == '\0' && *b == '\0';
}

static int str_starts(const char *s, const char *pfx) {
    while (*pfx) if (*s++ != *pfx++) return 0;
    return 1;
}

/* ---- Memory detection ------------------------------------------------- */
static void detect_memory(uintptr_t *hs, size_t *hz) {
    *hs = 0x40800000UL;         /* 6 MB past kernel load */
    *hz = 320UL * 1024 * 1024;  /* main kmalloc heap → up to 0x54800000 */
}

/* The compositor allocates window surface/content buffers from a separate GFX
 * heap (gfx_alloc, like x86). Carve it out of RAM after the main heap, still
 * inside the identity-mapped RAM block (0x40000000-0x7FFFFFFF, QEMU -m 512M
 * backs up to 0x5FFFFFFF). Without this, gfx_alloc returns NULL and window
 * creation (desktop_app_create_ex) fails — dock/topbar can't open windows. */
#define ARM64_GFX_BASE  0x54800000UL   /* right after the 320 MB main heap */
#define ARM64_GFX_SIZE  (64UL * 1024 * 1024)
#define ARM64_IMG_BASE  0x58800000UL   /* after gfx */
#define ARM64_IMG_SIZE  (16UL * 1024 * 1024)

/* ---- Filesystem state ------------------------------------------------- */
/* Block device and filesystem — keep large structs 64-byte aligned so that
 * struct-copy STP/LDP pairs generated by clang never cross a misaligned
 * boundary. The ramdisk_device struct contains the read/write function
 * pointers; if a struct copy goes out of bounds and overwrites these, the
 * next ramdisk_read call will jump to a garbage address (PC alignment fault).
 * Explicit alignment + 64-byte separation guards between them. */
static void fs_init(void) {
    if (virtio_blk_init() != 0) return;
    if (virtio_blk_get_device(&g_blk_dev) != 0) return;
    serial_write("[fs] virtio-blk OK, mounting ext2...\r\n");
    if (ext2_mount(&g_fs, &g_blk_dev) != 0) {
        serial_write("[fs] ext2_mount failed\r\n");
        return;
    }
    g_fs_ready = 1;
    serial_write("[fs] ext2 mounted\r\n");
}

/* ---- Shell commands --------------------------------------------------- */
static void gui_run(void);   /* starts the desktop compositor (defined below) */

static void cmd_help(void) {
    serial_write("  help          — this message\r\n");
    serial_write("  uname         — kernel info\r\n");
    serial_write("  mem           — heap usage\r\n");
    serial_write("  uptime        — seconds since boot\r\n");
    serial_write("  cpuinfo       — CPU registers\r\n");
    serial_write("  run           — run the built-in EL0 demo (SVC syscalls)\r\n");
    serial_write("  exec <path>   — load+run an aarch64 ELF from disk at EL0\r\n");
    serial_write("  ls [path]     — list directory\r\n");
    serial_write("  cat <path>    — print file\r\n");
    serial_write("  cd <path>     — change directory\r\n");
    serial_write("  pwd           — print working directory\r\n");
    serial_write("  gui / desktop — start the desktop compositor\r\n");
    serial_write("  halt          — stop the machine\r\n");
}

static void cmd_uname(void) {
    serial_write("VibeOS arm64 | QEMU virt | ARM generic timer | ");
    uint64_t midr = read_sysreg(midr_el1);
    /* implementer 0x61 = Apple */
    if (((midr >> 24) & 0xFF) == 0x61)
        serial_write("HVF\r\n");
    else
        serial_write("TCG\r\n");
    serial_write("  MIDR_EL1 = "); serial_write_hex_u64(midr); serial_write("\r\n");
}

static void cmd_mem(void) {
    size_t total = kmalloc_get_total();
    size_t used  = kmalloc_get_used();
    /* Derive free from total - used to avoid the free-list walk which
     * traverses all blocks and may touch unaligned pointers on arm64. */
    size_t free  = (total > used) ? (total - used) : 0;
    serial_write("  total="); print_dec(total >> 10);
    serial_write(" KB  used="); print_dec(used >> 10);
    serial_write(" KB  free="); print_dec(free >> 10);
    serial_write(" KB\r\n");
}

static void cmd_uptime(void) {
    poll_timer();  /* flush pending ticks */
    uint64_t t = g_tick;
    print_dec(t / 100); serial_write(".");
    print_dec((t % 100) / 10);
    serial_write(" s  ("); print_dec(t); serial_write(" ticks)\r\n");
}

static void cmd_ls(const char *path) {
    if (!g_fs_ready) { serial_write("  no filesystem\r\n"); return; }
    if (!path || !*path) path = g_cwd;

    /* path may come from g_cwd; ext2_lookup_inode wants an absolute path */
    uint32_t ino = ext2_lookup_inode(&g_fs, path);
    if (!ino) { serial_write("  not found: "); serial_write(path); serial_write("\r\n"); return; }

    /* ext2_dir_entry is 264 bytes; keep the array small so it doesn't blow the
     * 16 KB boot stack (64 entries = 16.5 KB → stack overflow corrupting BSS). */
    static struct ext2_dir_entry entries[64];
    int n = ext2_readdir(&g_fs, ino, entries, 64);
    if (n < 0) { serial_write("  readdir failed\r\n"); return; }

    for (int i = 0; i < n; i++) {
        /* skip . and .. for cleanliness */
        if (entries[i].name_len == 1 && entries[i].name[0] == '.') continue;
        if (entries[i].name_len == 2 && entries[i].name[0] == '.' && entries[i].name[1] == '.') continue;
        char namebuf[256];
        size_t nl = entries[i].name_len < 255 ? entries[i].name_len : 255;
        for (size_t k = 0; k < nl; k++) namebuf[k] = entries[i].name[k];
        namebuf[nl] = '\0';
        /* file_type 2 = directory */
        if (entries[i].file_type == 2) serial_write("  [dir]  ");
        else                           serial_write("  [file] ");
        serial_write(namebuf); serial_write("\r\n");
    }
}

static void cmd_cat(const char *path) {
    if (!g_fs_ready) { serial_write("  no filesystem\r\n"); return; }
    if (!path || !*path) { serial_write("  usage: cat <path>\r\n"); return; }

    /* resolve absolute path */
    char abspath[256];
    if (path[0] == '/') {
        size_t i;
        for (i = 0; path[i] && i < 255; i++) abspath[i] = path[i];
        abspath[i] = '\0';
    } else {
        /* prepend cwd */
        size_t cl = 0; while (g_cwd[cl]) cl++;
        size_t i;
        for (i = 0; i < cl && i < 254; i++) abspath[i] = g_cwd[i];
        if (i > 0 && abspath[i-1] != '/') abspath[i++] = '/';
        for (size_t j = 0; path[j] && i < 255; j++, i++) abspath[i] = path[j];
        abspath[i] = '\0';
    }

    uint32_t ino = ext2_lookup_inode(&g_fs, abspath);
    if (!ino) { serial_write("  not found: "); serial_write(abspath); serial_write("\r\n"); return; }

    struct ext2_inode *node = &g_fs.inode_table[ino - 1];
    uint32_t filesize = node->size;
    if (filesize == 0) { serial_write("  (empty)\r\n"); return; }
    if (filesize > 65536) { serial_write("  file too large to cat\r\n"); return; }

    uint8_t *buf = (uint8_t *)kmalloc(filesize + 1);
    if (!buf) { serial_write("  OOM\r\n"); return; }

    ssize_t got = ext2_read(&g_fs, ino, 0, filesize, buf);
    if (got < 0) { serial_write("  read error\r\n"); kfree(buf); return; }

    buf[got] = '\0';
    for (ssize_t i = 0; i < got; i++) {
        if (buf[i] == '\n') serial_write_char('\r');
        serial_write_char((char)buf[i]);
    }
    serial_write("\r\n");
    kfree(buf);
}

static void cmd_cd(const char *path) {
    if (!g_fs_ready) { serial_write("  no filesystem\r\n"); return; }
    if (!path || !*path || str_eq(path, "/")) { g_cwd[0]='/'; g_cwd[1]='\0'; return; }

    /* Resolve */
    char newpath[256];
    if (path[0] == '/') {
        size_t i;
        for (i = 0; path[i] && i < 255; i++) newpath[i] = path[i];
        newpath[i] = '\0';
    } else {
        size_t cl = 0; while (g_cwd[cl]) cl++;
        size_t i;
        for (i = 0; i < cl && i < 254; i++) newpath[i] = g_cwd[i];
        if (i > 0 && newpath[i-1] != '/') newpath[i++] = '/';
        for (size_t j = 0; path[j] && i < 255; j++, i++) newpath[i] = path[j];
        newpath[i] = '\0';
    }

    uint32_t ino = ext2_lookup_inode(&g_fs, newpath);
    if (!ino) { serial_write("  not found\r\n"); return; }
    struct ext2_inode *node = &g_fs.inode_table[ino - 1];
    if ((node->mode & 0xF000) != 0x4000) { serial_write("  not a directory\r\n"); return; }

    size_t i;
    for (i = 0; newpath[i] && i < 255; i++) g_cwd[i] = newpath[i];
    g_cwd[i] = '\0';
}

/* Embedded EL0 demo program (from user_demo.S) */
extern uint8_t user_demo_start[];
extern uint8_t user_demo_end[];
extern uint64_t arm64_enter_user(uint64_t entry, uint64_t user_sp,
                                 uint64_t argc, uint64_t argv);

static void cmd_run(void) {
    /* Copy the position-independent EL0 demo into a fresh RAM page and run it
     * at EL0. The page is in the identity-mapped RAM block, which is mapped
     * EL0-accessible + executable (see mmu.c block_normal). */
    size_t len = (size_t)(user_demo_end - user_demo_start);
    uint8_t *code = (uint8_t *)kmalloc(4096);
    uint8_t *stack = (uint8_t *)kmalloc(8192);
    if (!code || !stack) { serial_write("  OOM\r\n"); return; }

    for (size_t i = 0; i < len; i++) code[i] = user_demo_start[i];
    /* Ensure I-cache sees the freshly written instructions */
    __asm__ volatile("dc cvau, %0" :: "r"(code) : "memory");
    __asm__ volatile("dsb ish; ic ialluis; dsb ish; isb" ::: "memory");

    /* The kernel buffers live at PA 0x40000000+. EL0 sees the same physical
     * RAM through the alias mapping at +0x40000000 (VA 0x80000000+), where it
     * is mapped EL0-accessible + executable. Convert the kernel VAs. */
    #define EL0_ALIAS 0x40000000ULL
    uint64_t code_el0  = (uint64_t)(uintptr_t)code  + EL0_ALIAS;
    uint64_t stack_top = ((uint64_t)(uintptr_t)stack + 8192 + EL0_ALIAS) & ~0xFULL;

    serial_write("[run] entering EL0 at ");
    serial_write_hex_u64(code_el0);
    serial_write("\r\n");

    uint64_t code_ret = arm64_enter_user(code_el0, stack_top, 0, 0);

    serial_write("[run] back in kernel, exit code = ");
    serial_write_hex_u64(code_ret);
    serial_write("\r\n");

    kfree(code);
    kfree(stack);
}

/* ---- ELF loader for EL0 programs read from disk ----------------------- *
 * arm64 user ELFs are linked for VA 0x90000000 (see user/arm64 linker).
 * The EL0-alias maps VA 0x80000000.. → PA 0x40000000.., so a user virtual
 * address v is backed by physical address (v - 0x40000000), which the kernel
 * (EL1) reaches through its identity map at the same PA. We copy each LOAD
 * segment to that PA and enter EL0 at the ELF entry point. */
#define USER_VA_BASE 0x90000000ULL
#define ALIAS_OFF    0x40000000ULL   /* user VA → kernel/PA offset */

/* argv staging area in the user region: VA 0x90F00000 (PA 0x50F00000),
 * below the stack top (0x91000000) and above the loaded image (0x90000000). */
#define ARGV_VA 0x90F00000ULL

static void cmd_exec(const char *cmdline) {
    if (!g_fs_ready) { serial_write("  no filesystem\r\n"); return; }
    if (!cmdline || !*cmdline) { serial_write("  usage: exec <path> [args]\r\n"); return; }

    /* Split the command line into the program path (arg0) and a copy we can
     * tokenise in place. */
    char path[256];
    size_t pi = 0;
    const char *p = cmdline;
    while (*p == ' ') p++;
    while (*p && *p != ' ' && pi + 1 < sizeof(path)) path[pi++] = *p++;
    path[pi] = '\0';
    if (path[0] == '\0') { serial_write("  usage: exec <path> [args]\r\n"); return; }

    /* Everything after the path becomes the argument string that SYS_GETARG
     * returns; the common crt0.c splits it into argv (argv[0]="app"). */
    while (*p == ' ') p++;
    {
        size_t i = 0;
        for (; i + 1 < sizeof(g_spawn_arg) && p[i]; i++) g_spawn_arg[i] = p[i];
        g_spawn_arg[i] = '\0';
    }

    uint32_t ino = ext2_lookup_inode(&g_fs, path);
    if (!ino) { serial_write("  not found: "); serial_write(path); serial_write("\r\n"); return; }

    struct ext2_inode *node = &g_fs.inode_table[ino - 1];
    uint32_t fsize = node->size;
    if (fsize == 0 || fsize > (4u << 20)) { serial_write("  bad size\r\n"); return; }

    uint8_t *buf = (uint8_t *)kmalloc(fsize);
    if (!buf) { serial_write("  OOM\r\n"); return; }
    ssize_t got = ext2_read(&g_fs, ino, 0, fsize, buf);
    if (got <= 0) { serial_write("  read error\r\n"); kfree(buf); return; }

    if (!elf64_validate(buf, (size_t)got)) {
        serial_write("  not a valid aarch64 ELF executable\r\n");
        kfree(buf); return;
    }

    struct elf64_header *eh = (struct elf64_header *)buf;
    struct elf64_program_header *ph =
        (struct elf64_program_header *)(buf + eh->program_header_offset);

    /* Copy each PT_LOAD segment to PA = vaddr - ALIAS_OFF */
    uint64_t image_end = 0;
    for (int i = 0; i < eh->program_header_count; i++) {
        if (ph[i].type != ELF_PROGRAM_TYPE_LOAD) continue;
        uint64_t vaddr = ph[i].virtual_address;
        if (vaddr < USER_VA_BASE) {
            serial_write("  segment below USER_VA_BASE — bad link address\r\n");
            kfree(buf); return;
        }
        uint8_t *dst = (uint8_t *)(uintptr_t)(vaddr - ALIAS_OFF);  /* kernel view = PA */
        /* copy file_size bytes, zero the rest up to memory_size (.bss) */
        for (uint64_t b = 0; b < ph[i].file_size; b++)
            dst[b] = buf[ph[i].offset + b];
        for (uint64_t b = ph[i].file_size; b < ph[i].memory_size; b++)
            dst[b] = 0;
        /* Track the highest end-of-segment address for heap placement */
        uint64_t seg_end = (vaddr - USER_VA_BASE) + ph[i].memory_size;
        if (seg_end > image_end) image_end = seg_end;
    }

    /* Make the just-written instructions visible to the I-cache */
    __asm__ volatile("dsb ish; ic ialluis; dsb ish; isb" ::: "memory");

    /* Register a minimal process entry so SYS_SBRK has a heap to work with.
     * Without this, TCC and other malloc-using apps fail with "memory full"
     * from the serial shell — cmd_exec doesn't go through the process
     * manager, so no struct process is created by default.
     * ARM64_MAX_PROCS is 16 (matching process.c). */
#define CMD_EXEC_MAX_PROCS 16
    int proc_slot = -1;
    for (int i = 0; i < CMD_EXEC_MAX_PROCS; i++) {
        if (!g_procs[i].loaded) { proc_slot = i; break; }
    }
    if (proc_slot >= 0) {
        struct process *proc = &g_procs[proc_slot];
        proc->loaded     = 1;
        proc->state      = PROCESS_STATE_RUNNING;
        proc->entry      = eh->entry;
        proc->code_size  = image_end;
        /* Stack at the top of the shared 16 MB region, 256 KB guard */
        proc->user_stack_top      = USER_VA_BASE + 0x01000000ULL - 16;
        proc->user_virtual_base   = USER_VA_BASE;
        proc->heap_start          = (USER_VA_BASE + image_end + 0xFFF) & ~0xFFF;
        proc->heap_break          = proc->heap_start;
        proc->pid                 = 0;  /* legacy: serial path has no PID tracking */
        proc->parent_pid          = 0;
        proc->uid                 = 0;
        proc->gid                 = 0;
        proc->runtime_ticks       = 0;
        proc->cr3                 = 0;  /* shared identity map */
        proc->user_image_allocation = 0;  /* serial path: freed by cmd_exec */

        /* Extract name from path */
        {
            const char *name = path;
            for (const char *s = path; *s; s++)
                if (*s == '/') name = s + 1;
            size_t ni = 0;
            for (; ni < sizeof(proc->name) - 1 && name[ni] && name[ni] != ' '; ni++)
                proc->name[ni] = name[ni];
            proc->name[ni] = '\0';
        }
        memset(&proc->context, 0, sizeof(proc->context));

        if (this_cpu()) this_cpu()->current = proc;
    }

    /* User stack top: VA 0x91000000 (PA 0x51000000). Args are delivered via
     * SYS_GETARG (g_spawn_arg), which crt0.c splits into argv — same as x86. */
    uint64_t user_sp = (USER_VA_BASE + 0x01000000ULL) & ~0xFULL;

    serial_write("[exec] "); serial_write(path);
    serial_write(" entry="); serial_write_hex_u64(eh->entry);
    serial_write("\r\n");

    uint64_t rc = arm64_enter_user(eh->entry, user_sp, 0, 0);

    serial_write("[exec] exit code = "); serial_write_hex_u64(rc); serial_write("\r\n");

    /* Clean up the serial-exec process slot if one was allocated.
     * SYS_EXIT may have already cleared it via process_handle_exit;
     * this is idempotent — just ensures we don't leak. */
    if (proc_slot >= 0 && g_procs[proc_slot].loaded) {
        g_procs[proc_slot].loaded = 0;
    }
    if (this_cpu()) this_cpu()->current = 0;
    kfree(buf);
}

static void cmd_fb(void) {
    if (ramfb_init(1280, 720) != 0) {
        serial_write("  ramfb init failed\r\n");
        return;
    }
    /* Drive the SHARED kernel renderer (kernel/src/render.c — the exact same
     * code x86 uses) over the ramfb framebuffer. This proves portable kernel
     * graphics code runs unmodified on arm64. */
    struct framebuffer fb;
    fb_init(&fb, (uintptr_t)ramfb_buffer(), ramfb_width(), ramfb_height(),
            ramfb_stride_px() * 4, 32);

    draw_gradient_background(&fb, 0xff202838, 0xff0a0c14);
    draw_rounded_panel(&fb, 220, 200, 360, 200, 16,
                       0xff2a3550, 0xff1a2030, 0xff5080c0, 0xff80a0e0);
    draw_text(&fb, 250, 240, "VibeOS arm64", 0xffffffff, 3);
    draw_text(&fb, 250, 300, "shared kernel renderer", 0xff90b0e0, 2);
    serial_write("  rendered via shared kernel/src/render.c — check the display\r\n");
}


/* Spawn a process via the process manager and run it cooperatively. */
static void cmd_spawn(const char *path) {
    while (*path == ' ') path++;
    if (!*path) { serial_write("  usage: spawn <path>\r\n"); return; }
    int pid = process_spawn_path(path, 0, 0, 0, 0);
    if (pid <= 0) { serial_write("  spawn failed\r\n"); return; }
    serial_write("  spawned pid=");
    { char b[16]; int i=0; uint32_t t=(uint32_t)pid; if(!t)b[i++]='0';
      else while(t){b[i++]=(char)('0'+t%10);t/=10;}
      while(i--) serial_write_char(b[i]); }
    serial_write(" -- running...\r\n");
    process_run_ready_slice();
    serial_write("  back in kernel\r\n");
}

/* Shell command: show or fake mouse state for testing virtio-input. */
static void cmd_mouse(void) {
    serial_write("  mouse: x="); print_dec(g_mouse_x);
    serial_write(" y=");        print_dec(g_mouse_y);
    serial_write(" buttons=");  print_dec(g_mouse_buttons);
    serial_write(" moved=");    print_dec(g_mouse_moved);
    serial_write("\r\n");
    if (!virtio_input_is_ready()) {
        serial_write("  (virtio-input not ready)\r\n");
    } else {
        serial_write("  virtio-input ready — move pointer over QEMU window\r\n");
    }
}

static void cmd_cpuinfo(void) {
    serial_write("  MIDR_EL1  = "); serial_write_hex_u64(read_sysreg(midr_el1)); serial_write("\r\n");
    serial_write("  CNTFRQ    = "); print_dec(read_sysreg(cntfrq_el0) / 1000000);
    serial_write(" MHz\r\n");
    serial_write("  CNTPCT    = "); serial_write_hex_u64(read_sysreg(cntpct_el0)); serial_write("\r\n");
    serial_write("  SCTLR_EL1 = "); serial_write_hex_u64(read_sysreg(sctlr_el1)); serial_write("\r\n");
}

static void run_command(const char *line) {
    while (*line == ' ') line++;
    if (!*line) return;
    if      (str_eq(line, "help"))         cmd_help();
    else if (str_eq(line, "uname"))        cmd_uname();
    else if (str_eq(line, "mem"))          cmd_mem();
    else if (str_eq(line, "uptime"))       cmd_uptime();
    else if (str_eq(line, "cpuinfo"))      cmd_cpuinfo();
    else if (str_eq(line, "fb"))           cmd_fb();
    else if (str_eq(line, "mouse"))        cmd_mouse();
    else if (str_eq(line, "run"))          cmd_run();
    else if (str_starts(line, "spawn "))   cmd_spawn(line + 6);
    else if (str_starts(line, "exec "))    cmd_exec(line + 5);
    else if (str_eq(line, "pwd"))          { serial_write(g_cwd); serial_write("\r\n"); }
    else if (str_eq(line, "ls"))           cmd_ls(g_cwd);
    else if (str_starts(line, "ls "))      cmd_ls(line + 3);
    else if (str_starts(line, "cat "))     cmd_cat(line + 4);
    else if (str_starts(line, "cd "))      cmd_cd(line + 3);
    else if (str_eq(line, "cd"))           cmd_cd("/");
    else if (str_eq(line, "halt")) {
        serial_write("halting.\r\n");
        __asm__ volatile("msr daifset, #3");
        while (1) __asm__ volatile("wfi");
    } else if (str_eq(line, "gui") || str_eq(line, "desktop")) {
        gui_run();   /* launch the window-server compositor (does not return) */
    } else {
        /* Unknown builtin — try as an ELF binary.
         * If the command contains '/', use it as-is (absolute or relative path).
         * Otherwise look in /bin/<command>.  This mirrors /bin/sh behaviour:
         *   desktop          → exec /bin/desktop
         *   ./bin/desktop    → exec ./bin/desktop
         *   /bin/desktop     → exec /bin/desktop
         *   desktop --help   → exec /bin/desktop --help
         */
        char exec_line[256];
        int has_slash = 0;
        for (const char *s = line; *s && *s != ' '; s++)
            if (*s == '/') { has_slash = 1; break; }

        if (has_slash) {
            /* Path already contains '/' — use entire line as-is. */
            size_t i = 0;
            for (; i < sizeof(exec_line) - 1 && line[i]; i++)
                exec_line[i] = line[i];
            exec_line[i] = '\0';
        } else {
            /* Prepend /bin/ to the first word, keep any arguments. */
            exec_line[0] = '/'; exec_line[1] = 'b'; exec_line[2] = 'i';
            exec_line[3] = 'n'; exec_line[4] = '/';
            size_t out = 5;
            const char *s = line;
            while (*s && *s != ' ' && out < sizeof(exec_line) - 1)
                exec_line[out++] = *s++;
            /* Copy remaining arguments (if any) */
            while (*s && out < sizeof(exec_line) - 1)
                exec_line[out++] = *s++;
            exec_line[out] = '\0';
        }
        cmd_exec(exec_line);
    }
}

/* ---- Non-blocking UART read, also polls timer ------------------------- */
static int uart_getc_nonblock(char *out) {
    poll_timer();
    if (!arm64_uart_can_read()) return 0;
    *out = arm64_uart_getc();
    return 1;
}

/* Try to read one character from any input source (UART or virtio keyboard).
 * Returns 1 and sets *c on success, 0 if no input available.
 * Checks virtio first so escape sequences (which arrive via virtio_input)
 * are consumed atomically without UART interleaving. */
static int shell_getc(char *c) {
    /* Check virtio keyboard first (QEMU window input) */
    char vk;
    if (virtio_kbd_read(&vk)) {
        *c = vk;
        return 1;
    }
    /* Then check UART (serial port / host terminal) */
    if (arm64_uart_can_read()) {
        *c = arm64_uart_getc();
        return 1;
    }
    return 0;
}

/* Minimal ANSI escape sequence consumer: reads and discards ESC [ X
 * sequences so arrow keys don't print garbage in the serial console.
 * Handles Up/Down for command history; other escape sequences are consumed
 * silently.
 * Returns 0 if no escape sequence was present (c is a normal char),
 * 1 if an escape was consumed and the line may have changed. */
static int shell_consume_escape(char first, char *line, int *pos,
                                char (*history)[256], int *hist_count,
                                int *hist_idx) {
    if (first != 0x1b) return 0;

    char b1 = 0, b2 = 0;

    for (int tries = 0; tries < 50; tries++) {
        if (shell_getc(&b1)) break;
        virtio_input_poll();
        poll_timer();
    }
    if (b1 != '[') return 1;   /* not ESC [ — consume the ESC */

    for (int tries = 0; tries < 50; tries++) {
        if (shell_getc(&b2)) break;
        virtio_input_poll();
        poll_timer();
    }

    /* Up arrow — recall older history entry */
    if (b2 == 'A') {
        if (*hist_count > 0) {
            /* First Up press: save the current line if navigating from scratch */
            if (*hist_idx == 0 && *pos > 0) {
                line[*pos] = '\0';
                size_t j = 0;
                for (; j < 255 && line[j]; j++) history[0][j] = line[j];
                history[0][j] = '\0';
            }
            if (*hist_idx < *hist_count) {
                (*hist_idx)++;
                /* Erase current line */
                while (*pos > 0) { serial_write("\b \b"); (*pos)--; }
                /* Copy history entry to line buffer */
                const char *src = history[(*hist_count) - (*hist_idx)];
                for (; *pos < 255 && src[*pos]; (*pos)++)
                    serial_write_char(line[*pos] = src[*pos]);
                line[*pos] = '\0';
            }
        }
        return 1;
    }

    /* Down arrow — recall newer history entry */
    if (b2 == 'B') {
        if (*hist_idx > 0) {
            (*hist_idx)--;
            while (*pos > 0) { serial_write("\b \b"); (*pos)--; }
            if (*hist_idx > 0) {
                const char *src = history[(*hist_count) - (*hist_idx)];
                for (; *pos < 255 && src[*pos]; (*pos)++)
                    serial_write_char(line[*pos] = src[*pos]);
            }
            line[*pos] = '\0';
        }
        return 1;
    }

    /* All other ESC [ X sequences: consume without action */
    return 1;
}

static void shell_loop(void) {
    static char line[256];
    static char history[8][256];   /* last 8 commands */
    static int  hist_count = 0;     /* number of history entries (0..8) */
    static int  hist_idx   = 0;     /* 0 = typing new line, 1..N = browsing */
    int pos = 0;

    serial_write("VibeOS arm64 — type 'help' for commands\r\n");

    while (1) {
        serial_write("\r\narm64:");
        serial_write(g_cwd);
        serial_write("$ ");
        pos = 0;
        hist_idx = 0;

        for (;;) {
            char c;
            /* Spin until a character arrives, polling timer + input each iteration */
            while (!shell_getc(&c)) {
                virtio_input_poll();
                poll_timer();
            }

            /* Consume ANSI escape sequences.  Up/Down recall history;
             * other escape sequences (Left, Right, Home, End) are consumed
             * silently. */
            if (shell_consume_escape(c, line, &pos,
                                     history, &hist_count, &hist_idx)) continue;

            if (c == '\r' || c == '\n') {
                serial_write("\r\n");
                line[pos] = '\0';
                break;
            } else if ((c == '\b' || c == 0x7f) && pos > 0) {
                pos--;
                serial_write("\b \b");
            } else if (c >= 0x20 && c < 0x7f && pos < 255) {
                line[pos++] = c;
                serial_write_char(c);
            }
            /* All other control characters (including lone ESC) are ignored */
        }
        run_command(line);
        /* Save non-empty commands to history (skip if same as most recent) */
        if (line[0]) {
            int skip = 0;
            if (hist_count > 0) {
                /* Check if identical to the most recent entry */
                const char *prev = history[(hist_count - 1) % 8];
                size_t i = 0;
                for (; i < 255 && line[i] && line[i] == prev[i]; i++);
                if (line[i] == prev[i]) skip = 1;  /* identical */
            }
            if (!skip) {
                if (hist_count < 8) {
                    size_t i = 0;
                    for (; i < 255 && line[i]; i++)
                        history[hist_count][i] = line[i];
                    history[hist_count][i] = '\0';
                    hist_count++;
                } else {
                    /* Shift history up by one (oldest falls off) */
                    for (int h = 0; h < 7; h++) {
                        size_t i = 0;
                        for (; i < 255 && history[h+1][i]; i++)
                            history[h][i] = history[h+1][i];
                        history[h][i] = '\0';
                    }
                    size_t i = 0;
                    for (; i < 255 && line[i]; i++)
                        history[7][i] = line[i];
                    history[7][i] = '\0';
                }
            }
        }
    }
}

/* ---- Kernel entry ----------------------------------------------------- */

static struct desktop_state *g_desktop = 0;
struct desktop_state *desktop_active(void) { return g_desktop; }

/* Framebuffer + backbuffer (updated on resolution change). */
static struct framebuffer g_fb;
static struct framebuffer g_bb;
static uint32_t          *g_backbuf = 0;

void kernel_main_arm64(void) {
    serial_init();

    keymap_init();
    keymap_set("de");   /* German keyboard layout */

    serial_write("\r\n");
    serial_write("  __   ___      _      ___  ____  \r\n");
    serial_write(" \\ \\ / (_)_ __| |__  / _ \\/ ___| \r\n");
    serial_write("  \\ V /| | '__| '_ \\| | | \\___ \\ \r\n");
    serial_write("   \\_/ |_|_|  |_|_|_|\\___/|____/ \r\n");
    serial_write("         arm64  |  HVF  |  M-chip   \r\n\r\n");

    /* CPU info */
    uint64_t freq = read_sysreg(cntfrq_el0);
    serial_write("[arm64] MIDR_EL1 = "); serial_write_hex_u64(read_sysreg(midr_el1));
    serial_write("  CNTFRQ = "); print_dec(freq / 1000000); serial_write(" MHz\r\n");

    /* MMU sanity check */
    if (read_sysreg(sctlr_el1) & 1)
        serial_write("[arm64] MMU on — identity mapping active\r\n");
    else
        serial_write("[arm64] WARNING: MMU off!\r\n");

    /* Heap */
    uintptr_t hs; size_t hz;
    detect_memory(&hs, &hz);
    kmalloc_init(hs, hz);
    serial_write("[arm64] heap "); print_dec(hz >> 20);
    serial_write(" MB at "); serial_write_hex_u64(hs); serial_write("\r\n");

    /* Separate GFX + image heaps for the compositor (window surface buffers). */
    gfx_heap_init(ARM64_GFX_BASE, ARM64_GFX_SIZE);
    image_heap_init(ARM64_IMG_BASE, ARM64_IMG_SIZE);
    serial_write("[arm64] gfx heap 64 MB at "); serial_write_hex_u64(ARM64_GFX_BASE);
    serial_write("\r\n");

    /* Filesystem — try virtio-blk + ext2 (before timer init) */
    fs_init();

    /* Input — try virtio-tablet / virtio-mouse */
    virtio_input_init();

    /* Audio — virtio-sound (optional; absent unless QEMU has -device
     * virtio-sound-device).  Apps fall back to silence if not ready. */
    virtio_snd_init();

    serial_write("[arm64] detecting platform...\r\n");
    /* Detect platform FIRST so we choose the right timer path.
     * Apple implementer = 0x61.  On HVF, GICC MMIO (0x08010000) triggers
     * an ISV=0 assertion in QEMU's HVF backend, so we skip it entirely. */
    uint64_t midr    = read_sysreg(midr_el1);
    int      is_apple = (((midr >> 24) & 0xFF) == 0x61);

    if (is_apple) {
        serial_write("[arm64] Apple HVF detected\r\n");
        /* HVF path: timer in poll mode (IMASK=1, no IRQ, no GIC MMIO). */
        serial_write("[arm64] init timer poll...\r\n");
        arm64_timer_init_poll();
        serial_write("[arm64] timer: poll mode (IMASK=1, no GIC MMIO — HVF)\r\n");
    } else {
        /* TCG / real-hardware path: GIC + IRQ-driven timer. */
        arm64_gic_init();
        arm64_timer_init();          /* ENABLE=1, IMASK=0, GIC IRQ 30 */
        __asm__ volatile("msr daifclr, #2");   /* unmask IRQ */
        serial_write("[arm64] GICv2 + timer IRQ enabled (TCG path)\r\n");
    }

    /* Register the boot CPU so this_cpu()/per-CPU state work in the shell
     * (needed to launch EL0 programs) and later in the compositor. */
    cpu_register(0);

    /* Drop to the interactive shell.  The desktop GUI is NOT started
     * automatically — type `gui` (or `desktop`) to launch the compositor,
     * matching the x86 behaviour. */
    shell_loop();
}

/* Start the desktop compositor: framebuffer + window server + GUI apps, then
 * the render loop.  Invoked from the shell's `gui`/`desktop` command; does not
 * return (the GUI takes over, same as x86). */
static void gui_run(void) {
    /* ---- Desktop Compositor ---- */
    if (ramfb_init(1280, 720) != 0) {
        serial_write("  ramfb init failed\r\n");
        return;
    }

    fb_init(&g_fb, (uintptr_t)ramfb_buffer(), ramfb_width(), ramfb_height(),
            ramfb_stride_px() * 4, 32);

    /* Allocate desktop state on the heap (it's large — contains window
     * storage, app slots, icon state, etc.) */
    g_desktop = (struct desktop_state *)
        kmalloc(sizeof(struct desktop_state));
    if (!g_desktop) { serial_write("  OOM for desktop\r\n"); return; }
    memset(g_desktop, 0, sizeof(struct desktop_state));

    serial_write("[gui] initialising desktop compositor...\r\n");
    desktop_init(g_desktop, ramfb_width(), ramfb_height());

    /* Allocate a backbuffer for flicker-free rendering.
     * We render the scene offscreen, then blit to the visible framebuffer
     * in one operation — this eliminates the tearing/flickering that
     * direct-to-fb rendering causes. */
    size_t bb_pixels = (size_t)ramfb_width() * (size_t)ramfb_height();
    g_backbuf = (uint32_t *)kmalloc(bb_pixels * 4);
    if (g_backbuf) {
        fb_init(&g_bb, (uintptr_t)g_backbuf, ramfb_width(), ramfb_height(),
                ramfb_width() * 4, 32);
        serial_write("[gui] backbuffer allocated\r\n");
    } else {
        /* Fall back to direct rendering if OOM */
        g_bb = g_fb;
        serial_write("[gui] WARNING: no backbuffer (OOM) — may flicker\r\n");
    }
    /* Spawn and run GUI apps sequentially.  Since all arm64 processes
     * share PA 0x50000000 (non-PIE, same VA), each spawn overwrites the
     * previous one.  We spawn-run-exit each app before starting the next.
     * After all apps have created their windows and exited, the compositor
     * renders their static content continuously. */

    /* Run wallpaper first (synchronously) so the background is set before
     * the first render frame — avoids a black flash.  Dock and topbar are
     * spawned into the round-robin queue and run during the render loop. */
    serial_write("[gui] spawning wallpaper...\r\n");
    process_spawn_path("/bin/wallpaper", 0, 0, 0, 0);
    process_run_ready_slice();  /* wallpaper sets background + exits */

    serial_write("[gui] spawning dock + topbar...\r\n");
    process_spawn_path("/bin/dock", 0, 0, 0, 0);
    process_spawn_path("/bin/topbar", 0, 0, 0, 0);

    /* Boot secondary CPUs AFTER all initial disk I/O is done so the ext2
     * driver (which is not yet SMP-safe) isn't accessed concurrently. */
    smp_boot_aps();

    serial_write("[gui] entering render loop\n");
    for (;;) {
        uint64_t frame_start = timer_tick_count();   /* CNTVCT, 24 MHz */
        bkl_acquire();

        /* Deferred resolution change: reinit ramfb + compositor buffers.
         * Must happen with BKL held so no rendering races with it. */
        if (g_res_change_pending) {
            g_res_change_pending = 0;
            serial_write("[gui] resolution change to ");
            serial_write_hex_u64(g_res_w); serial_write("x"); serial_write_hex_u64(g_res_h);
            serial_write("\r\n");

            if (ramfb_set_mode(g_res_w, g_res_h) == 0) {
                /* Free old backbuffer */
                if (g_backbuf && g_bb.base != g_fb.base)
                    kfree(g_backbuf);

                /* Reinit framebuffer and backbuffer */
                fb_init(&g_fb, (uintptr_t)ramfb_buffer(), ramfb_width(), ramfb_height(),
                        ramfb_stride_px() * 4, 32);

                size_t bb_px = (size_t)ramfb_width() * (size_t)ramfb_height();
                g_backbuf = (uint32_t *)kmalloc(bb_px * 4);
                if (g_backbuf) {
                    memset(g_backbuf, 0, bb_px * 4);
                    fb_init(&g_bb, (uintptr_t)g_backbuf, ramfb_width(), ramfb_height(),
                            ramfb_width() * 4, 32);
                } else {
                    g_bb = g_fb;
                }

                /* Update desktop dimensions without destroying running apps.
                 * desktop_init would reset all windows and orphan processes. */
                int old_sw = (int)g_desktop->screen_width;
                int old_sh = (int)g_desktop->screen_height;
                g_desktop->screen_width  = ramfb_width();
                g_desktop->screen_height = ramfb_height();
                g_desktop->tiles_x = (int)(ramfb_width()  / g_desktop->tile_size) + 1;
                g_desktop->tiles_y = (int)(ramfb_height() / g_desktop->tile_size) + 1;
                /* Clamp all visible windows to the new screen bounds */
                for (int wi = 0; wi < WINDOW_COUNT; wi++) {
                    struct window_state *w = &g_desktop->windows[wi];
                    if (!w->title || !w->title[0]) continue;
                    if (w->x < 0) w->x = 0;
                    if (w->y < 0) w->y = 0;
                    if (w->x + w->width  > (int)g_desktop->screen_width)
                        w->x = (int)g_desktop->screen_width - w->width;
                    if (w->y + w->height > (int)g_desktop->screen_height)
                        w->y = (int)g_desktop->screen_height - w->height;
                    if (w->x < 0) w->x = 0;
                    if (w->y < 0) w->y = 0;
                }
                /* Force full redraw on next frame */
                g_desktop->background_dirty = 1;
                g_desktop->dirty = 1;
                g_desktop->dirty_rect.x = 0;
                g_desktop->dirty_rect.y = 0;
                g_desktop->dirty_rect.width = (int)ramfb_width();
                g_desktop->dirty_rect.height = (int)ramfb_height();

                /* Clear dirty tiles — old tile data may have stale dimensions */
                memset(g_desktop->dirty_tiles, 0, sizeof(g_desktop->dirty_tiles));

                /* Mark all visible windows dirty so surfaces are re-rendered
                 * at their new (clamped) dimensions. */
                for (int wi = 0; wi < WINDOW_COUNT; wi++) {
                    if (g_desktop->windows[wi].title && g_desktop->windows[wi].title[0]) {
                        g_desktop->window_dirty[wi] = 1;
                        g_desktop->window_dirty_rects[wi] = (struct rect){
                            .x = 0, .y = 0,
                            .width = g_desktop->windows[wi].width,
                            .height = g_desktop->windows[wi].height
                        };
                    }
                }

                /* Reinitialize background framebuffer at new resolution.
                 * background_storage is DESKTOP_MAX_WIDTH * DESKTOP_MAX_HEIGHT
                 * (static, big enough), so clear it and re-init the fb struct. */
                memset(g_desktop->background_storage, 0,
                       (size_t)g_desktop->screen_width * (size_t)g_desktop->screen_height * 4);
                fb_init(&g_desktop->background_fb,
                        (uintptr_t)g_desktop->background_storage,
                        ramfb_width(), ramfb_height(),
                        ramfb_width() * 4, 32);

                /* Expand full-width windows (topbar) and reposition
                 * edge-anchored windows (dock) for the new resolution.
                 * Do NOT reinit window FBs — their buffer strides remain
                 * at the original allocation width.  The compositor's
                 * fb_blit_rect correctly handles sub-rectangle copies. */
                for (int wi = 0; wi < WINDOW_COUNT; wi++) {
                    struct window_state *w = &g_desktop->windows[wi];
                    if (!w->title || !w->title[0]) continue;
                    /* Full-width windows: were full-width at old resolution,
                     * expand (or shrink) to match the new screen width. */
                    if (w->width == old_sw || w->width > old_sw * 3 / 4) {
                        w->width = (int)ramfb_width();
                        w->x = 0;
                    }
                }
            }
        }

        /* Feed mouse input to the compositor before rendering */
        struct mouse_state mouse;
        struct keyboard_state keyboard;
        memset(&mouse, 0, sizeof(mouse));
        memset(&keyboard, 0, sizeof(keyboard));

        /* Drain UART into keyboard_state so serial keypresses reach the
         * compositor and are forwarded to the focused app (DOOM needs Enter
         * to start, arrow keys to play, etc.).  Non-blocking — only read
         * characters that are already in the RX FIFO.
         * Also drain virtio keyboard events (from QEMU graphical window). */
        while (serial_can_read() && keyboard.count < 32) {
            char c = serial_read_byte();
            if (c == '\r' || c == '\n') keyboard.enter_pressed = 1;
            else if (c == 0x7f || c == '\b') keyboard.backspace_pressed = 1;
            else keyboard.chars[keyboard.count++] = c;
        }
        /* Drain virtio keyboard buffer */
        while (keyboard.count < 32) {
            char c;
            if (!virtio_kbd_read(&c)) break;
            if (c == '\n') keyboard.enter_pressed = 1;
            else if (c == '\b') keyboard.backspace_pressed = 1;
            else keyboard.chars[keyboard.count++] = c;
        }
        if (keyboard.count || keyboard.enter_pressed) {
            serial_write("[kbd] count="); serial_write_hex_u64(keyboard.count);
            serial_write(" enter="); serial_write_hex_u64(keyboard.enter_pressed);
            serial_write("\r\n");
        }

        /* Ctrl+C (0x03) — interrupt the foreground job of any waiting shell.
         * This must happen BEFORE the keyboard bytes reach the focused app
         * (e.g. DOOM), otherwise the app might consume Ctrl+C itself or
         * ignore it, leaving no way to kill a runaway foreground process. */
        for (size_t ki = 0; ki < keyboard.count; ki++) {
            if (keyboard.chars[ki] == 0x03) {
                /* Find any process waiting for a child — that's the shell. */
                for (int pi = 0; pi < 16; pi++) {
                    if (g_procs[pi].loaded &&
                        g_procs[pi].state == PROCESS_STATE_WAITING &&
                        g_procs[pi].wait_target_pid != 0) {
                        process_kill(g_procs[pi].wait_target_pid);
                        break;  /* only kill one foreground job */
                    }
                }
                /* Remove 0x03 from the buffer so it doesn't reach the app. */
                for (size_t m = ki; m + 1 < keyboard.count; m++)
                    keyboard.chars[m] = keyboard.chars[m + 1];
                keyboard.count--;
                ki--;  /* re-check this index (now holds the next char) */
            }
        }

        mouse.x = g_mouse_x * (int)ramfb_width() / 32767;
        mouse.y = g_mouse_y * (int)ramfb_height() / 32767;
        mouse.buttons = (uint8_t)g_mouse_buttons;
        mouse.moved = (uint8_t)g_mouse_moved;
        static int prev_buttons = 0;
        if ((g_mouse_buttons & 1) && !(prev_buttons & 1))
            mouse.left_pressed = 1;
        if (!(g_mouse_buttons & 1) && (prev_buttons & 1))
            mouse.left_released = 1;
        if ((g_mouse_buttons & 2) && !(prev_buttons & 2))
            mouse.right_pressed = 1;
        if (!(g_mouse_buttons & 2) && (prev_buttons & 2))
            mouse.right_released = 1;
        prev_buttons = g_mouse_buttons;

        desktop_handle_input(g_desktop, &mouse, &keyboard);

        /* Reap windows whose owning process has exited (e.g. killed from the
         * task manager) and mix queued audio from all apps to the device. */
        desktop_poll_apps(g_desktop);
        virtio_snd_mix_tick();

        /* Render scene offscreen first (backbuffer), then blit to the
         * visible framebuffer in one shot — no tearing/flickering. */
        desktop_render(g_desktop, &g_bb);
        fb_blit(&g_fb, &g_bb);

        /* Draw cursor directly on the visible framebuffer so it
         * doesn't get smeared by the backbuffer copy. */
        desktop_draw_cursor_overlay(g_desktop, &g_fb);

        /* Clean the framebuffer out of the data cache to Point of
         * Coherency so QEMU's ramfb reader sees every pixel.  DSB
         * alone only orders accesses within the shareability domain;
         * on Apple Silicon's write-back cache dirty lines can sit
         * indefinitely without an explicit DC CVAU/CVAC.
         * We iterate every 64 bytes (the architectural minimum
         * cache-line size) across the entire framebuffer. */
        {
            uintptr_t fb_va   = (uintptr_t)g_fb.base;
            uint32_t  fb_size = g_fb.height * g_fb.pitch;
            for (uintptr_t a = fb_va; a < fb_va + fb_size; a += 64)
                __asm__ volatile("dc cvau, %0" :: "r"(a));
        }
        __asm__ volatile("dsb sy" ::: "memory");

        bkl_release();

        /* Per-CPU tick accounting. */
        struct cpu *c_bsp = this_cpu();
        if (c_bsp) { c_bsp->ticks++; c_bsp->busy_ticks++; }
        if (arm64_timer_poll() && c_bsp) c_bsp->ticks++;

        virtio_input_poll();

        /* Decouple process scheduling from the compositor: the desktop is
         * composited once per ~60 Hz frame above, then we spend the REST of the
         * 16.6 ms budget pumping user process slices (BKL released between each
         * so the AP cores run them too).  Previously the loop ran exactly one
         * slice per full-screen blit, so two CPU-bound apps (e.g. two DOOMs)
         * starved each other and stuttered.  When nothing is runnable we issue a
         * YIELD hint and keep checking so sleeping apps wake on time. */
        {
            const uint64_t FRAME_TICKS = 400000ull;  /* 1/60 s at 24 MHz */
            for (;;) {
                if (timer_tick_count() - frame_start >= FRAME_TICKS) break;
                bkl_acquire();
                int ran = process_run_ready_slice();
                bkl_release();
                if (!ran) __asm__ volatile("yield");
            }
        }
    }
}
