/* VibeOS arm64 — virtio-net legacy MMIO driver.
 *
 * This is the Ethernet-frame shim used by the shared IPv4 stack in net.c.
 * QEMU virt exposes it with:
 *   -netdev user,id=net0 -device virtio-net-device,netdev=net0
 */
#include "arch.h"
#include "virtio_net.h"
#include "../../include/serial.h"
#include "../../include/string.h"

#define VL_MAGIC        0x000
#define VL_VERSION      0x004
#define VL_DEVICE_ID    0x008
#define VL_HOST_FEAT    0x010
#define VL_GUEST_FEAT   0x020
#define VL_GUEST_PGS    0x028
#define VL_QUEUE_SEL    0x030
#define VL_QUEUE_NMAX   0x034
#define VL_QUEUE_NUM    0x038
#define VL_QUEUE_ALIGN  0x03C
#define VL_QUEUE_PFN    0x040
#define VL_QUEUE_NOTIFY 0x050
#define VL_IRQ_STATUS   0x060
#define VL_IRQ_ACK      0x064
#define VL_STATUS       0x070
#define VL_CONFIG       0x100

#define VTSTAT_ACK       1
#define VTSTAT_DRIVER    2
#define VTSTAT_DRIVER_OK 4

#define VIRTIO_ID_NET 1
#define VIRTIO_NET_F_MAC 5

#define VDESC_F_NEXT  1
#define VDESC_F_WRITE 2

#define PAGE_SIZE 4096
#define RX_QSZ 32
#define TX_QSZ 8
#define NET_HDR_LEN 10
#define RX_BUF_BYTES 2048
#define TX_BUF_BYTES 2048

struct vq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct vq_used_elem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed));

struct rx_avail { uint16_t flags, idx, ring[RX_QSZ]; } __attribute__((packed));
struct rx_used  { uint16_t flags, idx; struct vq_used_elem ring[RX_QSZ]; } __attribute__((packed));
struct tx_avail { uint16_t flags, idx, ring[TX_QSZ]; } __attribute__((packed));
struct tx_used  { uint16_t flags, idx; struct vq_used_elem ring[TX_QSZ]; } __attribute__((packed));

static uint8_t g_rx_q[2 * PAGE_SIZE] __attribute__((aligned(PAGE_SIZE)));
static uint8_t g_tx_q[2 * PAGE_SIZE] __attribute__((aligned(PAGE_SIZE)));

#define RX_DESC  ((struct vq_desc *)g_rx_q)
#define RX_AVAIL ((struct rx_avail *)(g_rx_q + RX_QSZ * sizeof(struct vq_desc)))
#define RX_USED  ((struct rx_used  *)(g_rx_q + PAGE_SIZE))
#define TX_DESC  ((struct vq_desc *)g_tx_q)
#define TX_AVAIL ((struct tx_avail *)(g_tx_q + TX_QSZ * sizeof(struct vq_desc)))
#define TX_USED  ((struct tx_used  *)(g_tx_q + PAGE_SIZE))

static uint8_t g_rx_buf[RX_QSZ][RX_BUF_BYTES] __attribute__((aligned(64)));
static uint8_t g_tx_hdr[NET_HDR_LEN] __attribute__((aligned(64)));
static uint8_t g_tx_buf[TX_BUF_BYTES] __attribute__((aligned(64)));

static uintptr_t g_base;
static int g_ready;
static uint8_t g_mac[6];
static uint16_t g_rx_last_used;
static uint16_t g_rx_avail_idx;
static uint16_t g_tx_last_used;
static uint16_t g_tx_avail_idx;

static inline void vtw(uint32_t reg, uint32_t val) { mmio_write32(g_base + reg, val); }
static inline uint32_t vtr(uint32_t reg) { return mmio_read32(g_base + reg); }

#define VIRTIO_MMIO_BASE   0x0a000000UL
#define VIRTIO_MMIO_STRIDE 0x200
#define VIRTIO_MMIO_COUNT  32

static uintptr_t find_net(void) {
    for (int i = 0; i < VIRTIO_MMIO_COUNT; i++) {
        uintptr_t b = VIRTIO_MMIO_BASE + (uintptr_t)i * VIRTIO_MMIO_STRIDE;
        if (mmio_read32(b + VL_MAGIC) != 0x74726976) continue;
        if (mmio_read32(b + VL_DEVICE_ID) != VIRTIO_ID_NET) continue;
        return b;
    }
    return 0;
}

static void setup_queue(int sel, void *qmem, uint32_t qsz) {
    vtw(VL_QUEUE_SEL, (uint32_t)sel);
    uint32_t qmax = vtr(VL_QUEUE_NMAX);
    if (qmax < qsz) {
        serial_write("[virtio-net] queue too small\r\n");
        qsz = qmax;
    }
    vtw(VL_QUEUE_NUM, qsz);
    vtw(VL_QUEUE_ALIGN, PAGE_SIZE);
    vtw(VL_QUEUE_PFN, (uint32_t)((uint64_t)(uintptr_t)qmem / PAGE_SIZE));
}

static void rx_repost(uint16_t id) {
    RX_DESC[id].addr = (uint64_t)(uintptr_t)g_rx_buf[id];
    RX_DESC[id].len = RX_BUF_BYTES;
    RX_DESC[id].flags = VDESC_F_WRITE;
    RX_DESC[id].next = 0;
    RX_AVAIL->ring[g_rx_avail_idx % RX_QSZ] = id;
    __asm__ volatile("dsb sy" ::: "memory");
    g_rx_avail_idx++;
    RX_AVAIL->idx = g_rx_avail_idx;
    __asm__ volatile("dsb sy" ::: "memory");
    vtw(VL_QUEUE_NOTIFY, 0);
}

