/* wallpaper — decode an image file and hand its pixels to the kernel to use as
 * the desktop backdrop.
 *
 * Runs in userspace where the shared libimage decoder (image_decode) and the
 * float unit are available; the kernel itself has no image decoder. The decoded
 * XRGB pixels are sent over SYS_SET_WALLPAPER, which scales them to the screen.
 *
 * Path selection (the foundation for user-managed wallpapers):
 *   - the spawn argument, if given (e.g. `wallpaper /wallpapers/foo.png`), else
 *   - the built-in default /wallpapers/default.png.
 * Drop more images into /wallpapers/ and pass the path to switch. */
#include <vibeos.h>
#include <sys/syscall.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include "image.h"
#include "umalloc.h"

static uint32_t mix_rgb(uint32_t a, uint32_t b, unsigned step, unsigned total) {
    uint32_t ar = (a >> 16) & 0xffu, ag = (a >> 8) & 0xffu, ab = a & 0xffu;
    uint32_t br = (b >> 16) & 0xffu, bg = (b >> 8) & 0xffu, bb = b & 0xffu;
    return ((((ar * (total - step)) + (br * step)) / total) << 16) |
           ((((ag * (total - step)) + (bg * step)) / total) << 8) |
           (((ab * (total - step)) + (bb * step)) / total);
}

static unsigned noise2(int x, int y) {
    uint32_t n = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
    n = (n ^ (n >> 13)) * 1274126177u;
    return (n ^ (n >> 16)) & 0xffu;
}

static unsigned int *make_fallback_wallpaper(int *w, int *h) {
    const int width = 1024;
    const int height = 768;
    uint32_t *px = (uint32_t *)malloc((size_t)width * (size_t)height * sizeof(uint32_t));
    int x, y;

    if (!px) return 0;
    for (y = 0; y < height; ++y) {
        uint32_t top = mix_rgb(0x0013263bu, 0x00192844u, (unsigned)y, (unsigned)(height - 1));
        uint32_t bottom = mix_rgb(0x0006101cu, 0x00142636u, (unsigned)y, (unsigned)(height - 1));
        for (x = 0; x < width; ++x) {
            uint32_t c = mix_rgb(top, bottom, (unsigned)x, (unsigned)(width - 1));
            unsigned glow = 0;
            int dx = x - width / 2;
            int dy = y - height / 3;
            int dist = (dx * dx) / 6 + (dy * dy) / 3;
            if (dist < 36000) glow = (unsigned)(80 - dist / 450);
            c = mix_rgb(c, 0x00375f88u, glow, 255u);
            c = mix_rgb(c, 0x00ffffffu, noise2(x, y) & 7u, 255u);
            px[(size_t)y * (size_t)width + (size_t)x] = c;
        }
    }
    *w = width;
    *h = height;
    return px;
}

static uint32_t read_le32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static const uint32_t *vwp_pixels_inplace(unsigned char *data, int len, int *w, int *h) {
    uint32_t width, height;
    unsigned long count, bytes;

    if (!data || len < 12 || data[0] != 'V' || data[1] != 'W' ||
        data[2] != 'P' || data[3] != '1') {
        return 0;
    }
    width = read_le32(data + 4);
    height = read_le32(data + 8);
    if (width == 0 || height == 0 || width > 4096u || height > 4096u) {
        return 0;
    }
    count = (unsigned long)width * (unsigned long)height;
    bytes = count * sizeof(uint32_t);
    if (count / (unsigned long)width != (unsigned long)height ||
        bytes / sizeof(uint32_t) != count ||
        (unsigned long)len < 12ul + bytes) {
        return 0;
    }
    *w = (int)width;
    *h = (int)height;
    return (const uint32_t *)(const void *)(data + 12);
}

static void log_stage(const char *stage) {
    vos_log(VOS_LOG_APP, stage);
}

static int stat_path(const char *path, unsigned long *kind, unsigned long *size) {
    long ret;
    unsigned long out_kind = 0;
    unsigned long out_size = 0;

    __asm__ volatile(
        "int $0x80\n\t"
        "mov %%rdx, %1\n\t"
        "mov %%r8, %2\n\t"
        : "=a"(ret), "=m"(out_kind), "=m"(out_size)
        : "a"((unsigned long)SYS_STAT), "D"((unsigned long)(size_t)path)
        : "rcx", "r11", "rdx", "r8", "memory");
    if (ret < 0) {
        return (int)ret;
    }
    if (kind) *kind = out_kind;
    if (size) *size = out_size;
    return 0;
}

