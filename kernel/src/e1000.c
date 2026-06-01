#include "e1000.h"
#include "io.h"
#include "serial.h"
#include "string.h"

/* ---- PCI configuration space access ---- */
#define PCI_CONFIG_ADDRESS 0x0CF8u
#define PCI_CONFIG_DATA 0x0CFCu

static uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t address = 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) |
                       ((uint32_t)func << 8) | ((uint32_t)(offset & 0xfcu));
    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

static void pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value) {
    uint32_t address = 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) |
                       ((uint32_t)func << 8) | ((uint32_t)(offset & 0xfcu));
    outl(PCI_CONFIG_ADDRESS, address);
    outl(PCI_CONFIG_DATA, value);
}

/* ---- e1000 MMIO registers ---- */
#define E1000_CTRL 0x0000u
#define E1000_STATUS 0x0008u
#define E1000_EERD 0x0014u
#define E1000_ICR 0x00C0u
#define E1000_IMS 0x00D0u
#define E1000_IMC 0x00D8u
#define E1000_RCTL 0x0100u
#define E1000_TCTL 0x0400u
#define E1000_TIPG 0x0410u
#define E1000_RDBAL 0x2800u
#define E1000_RDBAH 0x2804u
#define E1000_RDLEN 0x2808u
#define E1000_RDH 0x2810u
#define E1000_RDT 0x2818u
#define E1000_TDBAL 0x3800u
#define E1000_TDBAH 0x3804u
#define E1000_TDLEN 0x3808u
#define E1000_TDH 0x3810u
#define E1000_TDT 0x3818u
#define E1000_MTA 0x5200u
#define E1000_RAL 0x5400u
#define E1000_RAH 0x5404u

/* RCTL bits */
#define RCTL_EN (1u << 1)
#define RCTL_SBP (1u << 2)
#define RCTL_UPE (1u << 3)
#define RCTL_MPE (1u << 4)
#define RCTL_BAM (1u << 15)
#define RCTL_SECRC (1u << 26)
#define RCTL_BSIZE_2048 0u /* (BSIZE=00, BSEX=0) */

/* TCTL bits */
#define TCTL_EN (1u << 1)
#define TCTL_PSP (1u << 3)
#define TCTL_CT_SHIFT 4
#define TCTL_COLD_SHIFT 12

/* RX descriptor status */
#define RX_STATUS_DD 0x01u
#define RX_STATUS_EOP 0x02u

/* TX descriptor cmd / status */
#define TX_CMD_EOP 0x01u
#define TX_CMD_IFCS 0x02u
#define TX_CMD_RS 0x08u
#define TX_STATUS_DD 0x01u

#define E1000_RX_DESC_COUNT 32u
#define E1000_TX_DESC_COUNT 32u
#define E1000_BUFFER_SIZE 2048u

struct e1000_rx_desc {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t status;
    uint8_t errors;
    uint16_t special;
} __attribute__((packed));

struct e1000_tx_desc {
    uint64_t addr;
    uint16_t length;
    uint8_t cso;
    uint8_t cmd;
    uint8_t status;
    uint8_t css;
    uint16_t special;
} __attribute__((packed));

/* DMA rings and packet buffers live in BSS, which is identity-mapped below
 * 0x2000000, so each buffer's virtual address equals its physical address. */
static struct e1000_rx_desc g_rx_ring[E1000_RX_DESC_COUNT] __attribute__((aligned(16)));
static struct e1000_tx_desc g_tx_ring[E1000_TX_DESC_COUNT] __attribute__((aligned(16)));
static uint8_t g_rx_buffers[E1000_RX_DESC_COUNT][E1000_BUFFER_SIZE] __attribute__((aligned(16)));
static uint8_t g_tx_buffers[E1000_TX_DESC_COUNT][E1000_BUFFER_SIZE] __attribute__((aligned(16)));

static void mmio_write32(struct e1000_device *dev, uint32_t reg, uint32_t value) {
    *(volatile uint32_t *)(dev->mmio + reg) = value;
}

static uint32_t mmio_read32(struct e1000_device *dev, uint32_t reg) {
    return *(volatile uint32_t *)(dev->mmio + reg);
}

