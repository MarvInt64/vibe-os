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

#include "input.h"
#include "io.h"
#include "journal.h"
#include "serial.h"

static int g_mouse_x;
static int g_mouse_y;
static uint8_t g_mouse_buttons;
static uint8_t g_mouse_cycle;
static uint8_t g_mouse_packet[4];
static uint8_t g_mouse_wheel;   /* 1 if the device delivers a 4th (Z) byte */
static uint8_t g_shift_down;
static uint8_t g_ctrl_down;
static uint8_t g_extended_code;
static uint32_t g_screen_width;
static uint32_t g_screen_height;
static uint8_t g_raw_diag_bytes;
static uint8_t g_mouse_diag_packets;

static void keyboard_push_char(struct keyboard_state *keyboard, char c) {
    if (keyboard->count < (sizeof(keyboard->chars) / sizeof(keyboard->chars[0]))) {
        keyboard->chars[keyboard->count++] = c;
    }
}

static void keyboard_push_bytes(struct keyboard_state *keyboard, const char *bytes, size_t count) {
    size_t i;

    for (i = 0; i < count; ++i) {
        keyboard_push_char(keyboard, bytes[i]);
    }
}

static void ps2_wait_write_ready(void) {
    uint32_t timeout = 100000u;

    while ((inb(0x64) & 0x02u) != 0u && timeout > 0u) {
        --timeout;
    }
}

static uint8_t ps2_wait_read_ready(void) {
    uint32_t timeout = 100000u;

    while ((inb(0x64) & 0x01u) == 0u && timeout > 0u) {
        --timeout;
    }

    return timeout > 0u;
}

static void ps2_flush_output(void) {
    while ((inb(0x64) & 0x01u) != 0u) {
        (void)inb(0x60);
    }
}

static void ps2_write_mouse(uint8_t value) {
    ps2_wait_write_ready();
    outb(0x64, 0xD4);
    ps2_wait_write_ready();
    outb(0x60, value);
}

static void ps2_ack_mouse(void) {
    if (ps2_wait_read_ready()) {
        (void)inb(0x60);
    }
}

static void mouse_cmd(uint8_t cmd) {
    ps2_write_mouse(cmd);
    ps2_ack_mouse();
}

static void mouse_set_rate(uint8_t rate) {
    mouse_cmd(0xF3);   /* set sample rate */
    mouse_cmd(rate);
}

/* "Magic knock" that switches an IntelliMouse-compatible device into wheel
 * mode (sample rates 200,100,80); the device then reports ID 0x03 and sends
 * 4-byte packets with a Z (scroll) byte. QEMU's PS/2 mouse supports this. */
static void mouse_enable_wheel(void) {
    uint8_t id = 0;
    mouse_set_rate(200);
    mouse_set_rate(100);
    mouse_set_rate(80);
    ps2_write_mouse(0xF2);   /* get device id */
    ps2_ack_mouse();
    if (ps2_wait_read_ready()) {
        id = inb(0x60);
    }
    g_mouse_wheel = (id == 0x03) ? 1 : 0;
    mouse_set_rate(100);     /* back to a sane rate */
}

static void clamp_mouse(void) {
    if (g_mouse_x < 0) {
        g_mouse_x = 0;
    }
    if (g_mouse_y < 0) {
        g_mouse_y = 0;
    }
    if ((uint32_t)g_mouse_x >= g_screen_width) {
        g_mouse_x = (int)g_screen_width - 1;
    }
    if ((uint32_t)g_mouse_y >= g_screen_height) {
        g_mouse_y = (int)g_screen_height - 1;
    }
}

