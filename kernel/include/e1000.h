#ifndef VIBEOS_E1000_H
#define VIBEOS_E1000_H

#include "types.h"

/* Intel 82540EM (e1000) NIC driver — the default network card QEMU attaches.
 * Polled (no IRQ): the main loop calls e1000_poll_receive() each iteration. */

#define E1000_VENDOR_ID 0x8086u
#define E1000_DEVICE_82540EM 0x100Eu

struct e1000_device {
    volatile uint8_t *mmio;   /* BAR0 MMIO base (identity-mapped) */
    uint8_t mac[6];
    uint8_t present;
    uint32_t rx_cur;
    uint32_t tx_cur;
};

/* Probe the PCI bus for a supported e1000, map it, read the MAC and bring up
 * the RX/TX rings. Returns 1 on success, 0 if no card was found. */
int e1000_init(struct e1000_device *dev);

/* Transmit one raw Ethernet frame (including header, excluding FCS).
 * Returns 0 on success, negative on error. */
int e1000_transmit(struct e1000_device *dev, const void *frame, uint16_t length);

/* Poll for one received frame. If a frame is available it is copied into
 * out_buffer (up to out_capacity bytes) and its length returned (>0).
 * Returns 0 if nothing is pending, negative on error. */
int e1000_poll_receive(struct e1000_device *dev, void *out_buffer, uint16_t out_capacity);

#endif
