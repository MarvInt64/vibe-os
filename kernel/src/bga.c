#include "bga.h"
#include "io.h"
#include "serial.h"

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
    out_fb->address = lfb_address;
    out_fb->width = width;
    out_fb->height = height;
    out_fb->pitch = width * (bpp / 8u);
    out_fb->bpp = (uint8_t)bpp;
    return 1;
}
