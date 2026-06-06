/* Minimal wallpaper for arm64 — sets a dark gradient background.
 *
 * Avoids libimage dependency by generating a small gradient pixel buffer
 * in-process and sending it via SYS_SET_WALLPAPER.  The compositor
 * stretches this to fill the screen.
 */
#include <sys/syscall.h>
#include <stdint.h>

#define WP_W 64
#define WP_H 64

static uint32_t buf[WP_W * WP_H];

/* Our own sc3 since we don't link libc */
static inline long sc3(long n, uint64_t a0, uint64_t a1, uint64_t a2) {
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = (long)a0;
    register long x1 __asm__("x1") = (long)a1;
    register long x2 __asm__("x2") = (long)a2;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8),"r"(x1),"r"(x2) : "memory");
    return x0;
}

void _start(void) {
    /* Generate a dark navy-to-near-black vertical gradient */
    for (int y = 0; y < WP_H; y++) {
        /* Top: dark navy (0x0a1628), bottom: near black (0x030508) */
        int r_top = 0x0a, g_top = 0x16, b_top = 0x28;
        int r_bot = 0x03, g_bot = 0x05, b_bot = 0x08;
        int r = r_top + (r_bot - r_top) * y / (WP_H - 1);
        int g = g_top + (g_bot - g_top) * y / (WP_H - 1);
        int b = b_top + (b_bot - b_top) * y / (WP_H - 1);
        uint32_t color = 0xff000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        for (int x = 0; x < WP_W; x++) {
            buf[y * WP_W + x] = color;
        }
    }

    sc3(36 /* SYS_SET_WALLPAPER */,
        (uint64_t)(uintptr_t)buf,
        (uint64_t)WP_W,
        (uint64_t)WP_H);

    /* Exit */
    register long x8 __asm__("x8") = 4; /* SYS_EXIT */
    register long x0 __asm__("x0") = 0;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory");
}
