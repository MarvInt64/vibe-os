#ifndef VIBEOS_SERIAL_H
#define VIBEOS_SERIAL_H

#include "types.h"

void serial_init(void);
void serial_write_char(char c);
void serial_write(const char *text);
void serial_write_buffer(const char *text, size_t count);
void serial_write_hex_u64(uint64_t value);
void serial_write_hex_u8(uint8_t value);

/* Returns 1 if at least one byte is ready to read from COM1, 0 otherwise. */
int  serial_can_read(void);

/* Read one byte from COM1. Caller must check serial_can_read() first.
 * Blocks until a byte is available (LSR Data Ready bit is polled). */
char serial_read_byte(void);

#endif
