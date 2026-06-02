#include "alloc.h"
#include "bga.h"
#include "ext2_fs.h"
#include "framebuffer.h"
#include "ide.h"
#include "input.h"
#include "interrupts.h"
#include "journal.h"
#include "multiboot2.h"
#include "net.h"
#include "paging.h"
#include "process.h"
#include "ramdisk.h"
#include "ramdisk_demo.h"
#include "serial.h"
#include "string.h"
#include "timer.h"
#include "tty.h"
#include "vfs.h"
#include "vga_text.h"
#include "window.h"

static struct framebuffer g_framebuffer;
static struct framebuffer g_backbuffer;
static struct desktop_state g_desktop;
static int g_wm_active = 0;
static uint32_t g_shell_dock_pid = 0;
static uint64_t g_shell_dock_next_launch_tick = 0;
static uint32_t g_shell_topbar_pid = 0;
static uint64_t g_shell_topbar_next_launch_tick = 0;
static uint8_t g_desktop_scene_started = 0;

/* Exposed to the syscall layer so userspace GUI apps can reach the compositor. */
struct desktop_state *desktop_active(void) {
    return g_wm_active ? &g_desktop : 0;
}
static struct tty g_cli_tty;
static uint32_t g_backbuffer_storage[1920u * 1080u];
extern uint8_t __kernel_end[];

static void halt_forever(void);

static uintptr_t align_up_uintptr(uintptr_t value, uintptr_t alignment) {
    return (value + alignment - 1u) & ~(alignment - 1u);
}

static void kernel_init_heap_from_bootinfo(uintptr_t mbi_addr) {
  struct boot_memory_info mem;
  uintptr_t heap_start = align_up_uintptr((uintptr_t)__kernel_end, 0x200000u);
  uint64_t heap_end;
  uint64_t heap_size;
  const uint64_t top_reserve = 16ull * 1024ull * 1024ull;
  const uint64_t min_heap = 32ull * 1024ull * 1024ull;

  serial_write("VIBEOS: kernel_end=");
  serial_write_hex_u64((uint64_t)(uintptr_t)__kernel_end);
  serial_write("\n");

  if (!multiboot2_memory_info(mbi_addr, heap_start, &mem)) {
    serial_write("VIBEOS: no multiboot memory map; using conservative heap fallback\n");
    kmalloc_init(heap_start, 32 * 1024 * 1024);
    gfx_heap_init(heap_start + 32 * 1024 * 1024, 12 * 1024 * 1024);
    image_heap_init(heap_start + 44 * 1024 * 1024, 20 * 1024 * 1024);
    kmalloc_set_physical_total(heap_start + 64 * 1024 * 1024);
    return;
  }

  heap_start = align_up_uintptr((uintptr_t)mem.heap_region_base, 0x200000u);
  heap_end = mem.heap_region_end > top_reserve ? mem.heap_region_end - top_reserve : mem.heap_region_end;
  heap_end &= ~0x1fffffull;

  if (heap_end <= heap_start || heap_end - heap_start < min_heap) {
    serial_write("VIBEOS: detected RAM too small for dynamic heap\n");
    halt_forever();
  }

  heap_size = heap_end - heap_start;

  /* Carve two separate, physically-disjoint heaps off the top of RAM:
   *   - the GFX heap for window/compositor surfaces, and
   *   - the IMAGE heap for process images,
   * so that neither can ever share physical memory with the other or with a
   * main-heap allocation (ELF read buffers, sbrk growth). This removes the
   * whole class of "allocation lands on a live image" corruption by
   * construction. Each is taken only if the main heap stays above the minimum. */
  {
    uint64_t gfx_size = heap_size / 6;
    uint64_t img_size = heap_size / 4;
    if (gfx_size > 64ull * 1024ull * 1024ull) gfx_size = 64ull * 1024ull * 1024ull;
    if (img_size > 96ull * 1024ull * 1024ull) img_size = 96ull * 1024ull * 1024ull;
    gfx_size &= ~0x1fffffull;                 /* 2 MB aligned */
    img_size &= ~0x1fffffull;
    if (gfx_size >= 8ull * 1024ull * 1024ull &&
        heap_size - gfx_size - img_size >= min_heap) {
      uint64_t gfx_base = heap_end - gfx_size;
      heap_size -= gfx_size;
      heap_end = gfx_base;
      gfx_heap_init((uintptr_t)gfx_base, (size_t)gfx_size);
      serial_write("VIBEOS: gfx heap base=");
      serial_write_hex_u64(gfx_base);
      serial_write(" size=");
      serial_write_hex_u64(gfx_size);
      serial_write("\n");
    }
    if (img_size >= 8ull * 1024ull * 1024ull &&
        heap_size - img_size >= min_heap) {
      uint64_t img_base = heap_end - img_size;
      heap_size -= img_size;
      heap_end = img_base;
      image_heap_init((uintptr_t)img_base, (size_t)img_size);
      serial_write("VIBEOS: image heap base=");
      serial_write_hex_u64(img_base);
      serial_write(" size=");
      serial_write_hex_u64(img_size);
      serial_write("\n");
    }
  }

  serial_write("VIBEOS: detected available RAM bytes=");
  serial_write_hex_u64(mem.available_bytes);
  serial_write(" heap_start=");
  serial_write_hex_u64((uint64_t)heap_start);
  serial_write(" heap_size=");
  serial_write_hex_u64(heap_size);
  serial_write("\n");
  kmalloc_init(heap_start, (size_t)heap_size);
  kmalloc_set_physical_total((size_t)mem.available_bytes);
}

