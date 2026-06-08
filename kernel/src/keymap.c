/* VibeOS — Shared keymap engine.
 *
 * Maps Linux input keycodes to ASCII characters for multiple layouts.
 * Both x86 (PS/2 scancodes → Linux keycodes) and arm64 (virtio-input
 * already delivers Linux keycodes) route through this single module
 * so keyboard behaviour is identical across architectures.
 *
 * Layout design:
 *   keymap_table[layout][keycode] = { .normal, .shifted, .altgr }
 *
 *  - A keycode with all three zero is unmapped (e.g. function keys).
 *  - Only printable ASCII is supported; non-ASCII keys (umlauts on US
 *    layout) have zero entries — use a DE layout for those.
 *  - AltGr layer is only populated for layouts that need it (DE, FR, …).
 *
 * API:
 *   keymap_init()            — auto-detect or default to US
 *   keymap_set("de")         — switch layout at runtime
 *   keymap_translate(kc, mod)— kc = Linux keycode, mod = SHIFT|ALTGR|CTRL
 *
 * Modifier bits (passed in by the caller):
 *   KMOD_SHIFT  (1<<0)
 *   KMOD_ALTGR  (1<<1)
 *   KMOD_CTRL   (1<<2)
 */
#include "keymap.h"
#include "string.h"

/* ---- Per-key entry ---------------------------------------------------- */
struct key_entry {
    char normal;
    char shifted;
    char altgr;
};

/* ---- Layout tables (256 Linux keycodes) ------------------------------- */
static const struct key_entry us_keymap[256] = {
    /* 0 */ {'\0','\0','\0'},
    [1] = { 0x1b, 0x1b, '\0' },           /* KEY_ESC */
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
    [14] = { '\b','\b','\0' },             /* KEY_BACKSPACE */
    [15] = { '\t','\t','\0' },             /* KEY_TAB */
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
    [28] = { '\n','\n','\0' },             /* KEY_ENTER */
    [29] = { '\0','\0','\0' },             /* KEY_LEFTCTRL */
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
    [42] = { '\0','\0','\0' },             /* KEY_LEFTSHIFT */
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
    [54] = { '\0','\0','\0' },             /* KEY_RIGHTSHIFT */
    [55] = { '*', '*', '\0' },             /* KEY_KPASTERISK */
    [56] = { '\0','\0','\0' },             /* KEY_LEFTALT */
    [57] = { ' ', ' ', '\0' },
    [58] = { '\0','\0','\0' },             /* KEY_CAPSLOCK */
};

static const struct key_entry de_keymap[256] = {
    /* 0 */ {'\0','\0','\0'},
    [1] = { 0x1b, 0x1b, '\0' },
    [2] = { '1', '!', '\0' },
    [3] = { '2', '\"','\0' },
    [4] = { '3', 0xa7,'\0' },             /* § on Shift-3 */
    [5] = { '4', '$', '\0' },
    [6] = { '5', '%', '\0' },
    [7] = { '6', '&', '\0' },
    [8] = { '7', '/', '{' },              /* / on Shift, { on AltGr */
    [9] = { '8', '(', '[' },
    [10] = { '9', ')', ']' },
    [11] = { '0', '=', '}' },
    [12] = { 0xdf,'?', '\\' },            /* ß on normal, ? on Shift, \ on AltGr */
    [13] = { '`', '`', '\0' },            /* dead acute — simplified to ` */
    [14] = { '\b','\b','\0' },
    [15] = { '\t','\t','\0' },
    [16] = { 'q', 'Q', '@' },
    [17] = { 'w', 'W', '\0' },
    [18] = { 'e', 'E', '\0' },            /* € on AltGr-E — omit for now */
    [19] = { 'r', 'R', '\0' },
    [20] = { 't', 'T', '\0' },
    [21] = { 'z', 'Z', '\0' },            /* DE: Z/Y swapped */
    [22] = { 'u', 'U', '\0' },
    [23] = { 'i', 'I', '\0' },
    [24] = { 'o', 'O', '\0' },
    [25] = { 'p', 'P', '\0' },
    [26] = { 0xfc,'\0','\0' },            /* ü — no uppercase in ASCII */
    [27] = { '+', '*', '~' },
    [28] = { '\n','\n','\0' },
    [29] = { '\0','\0','\0' },
    [30] = { 'a', 'A', '\0' },
    [31] = { 's', 'S', '\0' },
    [32] = { 'd', 'D', '\0' },
    [33] = { 'f', 'F', '\0' },
    [34] = { 'g', 'G', '\0' },
    [35] = { 'h', 'H', '\0' },
    [36] = { 'j', 'J', '\0' },
    [37] = { 'k', 'K', '\0' },
    [38] = { 'l', 'L', '\0' },
    [39] = { 0xf6,'\0','\0' },            /* ö */
    [40] = { 0xe4,'\0','\0' },            /* ä */
    [41] = { '^', 0xb0,'\0' },            /* ^ on normal, ° on Shift */
    [42] = { '\0','\0','\0' },
    [43] = { '#', '\'','\0' },
    [44] = { 'y', 'Y', '\0' },            /* DE: Z/Y swapped */
    [45] = { 'x', 'X', '\0' },
    [46] = { 'c', 'C', '\0' },
    [47] = { 'v', 'V', '\0' },
    [48] = { 'b', 'B', '\0' },
    [49] = { 'n', 'N', '\0' },
    [50] = { 'm', 'M', '\0' },
    [51] = { ',', ';', '\0' },
    [52] = { '.', ':', '\0' },
    [53] = { '-', '_', '\0' },            /* DE: -_ on the key right of 0 */
    [54] = { '\0','\0','\0' },
    [55] = { '*', '*', '\0' },
    [56] = { '\0','\0','\0' },
    [57] = { ' ', ' ', '\0' },
    [58] = { '\0','\0','\0' },
    /* Extra keys for German layout — < > | key (left of Y on DE QWERTZ) */
    [86] = { '<', '>', '|' },            /* KEY_102ND (extra key on DE keyboard) */
};

/* ---- Active layout ---------------------------------------------------- */
static const struct key_entry *g_active_keymap = us_keymap;

/* ---- Public API ------------------------------------------------------- */
void keymap_init(void) {
    g_active_keymap = us_keymap;
}

int keymap_set(const char *name) {
    if (!name) return -1;
    /* Simple name match */
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

    /* CTRL masks the character to the control range */
    if (modifiers & KMOD_CTRL) {
        char base = e->normal;
        if (!base) base = e->shifted;
        if (base >= 'a' && base <= 'z') return (char)(base - 'a' + 1);
        if (base >= 'A' && base <= 'Z') return (char)(base - 'A' + 1);
        return '\0';
    }

    if (modifiers & KMOD_ALTGR) {
        if (e->altgr) return e->altgr;
        /* Fall through to shifted if no AltGr variant */
    }

    if (modifiers & KMOD_SHIFT) {
        if (e->shifted) return e->shifted;
        /* Fall through to normal if no shifted variant */
    }

    return e->normal;
}