/* Read a known-size file into a heap buffer. The caller passes the exact size
 * from SYS_STAT, so we never need to grow the buffer mid-read. */
static int read_all_exact(const char *path, size_t cap, unsigned char **out_buf, int *out_len) {
    char msg[160];
    int fd;
    int total = 0;
    int n;
    unsigned char *buf;

    snprintf(msg, sizeof(msg), "wallpaper: read_all open %s cap=%lu", path, (unsigned long)cap);
    vos_log(VOS_LOG_APP, msg);
    fd = (int)__sc1(SYS_OPEN, (uint64_t)(size_t)path);
    snprintf(msg, sizeof(msg), "wallpaper: read_all open rc=%d", fd);
    vos_log(VOS_LOG_APP, msg);
    if (fd < 0) return -1;
    vos_log(VOS_LOG_APP, "wallpaper: read_all before malloc");
    buf = (unsigned char *)malloc((size_t)cap);
    if (!buf) {
        vos_log(VOS_LOG_APP, "wallpaper: read_all malloc failed");
        __sc1(SYS_CLOSE, (uint64_t)fd);
        return -1;
    }
    vos_log(VOS_LOG_APP, "wallpaper: read_all malloc ok");

    while ((size_t)total < cap) {
        size_t remain = cap - (size_t)total;
        snprintf(msg, sizeof(msg), "wallpaper: read_all before read off=%d cap=%lu", total, (unsigned long)cap);
        vos_log(VOS_LOG_APP, msg);
        n = (int)__sc3(SYS_READ, (uint64_t)fd, (uint64_t)(size_t)(buf + total),
                       (uint64_t)remain);
        snprintf(msg, sizeof(msg), "wallpaper: read_all read rc=%d", n);
        vos_log(VOS_LOG_APP, msg);
        if (n <= 0) break;
        total += n;
    }
    vos_log(VOS_LOG_APP, "wallpaper: read_all before close");
    __sc1(SYS_CLOSE, (uint64_t)fd);
    vos_log(VOS_LOG_APP, "wallpaper: read_all after close");

    if (total <= 0) { free(buf); return -1; }
    *out_buf = buf;
    *out_len = total;
    return 0;
}

