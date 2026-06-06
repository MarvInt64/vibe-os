/* VibeOS — userspace input state (arch-neutral).
 *
 * SYS_INPUT_POLL fills a vos_input_state with the current mouse/pointer state.
 * Returns 0 on success, <0 if no input device is available.
 */
#ifndef VIBEOS_SYS_INPUT_H
#define VIBEOS_SYS_INPUT_H

#include <sys/syscall.h>

struct vos_input_state {
    int x;           /* absolute x coordinate (0 = left edge) */
    int y;           /* absolute y coordinate (0 = top edge) */
    int buttons;     /* bit 0 = left, bit 1 = right, bit 2 = middle */
    int moved;       /* 1 if state changed since last poll, 0 if not */
};

static inline int vos_input_poll(struct vos_input_state *out) {
    return (int)__sc1(SYS_INPUT_POLL, (uint64_t)(uintptr_t)out);
}

#endif
