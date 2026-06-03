/* doomgeneric_vibeos.c — VibeOS platform layer for doomgeneric.
 *
 * Implements the six DG_ callbacks:
 *   DG_Init          open a 640×400 window and allocate the canvas
 *   DG_DrawFrame     blit DG_ScreenBuffer → canvas → SYS_WINDOW_PRESENT
 *   DG_GetTicksMs    milliseconds since start using SYS_SYSTEM_INFO
 *   DG_SleepMs       yield-based sleep (no blocking sleep syscall needed)
 *   DG_GetKey        translate keyboard events to DOOM key codes
 *   DG_SetWindowTitle no-op (window title is fixed at "DOOM")
 *
 * DOOM's 320×200 framebuffer is pixel-doubled to 640×400 so it is readable
 * without fractional scaling — each DOOM pixel becomes a 2×2 block.
 */

#include "doomgeneric.h"
#include "doomkeys.h"

#include <vibeos.h>
#include <sys/syscall.h>
#include <stdlib.h>
#include <string.h>

/* ---- Window geometry --------------------------------------------------- */
#define DOOM_W   320
#define DOOM_H   200
#define SCALE    2               /* pixel-doubling factor */
#define WIN_W    (DOOM_W * SCALE)
#define WIN_H    (DOOM_H * SCALE)

/* VexUI/window-server limits (must match VUI_MAX_W/H). */
#define BUF_STRIDE  900
#define BUF_H_MAX   640

/* ---- vexui event constants (mirrored from vexui.c) -------------------- */
#define EV_MOUSE_MOVE  1
#define EV_MOUSE_DOWN  2
#define EV_MOUSE_UP    3
#define EV_KEY         4
#define EV_CLOSE       5
#define EV_RESIZE      6
#define EV_CONTEXT_MENU 7
#define EV_MENU_ACTION  7
#define EV_SCROLL      8

struct winsys_event {
    unsigned int type;
    int          x, y;
    unsigned int buttons;
    unsigned int key;
};

/* SYS_WINDOW_PRESENT wrapper. */
static inline void win_present(int id, const unsigned int *px, int w, int h) {
    __sc6(SYS_WINDOW_PRESENT, (unsigned long)id,
          (unsigned long)(size_t)px,
          (unsigned long)w, (unsigned long)h, 0, 0);
}

/* SYS_EVENT_POLL wrapper. */
static inline int event_poll(int id, struct winsys_event *ev) {
    return (int)__sc2(SYS_EVENT_POLL, (unsigned long)id,
                      (unsigned long)(size_t)ev);
}

/* ---- State ------------------------------------------------------------- */
static int      s_win_id = -1;
static unsigned int s_canvas[BUF_STRIDE * BUF_H_MAX];

/* Key event queue (small ring — DOOM drains it every tic). */
#define KEYQ_CAP 32
static struct { unsigned char key; int pressed; } s_keyq[KEYQ_CAP];
static int s_keyq_head = 0, s_keyq_tail = 0;

static void keyq_push(unsigned char key, int pressed) {
    int next = (s_keyq_head + 1) % KEYQ_CAP;
    if (next != s_keyq_tail) {
        s_keyq[s_keyq_head].key = key;
        s_keyq[s_keyq_head].pressed = pressed;
        s_keyq_head = next;
    }
}

/* ---- Key translation --------------------------------------------------- */
/* DOOM uses its own key codes (doomkeys.h). Map vexui key values.
 * Arrow keys arrive as ANSI escape sequences ESC [ A/B/C/D parsed below.
 * We maintain a minimal soft-key-up by re-queuing a release on the NEXT
 * call when a key is still "held" (VibeOS does not send key-up events). */

static unsigned char s_held_key = 0;
static int s_held_life = 0;   /* decremented each DG_GetKey call */

static unsigned char vexui_to_doom_key(unsigned int k) {
    switch (k) {
        case 0x1b:  return KEY_ESCAPE;
        case '\r':
        case '\n':  return KEY_ENTER;
        case ' ':   return KEY_USE;
        case 0x08:
        case 0x7f:  return KEY_BACKSPACE;
        case '[':   return KEY_STRAFE_L;   /* strafe left  */
        case ']':   return KEY_STRAFE_R;   /* strafe right */
        case ',':   return KEY_FIRE;
        case '.':   return KEY_USE;
        case '1': case '2': case '3': case '4':
        case '5': case '6': case '7':
            return (unsigned char)('0' + (k - '0'));
        default:
            if (k >= 'a' && k <= 'z') return (unsigned char)k;
            if (k >= 'A' && k <= 'Z') return (unsigned char)(k | 32);
            return 0;
    }
}

