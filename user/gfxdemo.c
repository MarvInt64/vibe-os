/* VibeOS — userspace graphics demo (arch-neutral).
 *
 * Asks the kernel for a linear framebuffer via SYS_FB_INFO and draws a test
 * scene directly into it: gradient background, color bars, a moving-looking
 * checkerboard, and a centered panel. Runs identically on x86_64 and arm64;
 * the kernel side supplies the framebuffer.
 *
 *   exec /bin/gfxdemo        (arm64; on x86 it would need a window-fb backend)
 */
#include <stdio.h>
#include <sys/fb.h>

static inline uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    return 0xff000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

int main(void) {
    struct vos_fb_info fb;
    if (vos_fb_info(&fb) != 0) {
        printf("gfxdemo: no framebuffer available\n");
        return 1;
    }
    printf("gfxdemo: framebuffer %ux%u stride=%u @ %p\n",
           fb.width, fb.height, fb.stride, (void *)(uintptr_t)fb.addr);

    uint32_t *px = (uint32_t *)(uintptr_t)fb.addr;
    uint32_t sp = fb.stride / 4;   /* stride in pixels */
    uint32_t W = fb.width, H = fb.height;

    /* Vertical gradient background (dark blue → near black). */
    for (uint32_t y = 0; y < H; y++) {
        uint8_t b = (uint8_t)(40 - (40 * y) / H);
        uint32_t c = rgb(8, 12, (uint8_t)(b + 16));
        for (uint32_t x = 0; x < W; x++) px[y * sp + x] = c;
    }

    /* Color bars across the top. */
    uint32_t bars[6] = { rgb(220,60,60), rgb(220,150,40), rgb(220,220,60),
                         rgb(60,200,90), rgb(60,120,220), rgb(150,80,210) };
    uint32_t bw = W / 6;
    for (int i = 0; i < 6; i++)
        for (uint32_t y = 20; y < 80; y++)
            for (uint32_t x = i * bw; x < (i + 1) * bw && x < W; x++)
                px[y * sp + x] = bars[i];

    /* Checkerboard band. */
    for (uint32_t y = 110; y < 210; y++)
        for (uint32_t x = 0; x < W; x++) {
            int c = ((x >> 4) ^ (y >> 4)) & 1;
            px[y * sp + x] = c ? rgb(60,60,70) : rgb(30,30,38);
        }

    /* Centered panel with a border. */
    uint32_t pw = 300, ph = 180;
    uint32_t x0 = (W - pw) / 2, y0 = 260;
    for (uint32_t y = y0; y < y0 + ph; y++)
        for (uint32_t x = x0; x < x0 + pw; x++) {
            int edge = (x < x0+3 || x >= x0+pw-3 || y < y0+3 || y >= y0+ph-3);
            px[y * sp + x] = edge ? rgb(120,180,255) : rgb(24,28,40);
        }

    printf("gfxdemo: scene drawn — check the display\n");
    return 0;
}
