/* VibeOS — userspace desktop demo (arch-neutral).
 *
 * Draws a full desktop scene — wallpaper gradient, top bar, a dock, and a
 * couple of windows with title bars and text — entirely from userspace, using
 * the SHARED kernel renderer (kernel/src/render.c) compiled as a userspace
 * library. The same binary source runs on x86_64 and arm64; the kernel only
 * has to answer SYS_FB_INFO and SYS_INPUT_POLL.
 *
 * After the initial paint the program enters an event loop: polls the
 * mouse/tablet via SYS_INPUT_POLL and draws a cursor.  The cursor is
 * drawn by saving/restoring a small patch of the wallpaper, so there is
 * no framebuffer damage (no trail).
 */
#include <stdio.h>
#include <sys/fb.h>
#include <sys/input.h>
#include "framebuffer.h"
#include "render.h"

/* Cursor size */
#define CUR_W 16
#define CUR_H 16

/* ---- Save / restore a rectangular patch of the framebuffer ------------ */
static void save_rect(struct framebuffer *fb, int x, int y, int w, int h,
                      uint32_t *save) {
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            save[row * w + col] = fb->base[(y + row) * (fb->pitch / 4) + (x + col)];
        }
    }
}

static void restore_rect(struct framebuffer *fb, int x, int y, int w, int h,
                         uint32_t *save) {
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            fb->base[(y + row) * (fb->pitch / 4) + (x + col)] = save[row * w + col];
        }
    }
}

/* ---- Draw a simple arrow cursor (16x16) ------------------------------- */
static void draw_cursor(struct framebuffer *fb, int cx, int cy) {
    /* Clamp to screen */
    if (cx < 0) cx = 0; if (cy < 0) cy = 0;
    if (cx + CUR_W > (int)fb->width)  cx = (int)fb->width  - CUR_W;
    if (cy + CUR_H > (int)fb->height) cy = (int)fb->height - CUR_H;

    /* Arrow shape (16x16, 1 = white pixel) */
    static const uint8_t arrow[16][16] = {
        {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
        {1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
        {1,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0},
        {1,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0},
        {1,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0},
        {1,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0},
        {1,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0},
        {1,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0},
        {1,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0},
        {1,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0},
        {1,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0},
        {1,1,1,0,0,0,0,0,0,1,1,1,0,0,0,0},
        {0,1,1,1,1,1,1,0,1,0,0,0,0,0,0,0},
        {0,0,1,1,1,1,1,1,0,0,0,0,0,0,0,0},
        {0,0,0,1,1,0,0,0,0,0,0,0,0,0,0,0},
        {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    };

    for (int row = 0; row < CUR_H; row++) {
        for (int col = 0; col < CUR_W; col++) {
            if (arrow[row][col]) {
                fb->base[(cy + row) * (fb->pitch / 4) + (cx + col)] = 0xffffffff;
            }
        }
    }
}

/* ---- Window drawing helper -------------------------------------------- */
static void draw_window(struct framebuffer *fb, int x, int y, int w, int h,
                        const char *title) {
    draw_soft_shadow(fb, x, y, w, h, 10, 18, 0x40000000);
    draw_rounded_panel(fb, x, y, w, h, 10,
                       0xff2b3142, 0xff20242f, 0xff4a5570, 0xff5b6a90);
    fb_fill_rect(fb, x + 2, y + 2, w - 4, 30, 0xff333b50);
    draw_text(fb, x + 14, y + 9, title, 0xffe0e8f0, 1);
    fb_fill_rect(fb, x + w - 18, y + 11, 8, 8, 0xffe06060);
    fb_fill_rect(fb, x + w - 34, y + 11, 8, 8, 0xffe0c060);
    fb_fill_rect(fb, x + w - 50, y + 11, 8, 8, 0xff60c070);
}

/* ---- Main — paint scene then handle input ----------------------------- */
int main(void) {
    struct vos_fb_info info;
    if (vos_fb_info(&info) != 0) {
        printf("desktop: no framebuffer\n");
        return 1;
    }

    struct framebuffer fb;
    fb_init(&fb, (uintptr_t)info.addr, info.width, info.height, info.stride, 32);

    int W = (int)info.width, H = (int)info.height;

    /* Wallpaper */
    draw_gradient_background(&fb, 0xff1b2436, 0xff0d1018);

    /* Top bar */
    fb_fill_rect(&fb, 0, 0, W, 28, 0xff161b28);
    draw_text(&fb, 12, 7, "VibeOS", 0xff9fc0ff, 1);
    draw_text(&fb, W - 150, 7, "arm64 . HVF . 800x600", 0xff708090, 1);

    /* Two windows */
    draw_window(&fb, 80, 90, 320, 200, "Files");
    draw_text(&fb, 100, 140, "one kernel,", 0xffb0c0d0, 2);
    draw_text(&fb, 100, 175, "two architectures", 0xffb0c0d0, 2);

    draw_window(&fb, 430, 150, 290, 220, "Terminal");
    draw_text(&fb, 448, 195, "vibeos$ uname", 0xff80e090, 1);
    draw_text(&fb, 448, 215, "VibeOS arm64", 0xffc0c0c0, 1);
    draw_text(&fb, 448, 235, "vibeos$ _", 0xff80e090, 1);

    /* Dock */
    int dockw = 320, dockx = (W - dockw) / 2, docky = H - 60;
    draw_rounded_panel(&fb, dockx, docky, dockw, 48, 12,
                       0x80303a52, 0x80222838, 0xff4a5570, 0xff5b6a90);
    for (int i = 0; i < 6; i++) {
        uint32_t cols[6] = { 0xff5b8def, 0xff5fbf6f, 0xffe0a040,
                             0xffe06070, 0xffb070e0, 0xff50c0c0 };
        fb_fill_rect(&fb, dockx + 16 + i * 50, docky + 9, 32, 30, cols[i]);
    }

    printf("desktop: drawn via shared renderer\n");

    /* ---- Event loop: poll mouse, draw cursor ---- */
    struct vos_input_state st;
    int last_x = -100, last_y = -100;
    uint32_t save_buf[CUR_W * CUR_H];
    int saved = 0;

    printf("desktop: entering interactive mode (move mouse in QEMU window)\n");

    for (;;) {
        if (vos_input_poll(&st) != 0) {
            /* No input device — just spin */
            continue;
        }
        if (!st.moved && st.x == last_x && st.y == last_y)
            continue;

        /* Restore previous cursor position (wallpaper underneath) */
        int old_cx = last_x, old_cy = last_y;
        if (saved) {
            restore_rect(&fb, old_cx, old_cy, CUR_W, CUR_H, save_buf);
        }

        /* Save new cursor position */
        int cx = st.x, cy = st.y;
        /* Clamp */
        if (cx < 0) cx = 0; if (cy < 0) cy = 0;
        if (cx + CUR_W > W) cx = W - CUR_W;
        if (cy + CUR_H > H) cy = H - CUR_H;

        save_rect(&fb, cx, cy, CUR_W, CUR_H, save_buf);
        saved = 1;

        /* Draw cursor */
        draw_cursor(&fb, cx, cy);

        last_x = cx;
        last_y = cy;
    }

    return 0;
}
