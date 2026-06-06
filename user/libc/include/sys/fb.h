/* VibeOS — userspace framebuffer access (arch-neutral).
 *
 * SYS_FB_INFO fills a vos_fb_info with a pointer to a linear 32-bit (XRGB8888)
 * framebuffer the program can draw into directly, plus its geometry. Returns 0
 * on success, <0 if no framebuffer is available.
 *
 * The same header/ABI works on both arches; only the kernel side differs.
 */
#ifndef VIBEOS_SYS_FB_H
#define VIBEOS_SYS_FB_H

#include <stdint.h>
#include <sys/syscall.h>

struct vos_fb_info {
    uint64_t addr;     /* framebuffer base, writable from the caller */
    uint32_t width;    /* pixels */
    uint32_t height;   /* pixels */
    uint32_t stride;   /* bytes per row */
    uint32_t bpp;      /* bits per pixel (32) */
};

static inline int vos_fb_info(struct vos_fb_info *out) {
    return (int)__sc1(SYS_FB_INFO, (uint64_t)(uintptr_t)out);
}

#endif