static int pci_find_e1000(uint8_t *out_bus, uint8_t *out_slot, uint8_t *out_func, uintptr_t *out_bar0) {
    uint16_t bus;
    uint8_t slot;

    for (bus = 0; bus < 256u; ++bus) {
        for (slot = 0; slot < 32u; ++slot) {
            uint32_t id = pci_read32((uint8_t)bus, slot, 0u, 0x00u);
            uint16_t vendor = (uint16_t)(id & 0xffffu);
            uint16_t device = (uint16_t)((id >> 16) & 0xffffu);
            uint32_t bar0;

            if (vendor == 0xffffu) {
                continue;
            }
            if (vendor != E1000_VENDOR_ID) {
                continue;
            }
            /* Accept the common QEMU e1000 device IDs. */
            if (device != E1000_DEVICE_82540EM && device != 0x100Fu && device != 0x10D3u) {
                continue;
            }

            bar0 = pci_read32((uint8_t)bus, slot, 0u, 0x10u);
            *out_bus = (uint8_t)bus;
            *out_slot = slot;
            *out_func = 0u;
            *out_bar0 = (uintptr_t)(bar0 & ~0xfu);
            serial_write("E1000: found NIC vendor=8086 device=");
            serial_write_hex_u64(device);
            serial_write(" BAR0=");
            serial_write_hex_u64(*out_bar0);
            serial_write("\n");
            return 1;
        }
    }
    return 0;
}

static void e1000_read_mac(struct e1000_device *dev) {
    uint32_t ral = mmio_read32(dev, E1000_RAL);
    uint32_t rah = mmio_read32(dev, E1000_RAH);

    if (ral != 0u || (rah & 0xffffu) != 0u) {
        dev->mac[0] = (uint8_t)(ral & 0xffu);
        dev->mac[1] = (uint8_t)((ral >> 8) & 0xffu);
        dev->mac[2] = (uint8_t)((ral >> 16) & 0xffu);
        dev->mac[3] = (uint8_t)((ral >> 24) & 0xffu);
        dev->mac[4] = (uint8_t)(rah & 0xffu);
        dev->mac[5] = (uint8_t)((rah >> 8) & 0xffu);
        return;
    }

    /* Fall back to EEPROM if RAL/RAH were not pre-populated. */
    {
        int i;
        for (i = 0; i < 3; ++i) {
            uint32_t tmp;
            mmio_write32(dev, E1000_EERD, ((uint32_t)i << 8) | 0x1u);
            do {
                tmp = mmio_read32(dev, E1000_EERD);
            } while ((tmp & 0x10u) == 0u);
            dev->mac[i * 2] = (uint8_t)((tmp >> 16) & 0xffu);
            dev->mac[i * 2 + 1] = (uint8_t)((tmp >> 24) & 0xffu);
        }
    }
}

static void e1000_setup_rx(struct e1000_device *dev) {
    uint32_t i;

    for (i = 0; i < E1000_RX_DESC_COUNT; ++i) {
        g_rx_ring[i].addr = (uint64_t)(uintptr_t)g_rx_buffers[i];
        g_rx_ring[i].status = 0;
    }

    mmio_write32(dev, E1000_RDBAL, (uint32_t)((uintptr_t)g_rx_ring & 0xffffffffu));
    mmio_write32(dev, E1000_RDBAH, (uint32_t)(((uint64_t)(uintptr_t)g_rx_ring) >> 32));
    mmio_write32(dev, E1000_RDLEN, E1000_RX_DESC_COUNT * sizeof(struct e1000_rx_desc));
    mmio_write32(dev, E1000_RDH, 0);
    mmio_write32(dev, E1000_RDT, E1000_RX_DESC_COUNT - 1u);
    dev->rx_cur = 0;

    mmio_write32(dev, E1000_RCTL,
                 RCTL_EN | RCTL_BAM | RCTL_SECRC | RCTL_BSIZE_2048);
}

