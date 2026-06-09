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

#include "bga.h"
#include "io.h"
#include "serial.h"
#include "edid.h"

#define BGA_IOPORT_INDEX 0x01CEu
#define BGA_IOPORT_DATA 0x01CFu

#define BGA_REG_ID 0u
#define BGA_REG_XRES 1u
#define BGA_REG_YRES 2u
#define BGA_REG_BPP 3u
#define BGA_REG_ENABLE 4u

#define BGA_ID5 0xB0C5u
#define BGA_ENABLE_FLAG 0x01u
#define BGA_ENABLE_LFB 0x40u

#define PCI_CONFIG_ADDRESS 0x0CF8u
#define PCI_CONFIG_DATA 0x0CFCu

static uint32_t pci_config_read_u32(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset) {
    uint32_t address = 0x80000000u |
                       ((uint32_t)bus << 16) |
                       ((uint32_t)slot << 11) |
                       ((uint32_t)function << 8) |
                       ((uint32_t)(offset & 0xfcu));

    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

static uintptr_t bga_find_lfb_address(void) {
    uintptr_t best_prefetchable_bar = 0;
    uintptr_t best_bar = 0;
    uint8_t slot;

    for (slot = 0; slot < 32u; ++slot) {
        uint32_t vendor_device = pci_config_read_u32(0u, slot, 0u, 0x00u);
        uint32_t class_register;
        uint8_t class_code;
        uint8_t subclass;
        uint8_t bar_index;

        if ((vendor_device & 0xffffu) == 0xffffu) {
            continue;
        }

        class_register = pci_config_read_u32(0u, slot, 0u, 0x08u);
        class_code = (uint8_t)((class_register >> 24) & 0xffu);
        subclass = (uint8_t)((class_register >> 16) & 0xffu);

        if (class_code == 0x03u) {
            serial_write("PCI: graphics dev found at slot ");
            serial_write_hex_u64(slot);
            serial_write(" id=");
            serial_write_hex_u64(vendor_device);
            serial_write("\n");
        }

        if (class_code != 0x03u || (subclass != 0x00u && subclass != 0x80u)) {
            continue;
        }

        for (bar_index = 0; bar_index < 6u; ++bar_index) {
            uint32_t bar = pci_config_read_u32(0u, slot, 0u, (uint8_t)(0x10u + (bar_index * 4u)));
            uintptr_t base;
            int prefetchable;

            if (bar == 0u || (bar & 0x01u) != 0u) {
                continue;
            }

            base = (uintptr_t)(bar & ~0x0fu);
            prefetchable = (bar & 0x08u) != 0u;

            serial_write("  BAR");
            serial_write_hex_u64(bar_index);
            serial_write(" addr=");
            serial_write_hex_u64(base);
            if (prefetchable) serial_write(" (prefetchable)");
            serial_write("\n");

            if (base >= 0x00100000u && prefetchable && base > best_prefetchable_bar) {
                best_prefetchable_bar = base;
            }
            if (base >= 0x00100000u && base > best_bar) {
                best_bar = base;
            }
        }
    }

    if (best_prefetchable_bar != 0) {
        return best_prefetchable_bar;
    }

    if (best_bar != 0) {
        return best_bar;
    }

    return BGA_LFB_FALLBACK_PHYS_ADDR;
}

static uintptr_t bga_lfb = 0;

static void bga_write(uint16_t index, uint16_t value) {
    outw(BGA_IOPORT_INDEX, index);
    outw(BGA_IOPORT_DATA, value);
}

static uint16_t bga_read(uint16_t index) {
    outw(BGA_IOPORT_INDEX, index);
    return inw(BGA_IOPORT_DATA);
}

int bga_init_framebuffer(struct boot_framebuffer *out_fb, uint32_t width, uint32_t height, uint32_t bpp) {
    uint16_t id = bga_read(BGA_REG_ID);
    uintptr_t lfb_address;

    if (out_fb == 0 || id < 0xB0C0u || id > BGA_ID5) {
        return 0;
    }

    bga_write(BGA_REG_ENABLE, 0u);
    bga_write(BGA_REG_XRES, (uint16_t)width);
    bga_write(BGA_REG_YRES, (uint16_t)height);
    bga_write(BGA_REG_BPP, (uint16_t)bpp);
    bga_write(BGA_REG_ENABLE, BGA_ENABLE_FLAG | BGA_ENABLE_LFB);

    if (bga_read(BGA_REG_XRES) != (uint16_t)width || bga_read(BGA_REG_BPP) != (uint16_t)bpp) {
        serial_write("VIBEOS: BGA register verify FAILED\n");
        return 0;
    }

    lfb_address = bga_find_lfb_address();
    bga_lfb = lfb_address;
    out_fb->address = lfb_address;
    out_fb->width = width;
    out_fb->height = height;
    out_fb->pitch = width * (bpp / 8u);
    out_fb->bpp = (uint8_t)bpp;
    return 1;
}

/* ---- BGA DDC / I2C bit-banging for EDID -------------------------------- */

#define BGA_I2C_OFFSET  0x500u
#define I2C_SCL         0x01u
#define I2C_SDA         0x02u
#define I2C_DIR         0x04u   /* 1 = output, 0 = input */

/* Read the I2C register (volatile MMIO). */
static inline uint8_t i2c_read_reg(void) {
    return *(volatile uint8_t *)(bga_lfb + BGA_I2C_OFFSET);
}

/* Write the I2C register. */
static inline void i2c_write_reg(uint8_t val) {
    *(volatile uint8_t *)(bga_lfb + BGA_I2C_OFFSET) = val;
}

/* Short delay for I2C timing (~5 µs). */
static void i2c_delay(void) {
    for (volatile int i = 0; i < 100; i++) { __asm__ volatile("" ::: "memory"); }
}

static void i2c_scl_lo(void) { i2c_write_reg(I2C_DIR); }   /* SCL=0, SDA=0 */
static void i2c_scl_hi(void) { i2c_write_reg(I2C_DIR | I2C_SCL); }  /* SCL=1 */
static void i2c_sda_lo(void) { i2c_write_reg(I2C_DIR); }
static void i2c_sda_hi(void) { i2c_write_reg(I2C_DIR | I2C_SDA); }
static uint8_t i2c_sda_in(void) { return i2c_read_reg() & I2C_SDA; }

static void i2c_start(void) {
    i2c_sda_hi(); i2c_delay();
    i2c_scl_hi(); i2c_delay();
    i2c_sda_lo(); i2c_delay();
    i2c_scl_lo(); i2c_delay();
}

static void i2c_stop(void) {
    i2c_sda_lo(); i2c_delay();
    i2c_scl_hi(); i2c_delay();
    i2c_sda_hi(); i2c_delay();
}

/* Send one byte, return 0 if ACK'd, -1 if NACK'd. */
static int i2c_send_byte(uint8_t b) {
    for (int i = 7; i >= 0; i--) {
        if (b & (1 << i)) i2c_sda_hi(); else i2c_sda_lo();
        i2c_delay();
        i2c_scl_hi(); i2c_delay();
        i2c_scl_lo(); i2c_delay();
    }
    /* Release SDA for ACK, switch to input */
    i2c_write_reg(I2C_SCL);  /* SCL=1, SDA=input */
    i2c_delay();
    i2c_scl_hi(); i2c_delay();
    int ack = i2c_sda_in();  /* 0 = ACK, 1 = NACK */
    i2c_scl_lo(); i2c_delay();
    i2c_write_reg(I2C_DIR);  /* back to output mode */
    return ack ? -1 : 0;
}

/* Read one byte, send ack=0 for ACK, ack=1 for NACK. */
static uint8_t i2c_recv_byte(int ack) {
    uint8_t val = 0;
    i2c_write_reg(I2C_SCL);  /* SCL=1, SDA=input */
    for (int i = 7; i >= 0; i--) {
        i2c_scl_hi(); i2c_delay();
        if (i2c_sda_in()) val |= (1 << i);
        i2c_scl_lo(); i2c_delay();
    }
    /* Send ACK/NACK */
    i2c_write_reg(I2C_DIR);  /* back to output */
    if (ack) i2c_sda_hi(); else i2c_sda_lo();
    i2c_delay();
    i2c_scl_hi(); i2c_delay();
    i2c_scl_lo(); i2c_delay();
    return val;
}

/*
 * Read the 128-byte base EDID block from the display via DDC2B.
 * Returns 0 on success, -1 if no display responded (I2C NACK).
 * The raw EDID block is written to `edid_raw[128]`.
 */
int bga_read_edid(uint8_t edid_raw[EDID_BLOCK_SIZE]) {
    if (bga_lfb == 0 || !edid_raw) return -1;

    /* DDC2B: send start, I2C addr 0xA0 (write), EDID offset 0x00,
     * repeated start, I2C addr 0xA1 (read), read 128 bytes, stop. */
    i2c_start();
    if (i2c_send_byte(0xA0) != 0) { i2c_stop(); return -1; }  /* display addr write */
    if (i2c_send_byte(0x00) != 0) { i2c_stop(); return -1; }  /* EDID offset 0 */

    i2c_start();  /* repeated start */
    if (i2c_send_byte(0xA1) != 0) { i2c_stop(); return -1; }  /* display addr read */

    for (int i = 0; i < EDID_BLOCK_SIZE; i++) {
        edid_raw[i] = i2c_recv_byte(i == EDID_BLOCK_SIZE - 1 ? 1 : 0);
    }

    i2c_stop();
    return 0;
}
