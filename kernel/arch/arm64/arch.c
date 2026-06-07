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
#include "../../include/syscall.h"   /* shared SYS_* numbers (x86 + arm64) */
#include "../../include/framebuffer.h"
#include "../../include/process.h"
#include "../../include/timer.h"
#include "../../include/string.h"
#include "../../include/render.h"
#include "../../include/window.h"
#include "../../include/input.h"
#include "../../include/winsys.h"

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
struct arm64_file {
    int      used;
    int      writable;
    uint32_t ino;
    uint32_t offset;
    uint32_t size;
};
static struct arm64_file g_files[ARM64_MAX_FD];

/* Argument string for the currently-exec'd program (SYS_GETARG returns it).
 * crt0.c reads this and splits it into argv — same mechanism as x86. */
static char g_spawn_arg[512];

/* EL0 exception frame, as laid out by SAVE_REGS in exceptions.S:
 * regs[0..30] = x0..x30, regs[31]=sp_el0, regs[32]=elr_el1, regs[33]=spsr. */
extern void arm64_return_to_kernel(uint64_t code) __attribute__((noreturn));
extern void arm64_yield_current(void *frame) __attribute__((noreturn));
extern struct process g_procs[];
void process_handle_exit(uint64_t code);
extern int g_current;
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
         * the common libc + apps work unmodified across arches. */
        switch (num) {
        case SYS_WRITE: {       /* 1: write(fd, buf, len) */
            const char *buf = (const char *)(uintptr_t)a1;
            if (a0 <= 2) {      /* stdout/stderr → UART */
                for (uint64_t i = 0; i < a2; i++)
                    serial_write_char(buf[i]);
                regs[0] = a2;
            } else if (a0 < ARM64_MAX_FD && g_files[a0].used && g_files[a0].writable) {
                struct arm64_file *f = &g_files[a0];
                ssize_t w = ext2_write(&g_fs, f->ino, f->offset, a2, buf);
                if (w > 0) { f->offset += (uint32_t)w; regs[0] = (uint64_t)w; }
                else regs[0] = (uint64_t)-1;
            } else regs[0] = (uint64_t)-1;
            return;
        }
        case SYS_READ: {        /* 0: read(fd, buf, len) */
            if (a0 < 3 || a0 >= ARM64_MAX_FD || !g_files[a0].used) {
                regs[0] = (uint64_t)-1; return;
            }
            struct arm64_file *f = &g_files[a0];
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
        case SYS_FB_INFO: {     /* 65: fill struct vos_fb_info* with the framebuffer */
            /* struct layout: u64 addr; u32 width,height,stride,bpp; */
            uint32_t *fb = ramfb_buffer();
            if (!fb) {          /* lazily bring up an 800x600 framebuffer */
                if (ramfb_init(800, 600) != 0) { regs[0] = (uint64_t)-1; return; }
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
            g_desktop_uid = (g_current >= 0) ? g_procs[g_current].uid : 0;
            regs[0] = 0;
            return;
        }
        case SYS_WINDOW_CREATE: {  /* 17: create a window */
            struct desktop_state *d = desktop_active();
            if (!d) { regs[0] = (uint64_t)-1; return; }
            const char *title = (const char *)(uintptr_t)a0;
            uint32_t pid = (g_current >= 0) ? g_procs[g_current].pid : 0;
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
            uint32_t pid = (g_current >= 0) ? g_procs[g_current].pid : 0;
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
            uint32_t pid = (g_current >= 0) ? g_procs[g_current].pid : 0;
            int result = desktop_app_present_rect(d, pid, win_id, pixels, w, h,
                                                   dx, dy, dw, dh);
            regs[0] = (uint64_t)(int64_t)result;
            return;
        }
        case SYS_DISPLAY_MODE: {    /* 27: get/set display resolution */
            uint32_t rw = (uint32_t)a0, rh = (uint32_t)a1;
            if (rw == 0) {
                regs[0] = (uint64_t)((800u << 16) | 600u);
            } else {
                regs[0] = 0; /* resolution change not supported on arm64 */
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
            out->cpu_count = 1;
            out->heap_used_bytes = 0;
            out->heap_total_bytes = 0;
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
             * a3=((x&0xffff)<<16)|(y&0xffff), a4=color, a5=scale.
             * The calling app's address space is active, so buf/text are
             * directly accessible — uses the SAME font atlas as the chrome. */
            uint32_t *buf = (uint32_t *)(uintptr_t)a0;
            int buf_w = (int)((a2 >> 16) & 0xffffu);
            int buf_h = (int)(a2 & 0xffffu);
            int x = (int)(int16_t)((a3 >> 16) & 0xffffu);
            int y = (int)(int16_t)(a3 & 0xffffu);
            if (buf == 0) { regs[0] = (uint64_t)-1; return; }
            char text[256];
            copy_user_str(text, (const char *)(uintptr_t)a1, sizeof(text));
            regs[0] = (uint64_t)(int64_t)draw_text_to_argb(buf, buf_w, buf_h,
                x, y, text, (uint32_t)a4, (int)a5);
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
        case SYS_TIMER_SLEEP:   /* 20: suspend one slice (frame-rate paced) */
            regs[0] = 0;
            arm64_yield_current(regs);
            /* not reached */
        case SYS_YIELD:         /* 3: suspend and reschedule (resumes after svc) */
            regs[0] = 0;
            arm64_yield_current(regs);
            /* not reached */
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
                for (int i = 0; i < 254 && user_name[i]; i++)
                    path[i] = user_name[i];
            } else {
                path[0]='/'; path[1]='b'; path[2]='i'; path[3]='n'; path[4]='/';
                int i = 5;
                for (; i < 254 && user_name[i-5]; i++)
                    path[i] = user_name[i-5];
                path[i] = '\0';
            }
            uint32_t uid = (g_current >= 0) ? g_procs[g_current].uid : 0;
            int pid = process_spawn_path(path, 0, 0, uid, uid);
            regs[0] = (uint64_t)(int64_t)pid;
            return;
        }
        case SYS_LOG: {          /* 33: journal log — write to serial */
            const char *msg = (const char *)(uintptr_t)a0;
            if (msg) { serial_write("[app] "); serial_write(msg); serial_write("\r\n"); }
            regs[0] = 0;
            return;
        }
        case SYS_GETPID:        /* 53: get current process ID */
            regs[0] = (uint64_t)((g_current >= 0) ? g_procs[g_current].pid : 0);
            return;
        case SYS_GETUID:        /* 55: get current user ID */
            regs[0] = (uint64_t)((g_current >= 0) ? g_procs[g_current].uid : 0);
            return;
        case SYS_PROCESS_KILL: { /* 29: kill a process by PID. a0=pid */
            uint32_t target = (uint32_t)a0;
            int result = process_kill(target);
            regs[0] = (uint64_t)(int64_t)result;
            return;
        }
        case SYS_DESKTOP_STATUS: /* 43: top-bar status — not yet wired on arm64 */
        case SYS_MENU_DISPATCH:  /* 44: menu action delivery — not yet on arm64 */
        case SYS_WINDOW_SET_MENUBAR: /* 37: menu bar — not yet on arm64 */
            /* Silent stub: the topbar polls these every frame; returning -1
             * (it renders fine without the data) without logging avoids
             * flooding the serial journal once it runs as a persistent app. */
            regs[0] = (uint64_t)-1;
            return;
        case SYS_EVENT_POLL: {  /* 19: poll for window events */
            /* rdi = win_id, rsi = struct winsys_event* (out) */
            struct desktop_state *d = desktop_active();
            if (!d) { regs[0] = 0; return; }
            uint32_t pid = (g_current >= 0) ? g_procs[g_current].pid : 0;
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
    *hz = 160UL * 1024 * 1024;  /* main kmalloc heap → up to 0x4A800000 */
}

/* The compositor allocates window surface/content buffers from a separate GFX
 * heap (gfx_alloc, like x86). Carve it out of RAM after the main heap, still
 * inside the identity-mapped RAM block (0x40000000-0x7FFFFFFF, QEMU -m 512M
 * backs up to 0x5FFFFFFF). Without this, gfx_alloc returns NULL and window
 * creation (desktop_app_create_ex) fails — dock/topbar can't open windows. */
#define ARM64_GFX_BASE  0x4A800000UL   /* right after the 160 MB main heap */
#define ARM64_GFX_SIZE  (64UL * 1024 * 1024)
#define ARM64_IMG_BASE  0x4E800000UL   /* after gfx */
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
    serial_write("  halt          — stop the machine\r\n");
}

static void cmd_uname(void) {
    serial_write("VibeOS arm64 | QEMU virt | ARM generic timer | ");
    uint64_t midr = read_sysreg(midr_el1);
    /* implementer 0x61 = Apple */
    if (((midr >> 24) & 0xFF) == 0x61)
        serial_write("Apple Silicon (HVF)\r\n");
    else
        serial_write("QEMU Cortex-A72 (TCG)\r\n");
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
    }

    /* Make the just-written instructions visible to the I-cache */
    __asm__ volatile("dsb ish; ic ialluis; dsb ish; isb" ::: "memory");

    /* User stack top: VA 0x91000000 (PA 0x51000000). Args are delivered via
     * SYS_GETARG (g_spawn_arg), which crt0.c splits into argv — same as x86. */
    uint64_t user_sp = (USER_VA_BASE + 0x01000000ULL) & ~0xFULL;

    serial_write("[exec] "); serial_write(path);
    serial_write(" entry="); serial_write_hex_u64(eh->entry);
    serial_write("\r\n");

    uint64_t rc = arm64_enter_user(eh->entry, user_sp, 0, 0);

    serial_write("[exec] exit code = "); serial_write_hex_u64(rc); serial_write("\r\n");
    kfree(buf);
}

static void cmd_fb(void) {
    if (ramfb_init(800, 600) != 0) {
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
    draw_text(&fb, 250, 340, "running on Apple M-series via HVF", 0xff708090, 1);
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
        cmd_exec("/bin/desktop");
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

static void shell_loop(void) {
    static char line[256];
    int pos = 0;

    serial_write("VibeOS arm64 — type 'help' for commands\r\n");

    while (1) {
        serial_write("\r\narm64:");
        serial_write(g_cwd);
        serial_write("$ ");
        pos = 0;

        for (;;) {
            char c;
            /* Spin until a character arrives, polling timer + input each iteration */
            while (!uart_getc_nonblock(&c))
                virtio_input_poll();

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
        }
        run_command(line);
    }
}

/* ---- Kernel entry ----------------------------------------------------- */

static struct desktop_state *g_desktop = 0;
struct desktop_state *desktop_active(void) { return g_desktop; }

void kernel_main_arm64(void) {
    serial_init();

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

    /* ---- Desktop Compositor Test ---- */
    if (ramfb_init(800, 600) != 0) {
        serial_write("  ramfb init failed\r\n");
        shell_loop();
        return;
    }

    struct framebuffer fb;
    fb_init(&fb, (uintptr_t)ramfb_buffer(), ramfb_width(), ramfb_height(),
            ramfb_stride_px() * 4, 32);

    /* Allocate desktop state on the heap (it's large — contains window
     * storage, app slots, icon state, etc.) */
    g_desktop = (struct desktop_state *)
        kmalloc(sizeof(struct desktop_state));
    if (!g_desktop) { serial_write("  OOM for desktop\r\n"); shell_loop(); return; }
    memset(g_desktop, 0, sizeof(struct desktop_state));

    serial_write("[gui] initialising desktop compositor...\r\n");
    desktop_init(g_desktop, ramfb_width(), ramfb_height());

    /* Allocate a backbuffer for flicker-free rendering.
     * We render the scene offscreen, then blit to the visible framebuffer
     * in one operation — this eliminates the tearing/flickering that
     * direct-to-fb rendering causes. */
    size_t bb_pixels = (size_t)ramfb_width() * (size_t)ramfb_height();
    uint32_t *backbuf = (uint32_t *)kmalloc(bb_pixels * 4);
    struct framebuffer bb;
    if (backbuf) {
        fb_init(&bb, (uintptr_t)backbuf, ramfb_width(), ramfb_height(),
                ramfb_width() * 4, 32);
        serial_write("[gui] backbuffer allocated\r\n");
    } else {
        /* Fall back to direct rendering if OOM */
        bb = fb;
        serial_write("[gui] WARNING: no backbuffer (OOM) — may flicker\r\n");
    }
    desktop_init(g_desktop, ramfb_width(), ramfb_height());

    /* Spawn and run GUI apps sequentially.  Since all arm64 processes
     * share PA 0x50000000 (non-PIE, same VA), each spawn overwrites the
     * previous one.  We spawn-run-exit each app before starting the next.
     * After all apps have created their windows and exited, the compositor
     * renders their static content continuously. */
    serial_write("[gui] spawning wallpaper...\r\n");
    process_spawn_path("/bin/wallpaper", 0, 0, 0, 0);
    process_run_ready_slice();  /* run wallpaper, it exits */

    serial_write("[gui] spawning dock...\r\n");
    process_spawn_path("/bin/dock", 0, 0, 0, 0);
    process_run_ready_slice();  /* run dock, it creates window + exits */

    serial_write("[gui] spawning topbar...\r\n");
    process_spawn_path("/bin/topbar", 0, 0, 0, 0);
    process_run_ready_slice();  /* run topbar, it creates window + exits */

    serial_write("[gui] entering render loop\r\n");
    for (;;) {
        /* Feed mouse input to the compositor before rendering */
        struct mouse_state mouse;
        struct keyboard_state keyboard;
        memset(&mouse, 0, sizeof(mouse));
        memset(&keyboard, 0, sizeof(keyboard));
        mouse.x = g_mouse_x * (int)ramfb_width() / 32767;
        mouse.y = g_mouse_y * (int)ramfb_height() / 32767;
        mouse.buttons = (uint8_t)g_mouse_buttons;
        mouse.moved = (uint8_t)g_mouse_moved;
        static int prev_buttons = 0;
        if ((g_mouse_buttons & 1) && !(prev_buttons & 1))
            mouse.left_pressed = 1;
        if (!(g_mouse_buttons & 1) && (prev_buttons & 1))
            mouse.left_released = 1;
        prev_buttons = g_mouse_buttons;

        desktop_handle_input(g_desktop, &mouse, &keyboard);

        /* Render scene offscreen first (backbuffer), then blit to the
         * visible framebuffer in one shot — no tearing/flickering. */
        desktop_render(g_desktop, &bb);
        fb_blit(&fb, &bb);

        /* Draw cursor directly on the visible framebuffer so it
         * doesn't get smeared by the backbuffer copy. */
        desktop_draw_cursor_overlay(g_desktop, &fb);

        /* Run one user process slice, then poll input */
        process_run_ready_slice();
        virtio_input_poll();
    }
}
