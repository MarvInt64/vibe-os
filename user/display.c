/*
 * MIT License
 *
 * Copyright (c) 2026 Marvin Kicha
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/*
 * display — list supported display modes and show current resolution
 *
 * Usage:
 *   display            list all supported modes, mark the current one
 *   display set WxH    request a resolution change (e.g. display set 1920x1080)
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>

/* A single mode as returned by SYS_EDID_READ: { w, h, hz_x100 }. */
struct edid_mode_entry {
    unsigned short w;
    unsigned short h;
    unsigned short hz_x100;
};

/* Max modes we can fetch (matches kernel's edid_info.modes[64]). */
#define MAX_MODES 64

int main(int argc, char *argv[]) {
    if (argc >= 2) {
        /* Subcommand: set WxH */
        if (strcmp(argv[1], "set") == 0) {
            if (argc < 3) {
                fputs("Usage: display set <W>x<H>\n", stderr);
                return 1;
            }
            unsigned int w = 0, h = 0;
            if (sscanf(argv[2], "%ux%u", &w, &h) != 2 || w == 0 || h == 0) {
                fprintf(stderr, "display: invalid resolution '%s'\n", argv[2]);
                fputs("Usage: display set 1920x1080\n", stderr);
                return 1;
            }
            __sc3(SYS_DISPLAY_MODE, w, h, 0);
            return 0;
        }
        fprintf(stderr, "display: unknown subcommand '%s'\n", argv[1]);
        return 1;
    }

    /* --- No arguments: list supported modes ----------------------------- */

    struct edid_mode_entry modes[MAX_MODES];
    int n = (int)__sc3(SYS_EDID_READ,
                          (unsigned long)modes,
                          (unsigned long)MAX_MODES, 0);
    if (n <= 0) {
        fputs("display: no modes available\n", stderr);
        return 1;
    }

    /* Get current resolution. */
    unsigned int cur_raw = (unsigned int)__sc3(SYS_DISPLAY_MODE, 0, 0, 0);
    unsigned int cur_w = (cur_raw >> 16) & 0xffffu;
    unsigned int cur_h = cur_raw & 0xffffu;

    printf("  %-12s %-8s   %s\n", "Resolution", "Refresh", "");
    printf("  %-12s %-8s   %s\n", "------------", "--------", "----");
    for (int i = 0; i < n; i++) {
        const char *mark = (modes[i].w == cur_w && modes[i].h == cur_h)
                           ? " ← current" : "";
        printf("  %4ux%-7u %3u.%-02u Hz %s\n",
               modes[i].w, modes[i].h,
               modes[i].hz_x100 / 100, modes[i].hz_x100 % 100,
               mark);
    }

    return 0;
}
