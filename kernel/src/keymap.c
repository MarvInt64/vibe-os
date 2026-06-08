/* VibeOS — Shared keymap engine.
 *
 * Maps Linux input keycodes to ASCII characters for multiple layouts.
 * Both x86 (PS/2 scancodes → Linux keycodes) and arm64 (virtio-input
 * already delivers Linux keycodes) route through this single module.
 *
 * Supports:
 *   - Built-in layouts: "us", "de"
 *   - File-based layouts: keymap_set("/etc/keymap.de") loads from VFS
 *
 * File format (text, one entry per line):
 *   # comment
 *   KEYCODE  NORMAL  SHIFT  ALTGR
 *
 *   KEYCODE = decimal Linux keycode (e.g. 30 = KEY_A)
 *   NORMAL  = ASCII char, or \0 for none, or \n \t \e for specials
 *   SHIFT   = ASCII char with Shift held (or \0)
 *   ALTGR   = ASCII char with AltGr held (or \0)
 *
 * API:
 *   keymap_init()                  — auto-detect or default to "de"
 *   keymap_set("de")               — switch to built-in
 *   keymap_set("/etc/keymap.fr")   — load from file
 *   keymap_translate(kc, mod)      — translate keycode + modifiers → ASCII
 */
#include "keymap.h"
#include "string.h"
#include "alloc.h"
#include "vfs.h"
#include "ext2_fs.h"

extern struct ext2_filesystem g_fs;

/* ---- Per-key entry ---------------------------------------------------- */
struct key_entry {
    char normal;
    char shifted;
    char altgr;
};

/* ---- Built-in layout tables ------------------------------------------- */
static const struct key_entry us_keymap[256] = {
    [1] = { 0x1b, 0x1b, '\0' },             /* KEY_ESC */
    [2] = { '1', '!', '\0' },
    [3] = { '2', '@', '\0' },
    [4] = { '3', '#', '\0' },
    [5] = { '4', '$', '\0' },
    [6] = { '5', '%', '\0' },
    [7] = { '6', '^', '\0' },
    [8] = { '7', '&', '\0' },
    [9] = { '8', '*', '\0' },
    [10] = { '9', '(', '\0' },
    [11] = { '0', ')', '\0' },
    [12] = { '-', '_', '\0' },
    [13] = { '=', '+', '\0' },
    [14] = { '\b','\b','\0' },
    [15] = { '\t','\t','\0' },
    [16] = { 'q', 'Q', '\0' },
    [17] = { 'w', 'W', '\0' },
    [18] = { 'e', 'E', '\0' },
    [19] = { 'r', 'R', '\0' },
    [20] = { 't', 'T', '\0' },
    [21] = { 'y', 'Y', '\0' },
    [22] = { 'u', 'U', '\0' },
    [23] = { 'i', 'I', '\0' },
    [24] = { 'o', 'O', '\0' },
    [25] = { 'p', 'P', '\0' },
    [26] = { '[', '{', '\0' },
    [27] = { ']', '}', '\0' },
    [28] = { '\n','\n','\0' },
    [30] = { 'a', 'A', '\0' },
    [31] = { 's', 'S', '\0' },
    [32] = { 'd', 'D', '\0' },
    [33] = { 'f', 'F', '\0' },
    [34] = { 'g', 'G', '\0' },
    [35] = { 'h', 'H', '\0' },
    [36] = { 'j', 'J', '\0' },
    [37] = { 'k', 'K', '\0' },
    [38] = { 'l', 'L', '\0' },
    [39] = { ';', ':', '\0' },
    [40] = { '\'','\"','\0' },
    [41] = { '`', '~', '\0' },
    [43] = { '\\','|', '\0' },
    [44] = { 'z', 'Z', '\0' },
    [45] = { 'x', 'X', '\0' },
    [46] = { 'c', 'C', '\0' },
    [47] = { 'v', 'V', '\0' },
    [48] = { 'b', 'B', '\0' },
    [49] = { 'n', 'N', '\0' },
    [50] = { 'm', 'M', '\0' },
    [51] = { ',', '<', '\0' },
    [52] = { '.', '>', '\0' },
    [53] = { '/', '?', '\0' },
    [55] = { '*', '*', '\0' },
    [57] = { ' ', ' ', '\0' },
};