static void e1000_setup_tx(struct e1000_device *dev) {
    uint32_t i;

    for (i = 0; i < E1000_TX_DESC_COUNT; ++i) {
        g_tx_ring[i].addr = (uint64_t)(uintptr_t)g_tx_buffers[i];
        g_tx_ring[i].status = TX_STATUS_DD; /* mark free */
        g_tx_ring[i].cmd = 0;
    }

    mmio_write32(dev, E1000_TDBAL, (uint32_t)((uintptr_t)g_tx_ring & 0xffffffffu));
    mmio_write32(dev, E1000_TDBAH, (uint32_t)(((uint64_t)(uintptr_t)g_tx_ring) >> 32));
    mmio_write32(dev, E1000_TDLEN, E1000_TX_DESC_COUNT * sizeof(struct e1000_tx_desc));
    mmio_write32(dev, E1000_TDH, 0);
    mmio_write32(dev, E1000_TDT, 0);
    dev->tx_cur = 0;

    mmio_write32(dev, E1000_TCTL,
                 TCTL_EN | TCTL_PSP | (0x10u << TCTL_CT_SHIFT) | (0x40u << TCTL_COLD_SHIFT));
    mmio_write32(dev, E1000_TIPG, 0x0060200Au);
}

int e1000_init(struct e1000_device *dev) {
    uint8_t bus;
    uint8_t slot;
    uint8_t func;
    uintptr_t bar0;
    uint32_t command;
    uint32_t i;

    memset(dev, 0, sizeof(*dev));

    if (!pci_find_e1000(&bus, &slot, &func, &bar0)) {
        serial_write("E1000: no NIC found on PCI bus\n");
        return 0;
    }

    /* Enable memory space + bus mastering so the card can DMA. */
    command = pci_read32(bus, slot, func, 0x04u);
    command |= (1u << 1) | (1u << 2);
    pci_write32(bus, slot, func, 0x04u, command);

    dev->mmio = (volatile uint8_t *)bar0;
    dev->present = 1;

    /* Disable interrupts (we poll). */
    mmio_write32(dev, E1000_IMC, 0xffffffffu);
    mmio_read32(dev, E1000_ICR);

    /* Clear the multicast table array. */
    for (i = 0; i < 128u; ++i) {
        mmio_write32(dev, E1000_MTA + (i * 4u), 0);
    }

    e1000_read_mac(dev);
    serial_write("E1000: MAC=");
    for (i = 0; i < 6u; ++i) {
        serial_write_hex_u8(dev->mac[i]);
        if (i < 5u) serial_write(":");
    }
    serial_write("\n");

    e1000_setup_rx(dev);
    e1000_setup_tx(dev);

    serial_write("E1000: link status=");
    serial_write_hex_u64(mmio_read32(dev, E1000_STATUS));
    serial_write("\n");

    return 1;
}

int e1000_transmit(struct e1000_device *dev, const void *frame, uint16_t length) {
    uint32_t tail;
    struct e1000_tx_desc *desc;

    if (!dev->present || frame == 0 || length == 0) {
        return -1;
    }
    if (length > E1000_BUFFER_SIZE) {
        length = E1000_BUFFER_SIZE;
    }

    tail = dev->tx_cur;
    desc = &g_tx_ring[tail];

    /* Wait until this descriptor has been consumed by the card. */
    if ((desc->status & TX_STATUS_DD) == 0u) {
        return -2; /* ring full */
    }

    memcpy(g_tx_buffers[tail], frame, length);
    desc->addr = (uint64_t)(uintptr_t)g_tx_buffers[tail];
    desc->length = length;
    desc->cso = 0;
    desc->css = 0;
    desc->special = 0;
    desc->cmd = TX_CMD_EOP | TX_CMD_IFCS | TX_CMD_RS;
    desc->status = 0;

    dev->tx_cur = (tail + 1u) % E1000_TX_DESC_COUNT;
    mmio_write32(dev, E1000_TDT, dev->tx_cur);
    return 0;
}

int e1000_poll_receive(struct e1000_device *dev, void *out_buffer, uint16_t out_capacity) {
    uint32_t cur;
    struct e1000_rx_desc *desc;
    uint16_t len;

    if (!dev->present) {
        return -1;
    }

    cur = dev->rx_cur;
    desc = &g_rx_ring[cur];

    if ((desc->status & RX_STATUS_DD) == 0u) {
        return 0; /* nothing received */
    }

    len = desc->length;
    if (len > out_capacity) {
        len = out_capacity;
    }
    memcpy(out_buffer, g_rx_buffers[cur], len);

    /* Hand the descriptor back to the card and advance the tail. */
    desc->status = 0;
    mmio_write32(dev, E1000_RDT, cur);
    dev->rx_cur = (cur + 1u) % E1000_RX_DESC_COUNT;

    return (int)len;
}
