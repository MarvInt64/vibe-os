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

#include "vga_text.h"
#include "string.h"

static volatile uint16_t *const g_vga_text = (volatile uint16_t *)0xb8000u;

static void vga_put_at(int x, int y, char c, uint8_t color) {
    g_vga_text[(y * VGA_TEXT_COLUMNS) + x] = ((uint16_t)color << 8) | (uint8_t)c;
}

void vga_text_clear(uint8_t color) {
    int y;
    int x;

    for (y = 0; y < VGA_TEXT_ROWS; ++y) {
        for (x = 0; x < VGA_TEXT_COLUMNS; ++x) {
            vga_put_at(x, y, ' ', color);
        }
    }
}

void vga_text_write_message(const char *text, uint8_t color) {
    size_t i = 0;

    while (text[i] != '\0' && i < VGA_TEXT_COLUMNS * VGA_TEXT_ROWS) {
        g_vga_text[i] = ((uint16_t)color << 8) | (uint8_t)text[i];
        ++i;
    }
}

void vga_text_render_tty(const struct tty *tty) {
    int start_line = 0;
    int max_lines = VGA_TEXT_ROWS - 1;
    int row = 0;
    size_t i;
    size_t out = 0;
    char input_line[TTY_MAX_LINE_CHARS];

    vga_text_clear(0x07u);

    if (tty->ansi_mode) {
        int y;
        int x;

        for (y = 0; y < VGA_TEXT_ROWS && y < TTY_SCREEN_ROWS; ++y) {
            for (x = 0; x < VGA_TEXT_COLUMNS && x < TTY_SCREEN_COLS; ++x) {
                uint8_t color = (tty->screen_attr[y][x] & TTY_ATTR_INVERSE) ? 0x70u : 0x07u;
                vga_put_at(x, y, tty->screen[y][x], color);
            }
        }

        if (tty->cursor_row >= 0 && tty->cursor_row < VGA_TEXT_ROWS &&
            tty->cursor_col >= 0 && tty->cursor_col < VGA_TEXT_COLUMNS) {
            uint8_t color = (tty->screen_attr[tty->cursor_row][tty->cursor_col] & TTY_ATTR_INVERSE) ? 0x0fu : 0x70u;
            char c = tty->screen[tty->cursor_row][tty->cursor_col];
            vga_put_at(tty->cursor_col, tty->cursor_row, c == ' ' ? '_' : c, color);
        }
        return;
    }

    if ((int)tty->line_count > max_lines) {
        start_line = (int)tty->line_count - max_lines;
    }

    for (i = (size_t)start_line; i < tty->line_count && row < max_lines; ++i, ++row) {
        size_t j;

        for (j = 0; tty->lines[i][j] != '\0' && j < VGA_TEXT_COLUMNS; ++j) {
            vga_put_at((int)j, row, tty->lines[i][j], 0x07u);
        }
    }

    for (i = 0; i < tty->partial_length && out + 1 < sizeof(input_line); ++i) {
        input_line[out++] = tty->partial_output[i];
    }
    for (i = 0; i < tty->input_length && out + 1 < sizeof(input_line); ++i) {
        input_line[out++] = tty->input[i];
    }
    input_line[out] = '\0';

    vga_put_at(0, VGA_TEXT_ROWS - 1, '>', 0x0au);
    vga_put_at(1, VGA_TEXT_ROWS - 1, ' ', 0x07u);

    for (i = 0; input_line[i] != '\0' && i + 2 < VGA_TEXT_COLUMNS; ++i) {
        vga_put_at((int)i + 2, VGA_TEXT_ROWS - 1, input_line[i], 0x0fu);
    }

    if (out + 2 < VGA_TEXT_COLUMNS) {
        vga_put_at((int)out + 2, VGA_TEXT_ROWS - 1, '_', 0x0fu);
    }
}
