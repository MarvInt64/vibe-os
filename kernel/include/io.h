#ifndef VIBEOS_IO_H
#define VIBEOS_IO_H

#include "types.h"

#ifdef ARCH_ARM64
/* ARM64 has no x86 port I/O. Stubs let shared headers compile cleanly;
 * ARM64 drivers use MMIO directly via arch.h mmio_read32/mmio_write32. */
static inline void io_wait(void) {}
static inline void outb(uint16_t p, uint8_t  v) { (void)p; (void)v; }
static inline void outw(uint16_t p, uint16_t v) { (void)p; (void)v; }
static inline void outl(uint16_t p, uint32_t v) { (void)p; (void)v; }
static inline uint8_t  inb(uint16_t p) { (void)p; return 0; }
static inline uint16_t inw(uint16_t p) { (void)p; return 0; }
static inline uint32_t inl(uint16_t p) { (void)p; return 0; }
#else /* x86_64 ---------------------------------------------------------- */

static inline void io_wait(void) {
    __asm__ volatile("outb %%al, $0x80" : : "a"(0));
}
static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}
static inline void outw(uint16_t port, uint16_t value) {
    __asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}
static inline uint16_t inw(uint16_t port) {
    uint16_t value;
    __asm__ volatile("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}
static inline void outl(uint16_t port, uint32_t value) {
    __asm__ volatile("outl %0, %1" : : "a"(value), "Nd"(port));
}
static inline uint32_t inl(uint16_t port) {
    uint32_t value;
    __asm__ volatile("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

#endif /* ARCH_ARM64 */
#endif /* VIBEOS_IO_H */
