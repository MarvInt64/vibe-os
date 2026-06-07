/* Wallpaper for arm64 — loads the real desktop image from disk.
 *
 * Reads /wallpapers/default.vwp (the SAME asset the x86 build ships, produced
 * by scripts/png_to_vwp.py) and hands its pixels to SYS_SET_WALLPAPER, which
 * nearest-neighbour-scales them to fill the screen.  No libimage / PNG decoder
 * needed — the .vwp container is just a 12-byte header + raw XRGB pixels.
 *
 * Uses raw syscalls and a static pixel buffer (no malloc): arm64 apps run on a
 * fixed BSS heap with no SBRK, and the image is larger than that arena.  If
 * the file is missing or malformed we fall back to a dark vertical gradient so
 * the desktop never comes up with a bare backdrop.
 *
 * vwp format:  "VWP1" | u32 width LE | u32 height LE | width*height XRGB LE.
 */
#include <sys/syscall.h>
#include <stdint.h>

/* Capacity for the shipped default.png (1280x853); recompile if it grows. */
#define WP_MAX_W 1280
#define WP_MAX_H 853
static uint32_t g_pixels[WP_MAX_W * WP_MAX_H];

/* Raw syscall trampolines (this app deliberately does not link libc). */
static inline long sc1(long n, uint64_t a0) {
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = (long)a0;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory");
    return x0;
}
static inline long sc3(long n, uint64_t a0, uint64_t a1, uint64_t a2) {
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = (long)a0;
    register long x1 __asm__("x1") = (long)a1;
    register long x2 __asm__("x2") = (long)a2;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8),"r"(x1),"r"(x2) : "memory");
    return x0;
}

static uint32_t rd_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Read exactly n bytes from fd into dst (SYS_READ may return short). */
static int read_exact(int fd, uint8_t *dst, long n) {
    long off = 0;
    while (off < n) {
        long got = sc3(SYS_READ, (uint64_t)fd,
                       (uint64_t)(uintptr_t)(dst + off), (uint64_t)(n - off));
        if (got <= 0) return -1;
        off += got;
    }
    return 0;
}

static void fill_gradient(int w, int h) {
    for (int y = 0; y < h; y++) {
        int r = 0x0a + (0x03 - 0x0a) * y / (h - 1);
        int g = 0x16 + (0x05 - 0x16) * y / (h - 1);
        int b = 0x28 + (0x08 - 0x28) * y / (h - 1);
        uint32_t c = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        for (int x = 0; x < w; x++) g_pixels[y * w + x] = c;
    }
}

void _start(void) {
    int sent = 0;

    int fd = (int)sc1(SYS_OPEN, (uint64_t)(uintptr_t)"/wallpapers/default.vwp");
    if (fd >= 0) {
        uint8_t hdr[12];
        if (read_exact(fd, hdr, 12) == 0 &&
            hdr[0] == 'V' && hdr[1] == 'W' && hdr[2] == 'P' && hdr[3] == '1') {
            uint32_t w = rd_le32(hdr + 4);
            uint32_t h = rd_le32(hdr + 8);
            if (w > 0 && h > 0 && w <= WP_MAX_W && h <= WP_MAX_H) {
                long bytes = (long)w * (long)h * 4;
                if (read_exact(fd, (uint8_t *)g_pixels, bytes) == 0) {
                    sc1(SYS_CLOSE, (uint64_t)fd);
                    sc3(SYS_SET_WALLPAPER, (uint64_t)(uintptr_t)g_pixels, w, h);
                    sent = 1;
                }
            }
        }
        if (!sent) sc1(SYS_CLOSE, (uint64_t)fd);
    }

    /* Fallback: dark gradient so the desktop never shows a bare backdrop. */
    if (!sent) {
        fill_gradient(64, 64);
        sc3(SYS_SET_WALLPAPER, (uint64_t)(uintptr_t)g_pixels, 64, 64);
    }

    sc1(SYS_EXIT, 0);
    for (;;) {}
}
