#ifndef VIBEOS_SERIAL_H
#define VIBEOS_SERIAL_H

#include "types.h"

void serial_init(void);
void serial_write_char(char c);
void serial_write(const char *text);
void serial_write_buffer(const char *text, size_t count);
void serial_write_hex_u64(uint64_t value);
void serial_write_hex_u8(uint8_t value);

#endif
