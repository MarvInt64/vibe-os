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
#include "image.h"

/* Read the whole file into a heap buffer (grows as needed). */
static int read_all(const char *path, unsigned char **out_buf, int *out_len) {
    int fd = (int)__sc1(SYS_OPEN, (uint64_t)(size_t)path);
    int cap = 2 * 1024 * 1024;
    int total = 0;
    int n;
    unsigned char *buf;

    if (fd < 0) return -1;
    buf = (unsigned char *)malloc((size_t)cap);
    if (!buf) { __sc1(SYS_CLOSE, (uint64_t)fd); return -1; }

    for (;;) {
        if (total >= cap) {
            int ncap = cap * 2;
            unsigned char *nb = (unsigned char *)realloc(buf, (size_t)ncap);
            if (!nb) { free(buf); __sc1(SYS_CLOSE, (uint64_t)fd); return -1; }
            buf = nb; cap = ncap;
        }
        n = (int)__sc3(SYS_READ, (uint64_t)fd, (uint64_t)(size_t)(buf + total),
                       (uint64_t)(cap - total));
        if (n <= 0) break;
        total += n;
    }
    __sc1(SYS_CLOSE, (uint64_t)fd);

    if (total <= 0) { free(buf); return -1; }
    *out_buf = buf;
    *out_len = total;
    return 0;
}

int main(void) {
    char path[256];
    int alen;
    unsigned char *data = 0;
    int len = 0;
    unsigned int *px;
    int w = 0, h = 0;

    path[0] = '\0';
    alen = vos_getarg(path, (int)sizeof(path));
    if (alen <= 0 || path[0] == '\0') {
        const char *def = "/wallpapers/default.png";
        int i = 0;
        for (; def[i] && i < (int)sizeof(path) - 1; ++i) path[i] = def[i];
        path[i] = '\0';
    }

    if (read_all(path, &data, &len) != 0) {
        vos_log(VOS_LOG_APP, "wallpaper: cannot read image file");
        return 1;
    }

    px = image_decode(data, len, &w, &h);
    free(data);
    if (!px || w <= 0 || h <= 0) {
        vos_log(VOS_LOG_APP, "wallpaper: decode failed");
        return 1;
    }

    if (vos_set_wallpaper(px, w, h) == 0)
        vos_log(VOS_LOG_APP, "wallpaper: applied");
    else
        vos_log(VOS_LOG_APP, "wallpaper: set failed");

    image_free(px);
    return 0;
}