static int rects_intersect(const struct rect *a, const struct rect *b) {
    return a->x < b->x + b->width &&
           a->x + a->width > b->x &&
           a->y < b->y + b->height &&
           a->y + a->height > b->y;
}

/* Smallest rectangle covering both a and b. */
static struct rect rect_union(const struct rect *a, const struct rect *b) {
    int x0 = a->x < b->x ? a->x : b->x;
    int y0 = a->y < b->y ? a->y : b->y;
    int ax1 = a->x + a->width;
    int ay1 = a->y + a->height;
    int bx1 = b->x + b->width;
    int by1 = b->y + b->height;
    int x1 = ax1 > bx1 ? ax1 : bx1;
    int y1 = ay1 > by1 ? ay1 : by1;
    struct rect r;
    r.x = x0;
    r.y = y0;
    r.width = x1 - x0;
    r.height = y1 - y0;
    return r;
}

static void halt_forever(void) {
    for (;;) {
        __asm__ volatile("hlt");
    }
}

static void cli_write_kernel_text(const char *text) {
    if (text == 0) {
        return;
    }

    (void)TTY_FD_OPS.write(&g_cli_tty, text, string_length(text));
}

/* Desired desktop resolution; changeable at runtime via the `display` tool.
 * Clamped to the statically-allocated backbuffer max (1920x1080). */
static uint32_t g_req_width = 1920u;
static uint32_t g_req_height = 1080u;
static volatile int g_resolution_change_requested = 0;

void kernel_cxx_init(void);
int kernel_cxx_smoke_value(void);

uint32_t kernel_current_resolution(void) {
    return (g_req_width << 16) | (g_req_height & 0xffffu);
}

void kernel_request_resolution(uint32_t width, uint32_t height) {
    if (width < 640u) width = 640u;
    if (height < 480u) height = 480u;
    if (width > DESKTOP_MAX_WIDTH) width = DESKTOP_MAX_WIDTH;
    if (height > DESKTOP_MAX_HEIGHT) height = DESKTOP_MAX_HEIGHT;
    g_req_width = width & ~7u; /* BGA wants width a multiple of 8 */
    g_req_height = height;
    g_resolution_change_requested = 1;
}

static int start_window_manager(int *presented_cursor_x, int *presented_cursor_y) {
    struct boot_framebuffer graphics_fb;

    serial_write("VIBEOS: windowmgr requested\n");
    if (!bga_init_framebuffer(&graphics_fb, g_req_width, g_req_height, BGA_DEFAULT_BPP)) {
        serial_write("VIBEOS: windowmgr graphics init failed\n");
        cli_write_kernel_text("windowmgr: graphics init failed\n");
        vga_text_render_tty(&g_cli_tty);
        return 0;
    }

    serial_write("VIBEOS: windowmgr fb addr=");
    serial_write_hex_u64((uint64_t)graphics_fb.address);
    serial_write("\n");
    paging_remap_framebuffer(graphics_fb.address, (size_t)graphics_fb.pitch * (size_t)graphics_fb.height);
    serial_write("VIBEOS: windowmgr fb remapped\n");
    fb_init(&g_framebuffer, graphics_fb.address, graphics_fb.width, graphics_fb.height, graphics_fb.pitch, graphics_fb.bpp);
    fb_init(&g_backbuffer, (uintptr_t)g_backbuffer_storage, graphics_fb.width, graphics_fb.height, graphics_fb.width * 4u, 32u);
    input_init(graphics_fb.width, graphics_fb.height);
    desktop_init(&g_desktop, graphics_fb.width, graphics_fb.height);
    desktop_render(&g_desktop, &g_backbuffer);
    serial_write("VIBEOS: windowmgr scene rendered\n");
    fb_blit(&g_framebuffer, &g_backbuffer);
    desktop_draw_cursor_overlay(&g_desktop, &g_framebuffer);
    *presented_cursor_x = g_desktop.mouse_x;
    *presented_cursor_y = g_desktop.mouse_y;
    serial_write("VIBEOS: desktop ready\n");
    return 1;
}