int main(void) {
    char path[256];
    char raw_path[256];
    char msg[160];
    int alen;
    unsigned char *data = 0;
    int len = 0;
    unsigned int *px;
    int w = 0, h = 0;
    unsigned long kind = 0;
    unsigned long size = 0;

    log_stage("wallpaper: enter");
    path[0] = '\0';
    log_stage("wallpaper: before getarg");
    alen = vos_getarg(path, (int)sizeof(path));
    log_stage("wallpaper: after getarg");
    if (alen <= 0 || path[0] == '\0') {
        const char *def = "/wallpapers/default.png";
        int i = 0;
        for (; def[i] && i < (int)sizeof(path) - 1; ++i) path[i] = def[i];
        path[i] = '\0';
        log_stage("wallpaper: using default png path");
    } else {
        log_stage("wallpaper: using spawn arg path");
    }
    {
        const char *raw = "/wallpapers/default.vwp";
        int i = 0;
        for (; raw[i] && i < (int)sizeof(raw_path) - 1; ++i) raw_path[i] = raw[i];
        raw_path[i] = '\0';
    }

    log_stage("wallpaper: before vwp read");
    if (alen <= 0 && stat_path(raw_path, &kind, &size) == 0) {
        snprintf(msg, sizeof(msg), "wallpaper: vwp stat kind=%lu size=%lu", kind, size);
        vos_log(VOS_LOG_APP, msg);
    }
    if (alen <= 0 && size > 12ul && read_all_exact(raw_path, (size_t)size, &data, &len) == 0) {
        const uint32_t *vwp_px;
        log_stage("wallpaper: vwp read ok");
        /* VWP already stores XRGB pixels, so avoid a second multi-megabyte
         * allocation and let the kernel copy from the read buffer directly. */
        vwp_px = vwp_pixels_inplace(data, len, &w, &h);
        if (vwp_px && w > 0 && h > 0) {
            snprintf(msg, sizeof(msg), "wallpaper: decoded real vwp %dx%d heap=%lu/%lu",
                     w, h, (unsigned long)umalloc_used(), (unsigned long)umalloc_capacity());
            vos_log(VOS_LOG_APP, msg);
            log_stage("wallpaper: before set_wallpaper");
            if (vos_set_wallpaper(vwp_px, w, h) == 0) {
                snprintf(msg, sizeof(msg), "wallpaper: applied %dx%d", w, h);
                vos_log(VOS_LOG_APP, msg);
            } else {
                snprintf(msg, sizeof(msg), "wallpaper: set failed %dx%d", w, h);
                vos_log(VOS_LOG_APP, msg);
            }
            free(data);
            log_stage("wallpaper: exit");
            return 0;
        }
        free(data);
        data = 0;
        vos_log(VOS_LOG_APP, "wallpaper: default vwp invalid, trying png");
    } else if (alen <= 0) {
        log_stage("wallpaper: vwp read failed");
    }

    if (alen <= 0) {
        log_stage("wallpaper: using generated fallback");
        px = make_fallback_wallpaper(&w, &h);
        if (!px) {
            log_stage("wallpaper: fallback alloc failed");
            vos_log(VOS_LOG_APP, "wallpaper: fallback allocation failed");
            return 1;
        }
        snprintf(msg, sizeof(msg), "wallpaper: generated fallback %dx%d heap=%lu/%lu",
                 w, h, (unsigned long)umalloc_used(), (unsigned long)umalloc_capacity());
        vos_log(VOS_LOG_APP, msg);
    } else {
        log_stage("wallpaper: before png read");
        size = 0;
        if (stat_path(path, &kind, &size) == 0) {
            snprintf(msg, sizeof(msg), "wallpaper: png stat kind=%lu size=%lu", kind, size);
            vos_log(VOS_LOG_APP, msg);
        }
        if (read_all_exact(path, (size_t)size, &data, &len) != 0) {
            log_stage("wallpaper: png read failed");
            vos_log(VOS_LOG_APP, "wallpaper: cannot read image file");
            return 1;
        }
        log_stage("wallpaper: png read ok");
        snprintf(msg, sizeof(msg), "wallpaper: read %d bytes from %s heap=%lu/%lu",
                 len, path, (unsigned long)umalloc_used(), (unsigned long)umalloc_capacity());
        vos_log(VOS_LOG_APP, msg);

        log_stage("wallpaper: before png decode");
        px = image_decode(data, len, &w, &h);
        free(data);
        data = 0;
        log_stage("wallpaper: after png decode");
        if (!px || w <= 0 || h <= 0) {
            snprintf(msg, sizeof(msg), "wallpaper: decode failed (%s), heap=%lu/%lu, using fallback",
                     image_decode_failure_reason(),
                     (unsigned long)umalloc_used(), (unsigned long)umalloc_capacity());
            vos_log(VOS_LOG_APP, msg);
            px = make_fallback_wallpaper(&w, &h);
            if (!px) {
                log_stage("wallpaper: fallback alloc failed");
                vos_log(VOS_LOG_APP, "wallpaper: fallback allocation failed");
                return 1;
            }
        } else {
            snprintf(msg, sizeof(msg), "wallpaper: decoded real image %dx%d heap=%lu/%lu",
                     w, h, (unsigned long)umalloc_used(), (unsigned long)umalloc_capacity());
            vos_log(VOS_LOG_APP, msg);
        }
    }

    log_stage("wallpaper: before set_wallpaper");
    if (vos_set_wallpaper(px, w, h) == 0) {
        snprintf(msg, sizeof(msg), "wallpaper: applied %dx%d", w, h);
        vos_log(VOS_LOG_APP, msg);
    } else {
        snprintf(msg, sizeof(msg), "wallpaper: set failed %dx%d", w, h);
        vos_log(VOS_LOG_APP, msg);
    }

    log_stage("wallpaper: exit");
    image_free(px);
    return 0;
}