static char keycode_to_char(uint8_t scancode) {
    switch (scancode) {
    case 0x02: return g_shift_down ? '!' : '1';
    case 0x03: return g_shift_down ? '"' : '2';
    case 0x04: return g_shift_down ? 0xA7 : '3';
    case 0x05: return g_shift_down ? '$' : '4';
    case 0x06: return g_shift_down ? '%' : '5';
    case 0x07: return g_shift_down ? '&' : '6';
    case 0x08: return g_shift_down ? '/' : '7';
    case 0x09: return g_shift_down ? '(' : '8';
    case 0x0A: return g_shift_down ? ')' : '9';
    case 0x0B: return g_shift_down ? '=' : '0';
    case 0x0C: return g_shift_down ? '_' : '-';
    case 0x0D: return g_shift_down ? '*' : '+';
    case 0x10: return g_shift_down ? 'Q' : 'q';
    case 0x11: return g_shift_down ? 'W' : 'w';
    case 0x12: return g_shift_down ? 'E' : 'e';
    case 0x13: return g_shift_down ? 'R' : 'r';
    case 0x14: return g_shift_down ? 'T' : 't';
    case 0x15: return g_shift_down ? 'Z' : 'z';
    case 0x16: return g_shift_down ? 'U' : 'u';
    case 0x17: return g_shift_down ? 'I' : 'i';
    case 0x18: return g_shift_down ? 'O' : 'o';
    case 0x19: return g_shift_down ? 'P' : 'p';
    case 0x1E: return g_shift_down ? 'A' : 'a';
    case 0x1F: return g_shift_down ? 'S' : 's';
    case 0x20: return g_shift_down ? 'D' : 'd';
    case 0x21: return g_shift_down ? 'F' : 'f';
    case 0x22: return g_shift_down ? 'G' : 'g';
    case 0x23: return g_shift_down ? 'H' : 'h';
    case 0x24: return g_shift_down ? 'J' : 'j';
    case 0x25: return g_shift_down ? 'K' : 'k';
    case 0x26: return g_shift_down ? 'L' : 'l';
    case 0x2C: return g_shift_down ? 'Y' : 'y';
    case 0x2D: return g_shift_down ? 'X' : 'x';
    case 0x2E: return g_shift_down ? 'C' : 'c';
    case 0x2F: return g_shift_down ? 'V' : 'v';
    case 0x30: return g_shift_down ? 'B' : 'b';
    case 0x31: return g_shift_down ? 'N' : 'n';
    case 0x32: return g_shift_down ? 'M' : 'm';
    case 0x33: return g_shift_down ? ';' : ',';
    case 0x34: return g_shift_down ? ':' : '.';
    case 0x35: return g_shift_down ? '_' : '#';
    case 0x39: return ' ';
    /* The German layout's key left of 'Y' produces < / > (and | with AltGr).
     * Without this the user could not type redirection operators at all. */
    case 0x56: return g_shift_down ? '>' : '<';
    default: return '\0';
    }
}

static char keycode_to_ctrl_char(uint8_t scancode) {
    char c = keycode_to_char(scancode);

    if (c >= 'A' && c <= 'Z') {
        c = (char)(c - 'A' + 'a');
    }
    if (c >= 'a' && c <= 'z') {
        return (char)(c - 'a' + 1);
    }
    return '\0';
}

void input_init(uint32_t screen_width, uint32_t screen_height) {
    g_screen_width = screen_width;
    g_screen_height = screen_height;
    g_mouse_x = (int)(screen_width / 2u);
    g_mouse_y = (int)(screen_height / 2u);
    g_mouse_buttons = 0;
    g_mouse_cycle = 0;
    g_mouse_wheel = 0;
    g_raw_diag_bytes = 0;
    g_mouse_diag_packets = 0;
    g_shift_down = 0;
    g_ctrl_down = 0;
    g_extended_code = 0;

    ps2_flush_output();

    ps2_wait_write_ready();
    outb(0x64, 0xA8);

    ps2_wait_write_ready();
    outb(0x64, 0x20);
    if (ps2_wait_read_ready()) {
        uint8_t command = inb(0x60);
        command |= 0x02u;
        command &= (uint8_t)~0x20u;
        ps2_wait_write_ready();
        outb(0x64, 0x60);
        ps2_wait_write_ready();
        outb(0x60, command);
    }

    ps2_write_mouse(0xF6);   /* set defaults */
    ps2_ack_mouse();
    mouse_enable_wheel();    /* runs magic-knock + reads device-ID byte from buffer */
    ps2_write_mouse(0xF4);   /* enable data reporting */
    ps2_ack_mouse();
}

