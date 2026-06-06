/* VibeOS arm64 — PL011 UART driver (UART0 in QEMU virt at 0x09000000).
 *
 * QEMU initialises the UART at reset, so arm64_uart_init() only ensures the
 * FIFO and transmitter are on — we do not reprogram the baud rate because
 * QEMU ignores it anyway.  The function is idempotent.
 *
 * These functions satisfy the serial.h interface on arm64:
 *   serial_init(), serial_write_char(), serial_write(), etc.
 */
#include "arch.h"
#include "../../include/serial.h"

void arm64_uart_init(void) {
    /* Wait for any pending transmission to drain */
    while (mmio_read32(UART_FR) & UART_FR_TXFF)
        ;
    /* Disable UART while configuring */
    mmio_write32(UART_CR, 0);

    /* 8-bit, no parity, 1 stop bit, FIFO enable */
    mmio_write32(UART_LCR_H, (1 << 4) | (3 << 5)); /* FEN | WLEN=8 */

    /* Re-enable UART: TXE + RXE + UARTEN */
    mmio_write32(UART_CR, (1 << 0) | (1 << 8) | (1 << 9));
}

void arm64_uart_putc(char c) {
    while (mmio_read32(UART_FR) & UART_FR_TXFF)
        ;
    mmio_write32(UART_DR, (uint32_t)(unsigned char)c);
}

int arm64_uart_can_read(void) {
    return !(mmio_read32(UART_FR) & UART_FR_RXFE);
}

char arm64_uart_getc(void) {
    while (!arm64_uart_can_read())
        ;
    return (char)(mmio_read32(UART_DR) & 0xFF);
}

/* ---- serial.h implementation for arm64 -------------------------------- */

void serial_init(void) {
    arm64_uart_init();
}

void serial_write_char(char c) {
    if (c == '\n')
        arm64_uart_putc('\r');
    arm64_uart_putc(c);
}

void serial_write(const char *s) {
    while (*s)
        serial_write_char(*s++);
}

void serial_write_buffer(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++)
        serial_write_char(s[i]);
}

void serial_write_hex_u64(uint64_t v) {
    const char *hex = "0123456789abcdef";
    serial_write("0x");
    for (int i = 60; i >= 0; i -= 4)
        serial_write_char(hex[(v >> i) & 0xf]);
}

void serial_write_hex_u8(uint8_t v) {
    const char *hex = "0123456789abcdef";
    serial_write_char(hex[v >> 4]);
    serial_write_char(hex[v & 0xf]);
}

int serial_can_read(void) {
    return arm64_uart_can_read();
}

char serial_read_byte(void) {
    return arm64_uart_getc();
}