static const struct key_entry de_keymap[256] = {
    [1] = { 0x1b, 0x1b, '\0' },
    [2] = { '1', '!', '\0' },
    [3] = { '2', '\"','\0' },
    [4] = { '3', 0xa7,'\0' },
    [5] = { '4', '$', '\0' },
    [6] = { '5', '%', '\0' },
    [7] = { '6', '&', '\0' },
    [8] = { '7', '/', '{' },
    [9] = { '8', '(', '[' },
    [10] = { '9', ')', ']' },
    [11] = { '0', '=', '}' },
    [12] = { 0xdf,'?', '\\' },
    [13] = { '`', '`', '\0' },
    [14] = { '\b','\b','\0' },
    [15] = { '\t','\t','\0' },
    [16] = { 'q', 'Q', '@' },
    [17] = { 'w', 'W', '\0' },
    [18] = { 'e', 'E', '\0' },
    [19] = { 'r', 'R', '\0' },
    [20] = { 't', 'T', '\0' },
    [21] = { 'z', 'Z', '\0' },
    [22] = { 'u', 'U', '\0' },
    [23] = { 'i', 'I', '\0' },
    [24] = { 'o', 'O', '\0' },
    [25] = { 'p', 'P', '\0' },
    [26] = { 0xfc,'\0','\0' },
    [27] = { '+', '*', '~' },
    [28] = { '\n','\n','\0' },
    [30] = { 'a', 'A', '\0' },
    [31] = { 's', 'S', '\0' },
    [32] = { 'd', 'D', '\0' },
    [33] = { 'f', 'F', '\0' },
    [34] = { 'g', 'G', '\0' },
    [35] = { 'h', 'H', '\0' },
    [36] = { 'j', 'J', '\0' },
    [37] = { 'k', 'K', '\0' },
    [38] = { 'l', 'L', '\0' },
    [39] = { 0xf6,'\0','\0' },
    [40] = { 0xe4,'\0','\0' },
    [41] = { '^', 0xb0,'\0' },
    [43] = { '#', '\'','\0' },
    [44] = { 'y', 'Y', '\0' },
    [45] = { 'x', 'X', '\0' },
    [46] = { 'c', 'C', '\0' },
    [47] = { 'v', 'V', '\0' },
    [48] = { 'b', 'B', '\0' },
    [49] = { 'n', 'N', '\0' },
    [50] = { 'm', 'M', '\0' },
    [51] = { ',', ';', '\0' },
    [52] = { '.', ':', '\0' },
    [53] = { '-', '_', '\0' },
    [55] = { '*', '*', '\0' },
    [57] = { ' ', ' ', '\0' },
    [86] = { '<', '>', '|' },
};

/* ---- Active layout ---------------------------------------------------- */
static struct key_entry  g_custom_keymap[256];
static const struct key_entry *g_active_keymap = de_keymap;

/* ---- Parse a keymap file buffer into g_custom_keymap ------------------ */
static int keymap_parse_buffer(const char *buf, size_t len) {
    memset(g_custom_keymap, 0, sizeof(g_custom_keymap));

    const char *p = buf;
    const char *end = buf + len;

    while (p < end) {
        /* Skip whitespace and comments */
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
        if (p >= end || *p == '#') {
            while (p < end && *p != '\n') p++;
            continue;
        }

        /* Parse: KEYCODE NORMAL SHIFT ALTGR */
        unsigned kc = 0;
        while (p < end && *p >= '0' && *p <= '9') {
            kc = kc * 10 + (unsigned)(*p - '0');
            p++;
        }
        if (kc >= 256) { while (p < end && *p != '\n') p++; continue; }

        /* Parse three chars (normal, shift, altgr) — space-separated */
        char vals[3] = {'\0','\0','\0'};
        for (int i = 0; i < 3; i++) {
            while (p < end && (*p == ' ' || *p == '\t')) p++;
            if (p >= end || *p == '\n' || *p == '#') break;
            if (p[0] == '\\' && p+1 < end) {
                p++;
                switch (*p) {
                case 'n': vals[i] = '\n'; break;
                case 't': vals[i] = '\t'; break;
                case 'e': vals[i] = 0x1b; break;
                case '0': vals[i] = '\0'; break;
                case '\\': vals[i] = '\\'; break;
                default:  vals[i] = *p; break;
                }
            } else {
                vals[i] = *p;
            }
            p++;
        }
        g_custom_keymap[kc].normal  = vals[0];
        g_custom_keymap[kc].shifted = vals[1];
        g_custom_keymap[kc].altgr   = vals[2];

        while (p < end && *p != '\n') p++;
    }
    return 0;
}

/* ---- Public API ------------------------------------------------------- */
void keymap_init(void) {
    g_active_keymap = de_keymap;
}

int keymap_set(const char *name) {
    if (!name) return -1;

    /* If name contains '/', treat as file path */
    if (name[0] == '/' || (name[0] == '.' && name[1] == '/')) {
        /* Read keymap file from VFS */
        uint32_t ino = ext2_lookup_inode(&g_fs, name);
        if (!ino) return -1;
        struct ext2_inode *node = &g_fs.inode_table[ino - 1];
        uint32_t fsize = node->size;
        if (fsize == 0 || fsize > 65536) return -1;

        char *buf = (char *)kmalloc(fsize + 1);
        if (!buf) return -1;
        ssize_t got = ext2_read(&g_fs, ino, 0, fsize, buf);
        if (got <= 0) { kfree(buf); return -1; }
        buf[got] = '\0';

        int r = keymap_parse_buffer(buf, (size_t)got);
        kfree(buf);
        if (r != 0) return -1;
        g_active_keymap = g_custom_keymap;
        return 0;
    }

    /* Built-in layout names */
    if (name[0] == 'd' && name[1] == 'e' && name[2] == '\0') {
        g_active_keymap = de_keymap;
        return 0;
    }
    if (name[0] == 'u' && name[1] == 's' && name[2] == '\0') {
        g_active_keymap = us_keymap;
        return 0;
    }
    return -1;
}

char keymap_translate(unsigned keycode, unsigned modifiers) {
    if (keycode >= 256) return '\0';
    const struct key_entry *e = &g_active_keymap[keycode];

    if (modifiers & KMOD_CTRL) {
        char base = e->normal;
        if (!base) base = e->shifted;
        if (base >= 'a' && base <= 'z') return (char)(base - 'a' + 1);
        if (base >= 'A' && base <= 'Z') return (char)(base - 'A' + 1);
        return '\0';
    }

    if (modifiers & KMOD_ALTGR) {
        if (e->altgr) return e->altgr;
    }

    if (modifiers & KMOD_SHIFT) {
        if (e->shifted) return e->shifted;
    }

    return e->normal;
}
