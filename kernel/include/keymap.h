/* VibeOS — Shared keymap interface.
 *
 * Maps Linux input keycodes to ASCII across architectures.
 * See kernel/src/keymap.c for the implementation.
 */
#ifndef VIBEOS_KEYMAP_H
#define VIBEOS_KEYMAP_H

/* Modifier bits */
#define KMOD_SHIFT   (1u << 0)
#define KMOD_ALTGR   (1u << 1)
#define KMOD_CTRL    (1u << 2)
#define KMOD_CAPS    (1u << 3)

void keymap_init(void);
int  keymap_set(const char *name);     /* "us" or "de", returns 0 on success */
char keymap_translate(unsigned keycode, unsigned modifiers);

#endif
