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
/* doomgeneric renders directly into DG_ScreenBuffer at DOOMGENERIC_RESX x RESY
 * (640x400 by default) — no pixel-doubling needed from our side.
 * We open a framed window slightly larger so the content area matches 640x400
 * after subtracting chrome.  On EV_RESIZE we get the actual content dimensions
 * and blit the buffer scaled to fill them. */
#define DOOM_BUF_W  DOOMGENERIC_RESX   /* 640 */
#define DOOM_BUF_H  DOOMGENERIC_RESY   /* 400 */

/* Request a window slightly larger than the buffer; after the kernel subtracts
 * the chrome we get at least DOOM_BUF_W x DOOM_BUF_H content pixels.
 * DG_Init drains the immediate EV_RESIZE to learn the exact content size, so
 * no chrome constant is hard-coded here. */
#define WIN_W       (DOOM_BUF_W + 64)   /* generous headroom for any chrome size */
#define WIN_H       (DOOM_BUF_H + 100)

/* Canvas: stride = live content width, updated on EV_RESIZE. */
#define BUF_MAX_W  900
#define BUF_MAX_H  640

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
static int      s_content_w = WIN_W;   /* actual content area (updated on resize) */
static int      s_content_h = WIN_H;
static unsigned int s_canvas[BUF_MAX_W * BUF_MAX_H];

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
    /* The kernel immediately enqueues an EV_RESIZE with the actual content
     * dimensions.  Drain it now so s_content_w/h are exact before the first
     * DG_DrawFrame — no chrome constant needed. */
    s_content_w = DOOM_BUF_W;
    s_content_h = DOOM_BUF_H;
    {
        struct winsys_event ev;
        int tries = 0;
        while (tries++ < 16 && event_poll(s_win_id, &ev) > 0) {
            if (ev.type == EV_RESIZE && ev.x > 0 && ev.y > 0) {
                s_content_w = ev.x < BUF_MAX_W ? ev.x : BUF_MAX_W;
                s_content_h = ev.y < BUF_MAX_H ? ev.y : BUF_MAX_H;
                break;
            }
        }
    }
    memset(s_canvas, 0, sizeof(s_canvas));
}

/* Blit DG_ScreenBuffer (DOOM_BUF_W x DOOM_BUF_H) into s_canvas scaled to the
 * current content area.  Stride == s_content_w so present is always correct. */
void DG_DrawFrame(void) {
    const unsigned int *src = DG_ScreenBuffer;
    int cw = s_content_w, ch = s_content_h;
    if (cw > BUF_MAX_W) cw = BUF_MAX_W;
    if (ch > BUF_MAX_H) ch = BUF_MAX_H;
    int y;

    if (cw == DOOM_BUF_W && ch == DOOM_BUF_H) {
        /* Fast path: content area matches buffer exactly — memcpy each row. */
        for (y = 0; y < ch; ++y)
            __builtin_memcpy(&s_canvas[y * cw], &src[y * DOOM_BUF_W],
                             (unsigned long)cw * sizeof(unsigned int));
    } else {
        /* Slow path: nearest-neighbour scale to fill the content area. */
        for (y = 0; y < ch; ++y) {
            unsigned int *dst = &s_canvas[y * cw];
            int sy = y * DOOM_BUF_H / ch;
            int x;
            for (x = 0; x < cw; ++x)
                dst[x] = src[sy * DOOM_BUF_W + x * DOOM_BUF_W / cw];
        }
    }
    win_present(s_win_id, s_canvas, cw, ch);
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
        } else if (ev.type == EV_RESIZE && ev.x > 0 && ev.y > 0) {
            /* Track the live content area so DG_DrawFrame scales correctly. */
            s_content_w = ev.x < BUF_MAX_W ? ev.x : BUF_MAX_W;
            s_content_h = ev.y < BUF_MAX_H ? ev.y : BUF_MAX_H;
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

/* Entry point: pass -iwad so DOOM finds the WAD on our filesystem. */
int main(void) {
    static char *argv[] = { "doom", "-iwad", "/games/doom1.wad", 0 };
    doomgeneric_Create(3, argv);
    for (;;) doomgeneric_Tick();
}