/* ANSI escape parser state for arrow keys. */
static unsigned char s_esc_buf[4];
static int           s_esc_len = 0;

static void parse_key_byte(unsigned char b) {
    if (s_esc_len == 0 && b == 0x1b) {
        s_esc_buf[s_esc_len++] = b;
        return;
    }
    if (s_esc_len == 1 && b == '[') {
        s_esc_buf[s_esc_len++] = b;
        return;
    }
    if (s_esc_len == 2) {
        unsigned char doom_k = 0;
        switch (b) {
            case 'A': doom_k = KEY_UPARROW;    break;
            case 'B': doom_k = KEY_DOWNARROW;  break;
            case 'C': doom_k = KEY_RIGHTARROW; break;
            case 'D': doom_k = KEY_LEFTARROW;  break;
        }
        s_esc_len = 0;
        if (doom_k) { keyq_push(doom_k, 1); s_held_key = doom_k; s_held_life = 6; }
        return;
    }
    /* Flush a dangling ESC as KEY_ESCAPE. */
    if (s_esc_len > 0) { keyq_push(KEY_ESCAPE, 1); s_esc_len = 0; }

    unsigned char dk = vexui_to_doom_key((unsigned int)b);
    if (dk) { keyq_push(dk, 1); s_held_key = dk; s_held_life = 3; }
}

/* ---- DG_ callbacks ----------------------------------------------------- */

void DG_Init(void) {
    s_win_id = (int)__sc3(SYS_WINDOW_CREATE,
                           (unsigned long)(size_t)"DOOM",
                           (unsigned long)WIN_W,
                           (unsigned long)WIN_H);
    memset(s_canvas, 0, sizeof(s_canvas));
}

/* Pixel-double DOOM's 320×200 XRGB buffer into s_canvas at stride BUF_STRIDE,
 * then present. DOOM packs rows at DOOM_W (no padding). */
void DG_DrawFrame(void) {
    const unsigned int *src = DG_ScreenBuffer;
    int y;

    for (y = 0; y < DOOM_H; ++y) {
        unsigned int *dst0 = &s_canvas[(y * 2)     * BUF_STRIDE];
        unsigned int *dst1 = &s_canvas[(y * 2 + 1) * BUF_STRIDE];
        int x;
        for (x = 0; x < DOOM_W; ++x) {
            unsigned int px = src[y * DOOM_W + x];
            dst0[x * 2]     = px;
            dst0[x * 2 + 1] = px;
            dst1[x * 2]     = px;
            dst1[x * 2 + 1] = px;
        }
    }
    win_present(s_win_id, s_canvas, WIN_W, WIN_H);
}

void DG_SleepMs(unsigned int ms) {
    /* Each tick is ~10 ms (100 Hz timer); yield in a loop. */
    unsigned int ticks = (ms + 9) / 10;
    if (ticks == 0) ticks = 1;
    vos_sleep_ticks((unsigned long)ticks);
}

unsigned int DG_GetTicksMs(void) {
    struct vos_system_info info;
    if (vos_system_info(&info) != 0) return 0;
    unsigned int hz = info.timer_hz ? info.timer_hz : 100;
    return (unsigned int)((info.uptime_ticks * 1000UL) / hz);
}

int DG_GetKey(int *pressed, unsigned char *doom_key) {
    struct winsys_event ev;

    /* Synthetic key-up for held keys (arrow keys etc.). */
    if (s_held_life > 0 && --s_held_life == 0 && s_held_key) {
        *pressed = 0;
        *doom_key = s_held_key;
        s_held_key = 0;
        return 1;
    }

    /* Drain window events into the queue. */
    while (event_poll(s_win_id, &ev) > 0) {
        if (ev.type == EV_KEY) {
            /* vexui delivers each byte of a multi-byte escape sequence as a
             * separate EV_KEY; parse them into DOOM key codes. */
            unsigned int k = ev.key;
            if (k < 256) parse_key_byte((unsigned char)k);
        }
    }

    if (s_keyq_head == s_keyq_tail) return 0;
    *pressed  = s_keyq[s_keyq_tail].pressed;
    *doom_key = s_keyq[s_keyq_tail].key;
    s_keyq_tail = (s_keyq_tail + 1) % KEYQ_CAP;
    return 1;
}

void DG_SetWindowTitle(const char *title) {
    (void)title;  /* fixed title "DOOM" at window creation */
}
