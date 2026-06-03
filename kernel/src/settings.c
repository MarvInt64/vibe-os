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

#include "settings.h"
#include "vfs.h"
#include "string.h"
#include "serial.h"
#include "alloc.h"
#include <stddef.h>

/* Forward declare missing functions */
extern int atoi(const char *nptr);
extern int snprintf(char *str, size_t size, const char *format, ...);
extern char *strchr(const char *s, int c);
extern char *strtok(char *s, const char *delim);

static struct os_settings g_settings;
static const char *SETTINGS_PATH = "/etc/vibeos.conf";

static char *my_strchr(const char *s, int c) {
    while (*s != '\0') {
        if (*s == (char)c) return (char *)s;
        s++;
    }
    return NULL;
}

static int my_atoi(const char *nptr) {
    int val = 0;
    while (*nptr >= '0' && *nptr <= '9') {
        val = val * 10 + (*nptr - '0');
        nptr++;
    }
    return val;
}

static char *my_strtok(char *s, const char *delim) {
    static char *last = NULL;
    if (s) last = s;
    if (!last) return NULL;
    
    char *start = last;
    while (*last && my_strchr(delim, *last)) last++;
    if (!*last) return (last = NULL);
    
    start = last;
    while (*last && !my_strchr(delim, *last)) last++;
    if (*last) *last++ = '\0';
    else last = NULL;
    
    return start;
}

static void int_to_str(char *dst, uint32_t val) {
    int i = 0;
    if (val == 0) { dst[i++] = '0'; }
    while (val > 0) {
        dst[i++] = (val % 10) + '0';
        val /= 10;
    }
    dst[i] = '\0';
    for (int j = 0; j < i / 2; j++) {
        char t = dst[j]; dst[j] = dst[i - j - 1]; dst[i - j - 1] = t;
    }
}

static void write_line(char *buf, size_t *pos, size_t size, const char *key, uint32_t val) {
    if (*pos >= size) return;
    while (*key && *pos < size) buf[(*pos)++] = *key++;
    if (*pos < size) buf[(*pos)++] = '=';
    
    char temp[12];
    int_to_str(temp, val);
    char *t = temp;
    while (*t && *pos < size) buf[(*pos)++] = *t++;
    if (*pos < size) buf[(*pos)++] = '\n';
}

static void format_settings(char *buf, size_t size) {
    size_t pos = 0;
    write_line(buf, &pos, size, "sample_rate", g_settings.audio_sample_rate);
    write_line(buf, &pos, size, "buffer_size", g_settings.audio_buffer_size);
    write_line(buf, &pos, size, "buffer_count", g_settings.dma_buffer_count);
    write_line(buf, &pos, size, "master_vol", g_settings.master_volume);
    if (pos < size) buf[pos] = '\0';
}

static void parse_line(char *line) {
    char *key = line;
    char *val = my_strchr(line, '=');
    if (!val) return;
    *val++ = '\0';

    if (strcmp(key, "sample_rate") == 0) g_settings.audio_sample_rate = (uint32_t)my_atoi(val);
    else if (strcmp(key, "buffer_size") == 0) g_settings.audio_buffer_size = (uint32_t)my_atoi(val);
    else if (strcmp(key, "buffer_count") == 0) g_settings.dma_buffer_count = (uint32_t)my_atoi(val);
    else if (strcmp(key, "master_vol") == 0) g_settings.master_volume = (uint32_t)my_atoi(val);
}

void settings_init(void) {
    struct vfs_stat stat;
    if (vfs_stat_path(SETTINGS_PATH, &stat) == 0 && stat.size > 0) {
        char *buf = (char *)kmalloc(stat.size + 1);
        if (vfs_read(SETTINGS_PATH, 0, buf, stat.size) == (ssize_t)stat.size) {
            buf[stat.size] = '\0';
            char *line = my_strtok(buf, "\n");
            while (line) {
                parse_line(line);
                line = my_strtok(NULL, "\n");
            }
            kfree(buf);
            serial_write("Settings: loaded from text file\n");
            return;
        }
        kfree(buf);
    }
    
    /* Defaults */
    g_settings.audio_sample_rate = 48000;
    g_settings.audio_buffer_size = 512;
    g_settings.dma_buffer_count = 32;
    g_settings.master_volume = 100;
    settings_save();
}

struct os_settings *settings_get(void) {
    return &g_settings;
}

void settings_save(void) {
    char buf[512];
    format_settings(buf, sizeof(buf));
    
    vfs_write_all(SETTINGS_PATH, buf, strlen(buf));
    serial_write("Settings: saved to disk\n");
}