static void ensure_shell_dock_running(void) {
    uint64_t now = timer_tick_count();

    if (!g_wm_active) {
        return;
    }
    if (desktop_shell_dock_active(&g_desktop)) {
        return;
    }
    if (g_shell_dock_pid != 0 && process_pid_alive(g_shell_dock_pid)) {
        return;
    }
    if (now < g_shell_dock_next_launch_tick) {
        return;
    }

    g_shell_dock_pid = (uint32_t)process_spawn_path("/bin/dock", 0, 0);
    g_shell_dock_next_launch_tick = now + timer_frequency_hz();
}

/* Keep the top bar app (logo, and later menus/clock) alive, like the dock. */
static void ensure_shell_topbar_running(void) {
    uint64_t now = timer_tick_count();

    if (!g_wm_active) {
        return;
    }
    if (g_shell_topbar_pid != 0 && process_pid_alive(g_shell_topbar_pid)) {
        return;
    }
    if (now < g_shell_topbar_next_launch_tick) {
        return;
    }

    g_shell_topbar_pid = (uint32_t)process_spawn_path("/bin/topbar", 0, 0);
    g_shell_topbar_next_launch_tick = now + timer_frequency_hz();
}

static void start_desktop_scene_apps(void) {
    /* On desktop start, only set the wallpaper. The dock is brought up by
     * ensure_shell_dock_running(); user apps (browser, task manager, …) are
     * launched on demand from the dock, not auto-started with the OS. */
    if (g_desktop_scene_started || !g_wm_active) {
        return;
    }
    (void)process_spawn_path("/bin/wallpaper", 0, 0);
    g_desktop_scene_started = 1;
}

/* IDE disk wrapper functions for ramdisk interface */
static struct block_device *g_ide_device = NULL;

static int ide_disk_read(void *ctx, uint64_t block_num, void *buffer, size_t count) {
    struct block_device *bdev = (struct block_device *)ctx;
    //serial_write("KERNEL: ide_disk_read called, bdev=");
    //serial_write_hex_u64((uint64_t)bdev);
    //serial_write(" read=");
    //serial_write_hex_u64((uint64_t)bdev->read);
    //serial_write("\n");
    if (!bdev || !bdev->read) return -1;
    return bdev->read(bdev, block_num, buffer, count);
}

static int ide_disk_write(void *ctx, uint64_t block_num, const void *buffer, size_t count) {
    struct block_device *bdev = (struct block_device *)ctx;
    if (!bdev || !bdev->write) return -1;
    return bdev->write(bdev, block_num, buffer, count);
}