void input_poll(struct mouse_state *mouse, struct keyboard_state *keyboard) {
    mouse->x = g_mouse_x;
    mouse->y = g_mouse_y;
    mouse->dx = 0;
    mouse->dy = 0;
    mouse->wheel = 0;
    mouse->buttons = g_mouse_buttons;
    mouse->moved = 0;
    mouse->left_pressed = 0;
    mouse->left_released = 0;
    mouse->right_pressed = 0;
    mouse->right_released = 0;

    keyboard->count = 0;
    keyboard->enter_pressed = 0;
    keyboard->backspace_pressed = 0;

    while ((inb(0x64) & 0x01u) != 0u) {
        uint8_t status = inb(0x64);
        uint8_t value = inb(0x60);

        if ((status & 0x20u) != 0u && g_raw_diag_bytes < 12u) {
            journal_log_hex(JOURNAL_APP, 0, "input raw status=", status);
            journal_log_hex(JOURNAL_APP, 0, "input raw value=", value);
            ++g_raw_diag_bytes;
        }

        if ((status & 0x20u) != 0u) {
            if (g_mouse_cycle == 0 && (value & 0x08u) == 0u) {
                continue;
            }

            g_mouse_packet[g_mouse_cycle++] = value;
            if (g_mouse_cycle == (g_mouse_wheel ? 4 : 3)) {
                int dx = (int)(int8_t)g_mouse_packet[1];
                int dy = (int)(int8_t)g_mouse_packet[2];
                uint8_t buttons = g_mouse_packet[0] & 0x07u;
                int dz = 0;

                if (g_mouse_wheel) {
                    dz = (int)(g_mouse_packet[3] & 0x0Fu);   /* 4-bit signed Z */
                    if (dz & 0x08) dz -= 16;
                }

                g_mouse_cycle = 0;
                if (g_mouse_diag_packets < 8u) {
                    journal_log_hex(JOURNAL_APP, 0, "input mouse pkt0=", g_mouse_packet[0]);
                    ++g_mouse_diag_packets;
                }
                g_mouse_x += dx;
                g_mouse_y -= dy;
                mouse->wheel += dz;
                clamp_mouse();

                mouse->dx += dx;
                mouse->dy += dy;
                mouse->moved = mouse->moved || dx != 0 || dy != 0;
                mouse->left_pressed = mouse->left_pressed || (((buttons & 0x01u) != 0u) && ((g_mouse_buttons & 0x01u) == 0u));
                mouse->left_released = mouse->left_released || (((buttons & 0x01u) == 0u) && ((g_mouse_buttons & 0x01u) != 0u));
                mouse->right_pressed = mouse->right_pressed || (((buttons & 0x02u) != 0u) && ((g_mouse_buttons & 0x02u) == 0u));
                mouse->right_released = mouse->right_released || (((buttons & 0x02u) == 0u) && ((g_mouse_buttons & 0x02u) != 0u));
                if (mouse->left_pressed) {
                    journal_log_hex(JOURNAL_APP, 0, "input left press pkt=", g_mouse_packet[0]);
                }
                if (mouse->left_released) {
                    journal_log_hex(JOURNAL_APP, 0, "input left release pkt=", g_mouse_packet[0]);
                }
                g_mouse_buttons = buttons;
                mouse->buttons = g_mouse_buttons;
                mouse->x = g_mouse_x;
                mouse->y = g_mouse_y;
            }
        } else {
            if (value == 0xE0u) {
                g_extended_code = 1;
                continue;
            }

            if (value == 0x2Au || value == 0x36u) {
                g_shift_down = 1;
                continue;
            }

            if (value == 0xAAu || value == 0xB6u) {
                g_shift_down = 0;
                continue;
            }

            if (!g_extended_code && value == 0x1Du) {
                g_ctrl_down = 1;
                continue;
            }

            if ((value == 0x9Du) || (g_extended_code && value == 0x1Du)) {
                g_ctrl_down = (value == 0x1Du) ? 1 : 0;
                if (value == 0x9Du) {
                    continue;
                }
            }

            if ((value & 0x80u) != 0u) {
                if (g_extended_code && value == 0x9Du) {
                    g_ctrl_down = 0;
                }
                g_extended_code = 0;
                continue;
            }

            if (g_extended_code) {
                g_extended_code = 0;
                switch (value) {
                case 0x47:
                    keyboard_push_bytes(keyboard, "\x1b[H", 3);
                    break;
                case 0x48:
                    keyboard_push_bytes(keyboard, "\x1b[A", 3);
                    break;
                case 0x49:
                    keyboard_push_bytes(keyboard, "\x1b[5~", 4);
                    break;
                case 0x4B:
                    keyboard_push_bytes(keyboard, "\x1b[D", 3);
                    break;
                case 0x4D:
                    keyboard_push_bytes(keyboard, "\x1b[C", 3);
                    break;
                case 0x4F:
                    keyboard_push_bytes(keyboard, "\x1b[F", 3);
                    break;
                case 0x50:
                    keyboard_push_bytes(keyboard, "\x1b[B", 3);
                    break;
                case 0x51:
                    keyboard_push_bytes(keyboard, "\x1b[6~", 4);
                    break;
                case 0x53:
                    keyboard_push_bytes(keyboard, "\x1b[3~", 4);
                    break;
                default:
                    break;
                }
                g_extended_code = 0;
                continue;
            }

            if (value == 0x1Cu) {
                keyboard->enter_pressed = 1;
                continue;
            }

            if (value == 0x0Eu) {
                keyboard->backspace_pressed = 1;
                continue;
            }

            if (value == 0x01u) {
                keyboard_push_char(keyboard, 0x1b);
                continue;
            }

            if (value == 0x0Fu) {
                keyboard_push_char(keyboard, '\t');
                continue;
            }

            if (g_ctrl_down) {
                char c = keycode_to_ctrl_char(value);
                if (c != '\0') {
                    keyboard_push_char(keyboard, c);
                }
                continue;
            }

            {
                char c = keycode_to_char(value);
                if (c != '\0') {
                    keyboard_push_char(keyboard, c);
                }
            }
        }
    }
}
