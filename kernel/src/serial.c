#include "serial.h"
#include "io.h"

void serial_init(void) {
    outb(0x3f8 + 1, 0x00);
    outb(0x3f8 + 3, 0x80);
    outb(0x3f8 + 0, 0x03);
    outb(0x3f8 + 1, 0x00);
    outb(0x3f8 + 3, 0x03);
    outb(0x3f8 + 2, 0xc7);
    outb(0x3f8 + 4, 0x0b);
    io_wait();
}

void serial_write_char(char c) {
    while ((inb(0x3f8 + 5) & 0x20u) == 0) {
    }

    outb(0x3f8, (uint8_t)c);
}

void serial_write_buffer(const char *text, size_t count) {
    size_t i;

    for (i = 0; i < count; ++i) {
        if (text[i] == '\n') {
            serial_write_char('\r');
        }
        serial_write_char(text[i]);
    }
}

void serial_write(const char *text) {
    while (*text != '\0') {
        if (*text == '\n') {
            serial_write_char('\r');
        }
        serial_write_char(*text);
        ++text;
    }
}

void serial_write_hex_u64(uint64_t value) {
    static const char hex[] = "0123456789ABCDEF";
    int shift;

    serial_write("0x");
    for (shift = 60; shift >= 0; shift -= 4) {
        serial_write_char(hex[(value >> shift) & 0xfu]);
    }
}

void serial_write_hex_u8(uint8_t value) {
    static const char hex[] = "0123456789ABCDEF";

    serial_write("0x");
    serial_write_char(hex[(value >> 4) & 0xfu]);
    serial_write_char(hex[value & 0xfu]);
}