void kernel_main(uint32_t boot_magic, uintptr_t mbi_addr) {
  int presented_cursor_x;
  int presented_cursor_y;
  uint32_t seen_cli_revision = 0;
  int window_manager_active = 0;

  serial_init();
  serial_write("VIBEOS: entered kernel_main\n");
  serial_write("VIBEOS: boot magic=");
  serial_write_hex_u64(boot_magic);
  serial_write("\n");
  serial_write("VIBEOS: multiboot info=");
  serial_write_hex_u64((uint64_t)mbi_addr);
  serial_write("\n");

  if (boot_magic != MULTIBOOT2_BOOTLOADER_MAGIC) {
    serial_write("VIBEOS: invalid multiboot magic\n");
    vga_text_write_message("VIBEOS: INVALID MULTIBOOT2 MAGIC", 0x4fu);
    halt_forever();
  }

  /* Initialize kernel heap BEFORE any filesystem operations */
  serial_write("VIBEOS: Initializing kernel heap...\n");
  kernel_init_heap_from_bootinfo(mbi_addr);
  serial_write("VIBEOS: Kernel heap initialized\n");
  kernel_cxx_init();
  if (kernel_cxx_smoke_value() != 42) {
    serial_write("VIBEOS: kernel C++ runtime self-test failed\n");
    halt_forever();
  }
  serial_write("VIBEOS: Kernel C++ runtime initialized\n");

  /* Initialize IDE and mount disk filesystem */
  serial_write("VIBEOS: Initializing IDE disk...\n");
  {
    static struct block_device ide_bdev;
    static struct ramdisk_device disk_device;
    static struct ext2_filesystem fs;
    int ide_ready = 0;
    
    if (ide_init() == 0) {
      serial_write("VIBEOS: IDE initialized\n");
      if (ide_block_device_init(&ide_bdev) == 0) {
        serial_write("VIBEOS: Block device ready\n");
        
        /* Create wrapper to use IDE as ramdisk device */
        disk_device.data = NULL; /* Not used for disk */
        disk_device.size = ide_bdev.block_count * ide_bdev.block_size;
        disk_device.block_size = ide_bdev.block_size;
        disk_device.block_count = ide_bdev.block_count;
        disk_device.read_fn = ide_disk_read;
        disk_device.write_fn = ide_disk_write;
        disk_device.io_context = &ide_bdev;
        g_ide_device = &ide_bdev;

	serial_write("VIBEOS: Calling ext2_mount...\n");
	/* Try to mount ext2 from disk */
	if (ext2_mount(&fs, &disk_device) == 0) {
          serial_write("VIBEOS: ext2 filesystem mounted from disk!\n");
          vfs_set_ext2(&fs);
          ide_ready = 1;
        } else {
          serial_write("VIBEOS: Failed to mount disk (not formatted?)\n");
        }
      }
    }
    
    if (!ide_ready) {
      serial_write("VIBEOS: Falling back to RAM disk\n");
      ramdisk_demo_init();
      serial_write("VIBEOS: RAM disk ready for filesystem\n");
    }
  }

  vga_text_clear(0x1fu);
  interrupts_init();
  journal_init();
  journal_log(JOURNAL_INFO, 0, "VibeOS kernel boot");
  journal_log(JOURNAL_APP, 0, "kernel inputdiag wheel-restored v4");
  process_init();

  /* Bring up the network card (best effort; networking stays optional). */
  serial_write("VIBEOS: Initializing network...\n");
  net_init();

  input_init(VGA_TEXT_COLUMNS, VGA_TEXT_ROWS);
  tty_init(&g_cli_tty);
  
  serial_write("VIBEOS: Attempting to spawn /bin/sh...\n");
  {
    int result = process_spawn_path("/bin/sh", &TTY_FD_OPS, &g_cli_tty);
    serial_write("VIBEOS: process_spawn_path returned ");
    serial_write_hex_u64(result);
    serial_write("\n");

    if (result <= 0) {
      serial_write("VIBEOS: process_spawn_path failed\n");
      vga_text_write_message("VIBEOS: FAILED TO START CLI SHELL", 0x4fu);
      halt_forever();
    }
  }
    vga_text_render_tty(&g_cli_tty);
    seen_cli_revision = g_cli_tty.revision;
    presented_cursor_x = 0;
    presented_cursor_y = 0;
    serial_write("VIBEOS: cli ready\n");

    for (;;) {
        struct mouse_state mouse;
        struct keyboard_state keyboard;
        struct rect dirty_rect;
        struct rect old_cursor_rect;
        struct rect current_cursor_rect;
        int cursor_moved;
        int had_dirty = 0;
        int run_result;

        input_poll(&mouse, &keyboard);
        net_poll();

        /* Persist the journal to /journal.log when an important event (WARN/
         * ERROR/FAULT) flagged it. Runs here in normal context — never from the
         * fault handler — so crash logs survive a reboot without risky I/O in
         * the trap path. */
        if (journal_persist_pending()) {
            struct ext2_filesystem *jfs = vfs_get_ext2();
            if (jfs != 0) {
                static char jbuf[24576];
                size_t jlen = journal_format_all(jbuf, sizeof jbuf);
                uint32_t jino = ext2_lookup_inode(jfs, "/journal.log");
                if (jino == 0) jino = ext2_create(jfs, "/journal.log", 0100644u);
                if (jino != 0) {
                    ext2_truncate(jfs, "/journal.log");
                    ext2_write(jfs, jino, 0, jlen, jbuf);
                }
            }
            journal_persist_clear();
        }

        if (!window_manager_active) {
            (void)tty_handle_keyboard(&g_cli_tty, &keyboard);
            run_result = process_run_ready_slice();
            if (g_cli_tty.revision != seen_cli_revision) {
                vga_text_render_tty(&g_cli_tty);
                seen_cli_revision = g_cli_tty.revision;
            }
            if (process_take_window_manager_request()) {
                window_manager_active = start_window_manager(&presented_cursor_x, &presented_cursor_y);
                g_wm_active = window_manager_active;
                if (window_manager_active) {
                    struct rect initial_rect;
                    (void)desktop_take_dirty_rect(&g_desktop, &initial_rect);
                    ensure_shell_dock_running();
                    ensure_shell_topbar_running();
                    start_desktop_scene_apps();
                }
            }
        } else {
            /* Apply a runtime resolution change by re-initialising the WM. */
            if (g_resolution_change_requested) {
                g_resolution_change_requested = 0;
                if (g_shell_dock_pid != 0 && process_pid_alive(g_shell_dock_pid)) {
                    (void)process_kill(g_shell_dock_pid);
                }
                if (g_shell_topbar_pid != 0 && process_pid_alive(g_shell_topbar_pid)) {
                    (void)process_kill(g_shell_topbar_pid);
                }
                g_shell_dock_pid = 0;
                g_shell_topbar_pid = 0;
                g_desktop_scene_started = 0;
                if (start_window_manager(&presented_cursor_x, &presented_cursor_y)) {
                    struct rect r;
                    (void)desktop_take_dirty_rect(&g_desktop, &r);
                    ensure_shell_dock_running();
                    ensure_shell_topbar_running();
                    start_desktop_scene_apps();
                }
                continue;
            }
            desktop_handle_input(&g_desktop, &mouse, &keyboard);
            cursor_moved = presented_cursor_x != g_desktop.mouse_x || presented_cursor_y != g_desktop.mouse_y;
            run_result = process_run_ready_slice();
            desktop_poll_apps(&g_desktop);
            ensure_shell_dock_running();
            ensure_shell_topbar_running();
            start_desktop_scene_apps();

            /* Flicker-free presentation: the cursor is composited INTO the
             * back buffer and the whole frame is pushed to the visible
             * framebuffer in a single contiguous blit per frame. The back
             * buffer is kept cursor-free between frames so scene re-renders
             * never include a stale cursor. */
            {
                struct rect present;
                int have_present = 0;

                if (desktop_take_dirty_rect(&g_desktop, &dirty_rect)) {
                    desktop_render_rect(&g_desktop, &g_backbuffer, &dirty_rect);
                    present = dirty_rect;
                    have_present = 1;
                    had_dirty = 1;
                }

                desktop_cursor_rect_at(&g_desktop, presented_cursor_x, presented_cursor_y, &old_cursor_rect);
                desktop_cursor_rect_at(&g_desktop, g_desktop.mouse_x, g_desktop.mouse_y, &current_cursor_rect);

                if (cursor_moved) {
                    present = have_present ? rect_union(&present, &old_cursor_rect) : old_cursor_rect;
                    present = rect_union(&present, &current_cursor_rect);
                    have_present = 1;
                } else if (had_dirty && rects_intersect(&dirty_rect, &current_cursor_rect)) {
                    present = rect_union(&present, &current_cursor_rect);
                }

                if (have_present) {
                    /* Draw cursor into the back buffer, present the whole
                     * region at once, then erase the cursor from the back
                     * buffer by re-rendering the scene under it. */
                    desktop_draw_cursor_overlay(&g_desktop, &g_backbuffer);
                    fb_blit_rect(&g_framebuffer, &g_backbuffer, &present);
                    desktop_render_rect(&g_desktop, &g_backbuffer, &current_cursor_rect);

                    presented_cursor_x = g_desktop.mouse_x;
                    presented_cursor_y = g_desktop.mouse_y;
                }
            }
        }
        if (run_result == PROCESS_RUN_NONE) {
            __asm__ volatile("sti; hlt; cli");
        } else {
            /* Work is pending (a process is runnable or parked on blocking I/O).
             * Enable interrupts so the timer keeps advancing — blocking-I/O
             * timeouts are measured in real time even though the waiting process
             * is parked mid-syscall. The timer's kernel-mode path only does
             * bookkeeping (tick count + wakeups) and returns, so this is safe. */
            __asm__ volatile("sti; pause");
        }
    }
}
