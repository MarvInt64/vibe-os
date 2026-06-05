#ifndef VIBEOS_VGA_TEXT_H
#define VIBEOS_VGA_TEXT_H

#include "tty.h"
#include "types.h"

#define VGA_TEXT_COLUMNS 80
#define VGA_TEXT_ROWS 25

void vga_text_clear(uint8_t color);
void vga_text_write_message(const char *text, uint8_t color);
void vga_text_render_tty(const struct tty *tty);
void vga_text_render_tty_blink(const struct tty *tty, int cursor_visible);

#endif