int virtio_net_init(void) {
    uintptr_t base = find_net();
    if (!base) {
        serial_write("[virtio-net] not found\r\n");
        return -1;
    }
    g_base = base;
    uint32_t ver = vtr(VL_VERSION);
    if (ver != 1) {
        serial_write("[virtio-net] unsupported version\r\n");
        return -1;
    }

    vtw(VL_STATUS, 0);
    vtw(VL_STATUS, VTSTAT_ACK);
    vtw(VL_STATUS, VTSTAT_ACK | VTSTAT_DRIVER);
    uint32_t host = vtr(VL_HOST_FEAT);
    uint32_t guest = (host & (1u << VIRTIO_NET_F_MAC));
    vtw(VL_GUEST_FEAT, guest);
    vtw(VL_GUEST_PGS, PAGE_SIZE);

    memset(g_rx_q, 0, sizeof(g_rx_q));
    memset(g_tx_q, 0, sizeof(g_tx_q));
    setup_queue(0, g_rx_q, RX_QSZ);
    setup_queue(1, g_tx_q, TX_QSZ);

    if (guest & (1u << VIRTIO_NET_F_MAC)) {
        uint32_t m0 = vtr(VL_CONFIG + 0);
        uint32_t m1 = vtr(VL_CONFIG + 4);
        g_mac[0] = (uint8_t)(m0 & 0xffu);
        g_mac[1] = (uint8_t)((m0 >> 8) & 0xffu);
        g_mac[2] = (uint8_t)((m0 >> 16) & 0xffu);
        g_mac[3] = (uint8_t)((m0 >> 24) & 0xffu);
        g_mac[4] = (uint8_t)(m1 & 0xffu);
        g_mac[5] = (uint8_t)((m1 >> 8) & 0xffu);
    } else {
        const uint8_t fallback[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
        memcpy(g_mac, fallback, 6);
    }

    g_rx_last_used = 0;
    g_rx_avail_idx = 0;
    g_tx_last_used = 0;
    g_tx_avail_idx = 0;
    for (uint16_t i = 0; i < RX_QSZ; i++) rx_repost(i);

    vtw(VL_STATUS, VTSTAT_ACK | VTSTAT_DRIVER | VTSTAT_DRIVER_OK);
    g_ready = 1;

    serial_write("[virtio-net] ready mac=");
    for (int i = 0; i < 6; i++) {
        if (i) serial_write(":");
        serial_write_hex_u64(g_mac[i]);
    }
    serial_write("\r\n");
    return 0;
}

int virtio_net_ready(void) { return g_ready; }
const uint8_t *virtio_net_mac(void) { return g_mac; }

static void tx_reclaim(void) {
    __asm__ volatile("dsb sy" ::: "memory");
    if (TX_USED->idx != g_tx_last_used) {
        g_tx_last_used = TX_USED->idx;
        vtw(VL_IRQ_ACK, vtr(VL_IRQ_STATUS));
    }
}

int virtio_net_transmit(const void *frame, uint16_t length) {
    if (!g_ready || !frame || length == 0 || length > TX_BUF_BYTES) return -1;
    tx_reclaim();
    memset(g_tx_hdr, 0, sizeof(g_tx_hdr));
    memcpy(g_tx_buf, frame, length);

    TX_DESC[0].addr = (uint64_t)(uintptr_t)g_tx_hdr;
    TX_DESC[0].len = NET_HDR_LEN;
    TX_DESC[0].flags = VDESC_F_NEXT;
    TX_DESC[0].next = 1;
    TX_DESC[1].addr = (uint64_t)(uintptr_t)g_tx_buf;
    TX_DESC[1].len = length;
    TX_DESC[1].flags = 0;
    TX_DESC[1].next = 0;

    TX_AVAIL->ring[g_tx_avail_idx % TX_QSZ] = 0;
    __asm__ volatile("dsb sy" ::: "memory");
    g_tx_avail_idx++;
    TX_AVAIL->idx = g_tx_avail_idx;
    __asm__ volatile("dsb sy" ::: "memory");
    vtw(VL_QUEUE_NOTIFY, 1);
    return 0;
}

int virtio_net_poll_receive(void *out_buffer, uint16_t out_capacity) {
    if (!g_ready || !out_buffer || out_capacity == 0) return 0;
    __asm__ volatile("dsb sy" ::: "memory");
    if (RX_USED->idx == g_rx_last_used) return 0;

    struct vq_used_elem elem = RX_USED->ring[g_rx_last_used % RX_QSZ];
    g_rx_last_used++;
    vtw(VL_IRQ_ACK, vtr(VL_IRQ_STATUS));

    uint16_t id = (uint16_t)elem.id;
    uint32_t n = elem.len;
    if (id >= RX_QSZ) return 0;

    int copied = 0;
    if (n > NET_HDR_LEN) {
        n -= NET_HDR_LEN;
        if (n > out_capacity) n = out_capacity;
        memcpy(out_buffer, g_rx_buf[id] + NET_HDR_LEN, n);
        copied = (int)n;
    }
    rx_repost(id);
    return copied;
}
