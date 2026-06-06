/* VibeOS arm64 — Virtio-blk driver (legacy/v1 MMIO transport).
 *
 * QEMU's virtio-blk-device uses the legacy (v1) MMIO transport by default.
 * In legacy mode the queue is set up via a single page-aligned guest-physical
 * address (GFN-based), not split into desc/avail/used pointers as in v2.
 *
 * Legacy MMIO register map (base + offset):
 *   0x000  MagicValue    "virt" = 0x74726976
 *   0x004  Version       1 (legacy)
 *   0x008  DeviceID      2 = block
 *   0x010  HostFeatures  (read)
 *   0x020  GuestFeatures (write)
 *   0x028  GuestPageSize (write: page size the driver uses, e.g. 4096)
 *   0x030  QueueSel      (write: select queue 0)
 *   0x034  QueueNumMax   (read: max queue size)
 *   0x038  QueueNum      (write: actual queue size to use)
 *   0x03C  QueueAlign    (write: queue alignment, e.g. 4096)
 *   0x040  QueuePFN      (write: queue base page-frame-number)
 *   0x050  QueueNotify   (write: kick queue N)
 *   0x060  InterruptStatus
 *   0x064  InterruptACK
 *   0x070  Status
 *   0x100  Config        (device-specific: capacity in 512-byte sectors)
 *
 * Legacy virtqueue layout (all in one physically-contiguous, page-aligned buf):
 *   [ descriptor table ] [ avail ring ] [ padding to QueueAlign ] [ used ring ]
 */
#include "arch.h"
#include "../../include/serial.h"
#include "../../include/ramdisk.h"
#include "../../include/alloc.h"
#include "../../include/string.h"

/* ---- Virtio-mmio register offsets (legacy) ---------------------------- */
#define VL_MAGIC        0x000
#define VL_VERSION      0x004
#define VL_DEVICE_ID    0x008
#define VL_HOST_FEAT    0x010
#define VL_GUEST_FEAT   0x020
#define VL_GUEST_PGS    0x028   /* guest page size */
#define VL_QUEUE_SEL    0x030
#define VL_QUEUE_NMAX   0x034
#define VL_QUEUE_NUM    0x038
#define VL_QUEUE_ALIGN  0x03C
#define VL_QUEUE_PFN    0x040   /* page-frame-number of queue */
#define VL_QUEUE_NOTIFY 0x050
#define VL_IRQ_STATUS   0x060
#define VL_IRQ_ACK      0x064
#define VL_STATUS       0x070
#define VL_CONFIG       0x100   /* device config space */

#define VTSTAT_ACK      1
#define VTSTAT_DRIVER   2
#define VTSTAT_DRIVER_OK 4
#define VTSTAT_FAILED   128

/* ---- Virtio-blk request types ---------------------------------------- */
#define VTBLK_T_IN  0
#define VTBLK_T_OUT 1

/* ---- Virtqueue structures --------------------------------------------- */
#define VQSIZE 16   /* keep small; power of 2 */

#define VDESC_F_NEXT  1
#define VDESC_F_WRITE 2

struct vq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct vq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[VQSIZE];
} __attribute__((packed));

struct vq_used_elem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed));

struct vq_used {
    uint16_t flags;
    uint16_t idx;
    struct vq_used_elem ring[VQSIZE];
} __attribute__((packed));

/* Virtio-blk request header */
struct vtblk_hdr {
    uint32_t type;
    uint32_t ioprio;
    uint64_t sector;
} __attribute__((packed));

/* ---- Per-device state (one global instance) --------------------------- */
#define PAGE_SIZE 4096

/* Legacy virtqueue: desc + avail must fit in first half-page,
 * used ring in the second (aligned to QueueAlign=4096).
 * For VQSIZE=16:
 *   desc  = 16 × 16 = 256 bytes
 *   avail = 2+2+16×2 = 36 bytes   → fits in 4096-byte first half
 *   used  = 2+2+16×8 = 132 bytes  → must start at next 4096-byte boundary
 */
static uint8_t g_queue_mem[2 * PAGE_SIZE] __attribute__((aligned(PAGE_SIZE)));

/* Overlay the structures on g_queue_mem */
#define G_DESC   ((struct vq_desc  *)g_queue_mem)
#define G_AVAIL  ((struct vq_avail *)(g_queue_mem + VQSIZE * sizeof(struct vq_desc)))
#define G_USED   ((struct vq_used  *)(g_queue_mem + PAGE_SIZE))

/* Request buffers — must be DMA-safe (in normal RAM) */
static struct vtblk_hdr g_req_hdr  __attribute__((aligned(16)));
static uint8_t          g_req_data [4096] __attribute__((aligned(512)));
static uint8_t          g_req_stat [16]   __attribute__((aligned(16)));

static uintptr_t g_base    = 0;
static uint64_t  g_cap_sec = 0;   /* capacity in 512-byte sectors */
static uint16_t  g_last_used = 0;
static int       g_ready   = 0;

static inline void vtw(uint32_t reg, uint32_t val) {
    mmio_write32(g_base + reg, val);
}
static inline uint32_t vtr(uint32_t reg) {
    return mmio_read32(g_base + reg);
}

/* ---- Find the virtio-blk device --------------------------------------- */
#define VIRTIO_MMIO_BASE   0x0a000000UL
#define VIRTIO_MMIO_STRIDE 0x200
#define VIRTIO_MMIO_COUNT  32

static uintptr_t find_blk(void) {
    for (int i = 0; i < VIRTIO_MMIO_COUNT; i++) {
        uintptr_t b = VIRTIO_MMIO_BASE + (uintptr_t)i * VIRTIO_MMIO_STRIDE;
        if (mmio_read32(b + VL_MAGIC)     != 0x74726976) continue;
        if (mmio_read32(b + VL_DEVICE_ID) != 2)          continue;
        return b;
    }
    return 0;
}

/* ---- Initialise ------------------------------------------------------- */
int virtio_blk_init(void) {
    uintptr_t base = find_blk();
    if (!base) { serial_write("[virtio-blk] not found\r\n"); return -1; }

    uint32_t ver = mmio_read32(base + VL_VERSION);
    serial_write("[virtio-blk] found at ");
    serial_write_hex_u64(base);
    serial_write(" version="); serial_write_hex_u64(ver); serial_write("\r\n");

    if (ver != 1) { serial_write("[virtio-blk] unsupported version\r\n"); return -1; }

    g_base = base;

    /* 1. Reset */
    vtw(VL_STATUS, 0);
    /* 2. Acknowledge */
    vtw(VL_STATUS, VTSTAT_ACK);
    /* 3. Driver */
    vtw(VL_STATUS, VTSTAT_ACK | VTSTAT_DRIVER);
    /* 4. Accept features (none required) */
    vtw(VL_GUEST_FEAT, 0);
    /* 5. Guest page size */
    vtw(VL_GUEST_PGS, PAGE_SIZE);
    /* 6. Set up queue 0 */
    vtw(VL_QUEUE_SEL, 0);
    uint32_t qmax = vtr(VL_QUEUE_NMAX);
    uint32_t qnum = (qmax >= VQSIZE) ? VQSIZE : qmax;
    vtw(VL_QUEUE_NUM,   qnum);
    vtw(VL_QUEUE_ALIGN, PAGE_SIZE);
    /* Queue PFN = physical page frame of our queue buffer */
    uint64_t qpa = (uint64_t)(uintptr_t)g_queue_mem;
    vtw(VL_QUEUE_PFN, (uint32_t)(qpa / PAGE_SIZE));
    /* 7. Driver OK */
    vtw(VL_STATUS, VTSTAT_ACK | VTSTAT_DRIVER | VTSTAT_DRIVER_OK);

    /* Read capacity (two 32-bit words at config+0 and config+4) */
    uint32_t cap_lo = vtr(VL_CONFIG + 0);
    uint32_t cap_hi = vtr(VL_CONFIG + 4);
    g_cap_sec = ((uint64_t)cap_hi << 32) | cap_lo;

    /* Clear queue memory */
    memset(g_queue_mem, 0, sizeof(g_queue_mem));
    g_last_used = 0;
    g_ready = 1;

    serial_write("[virtio-blk] ready, capacity=");
    serial_write_hex_u64(g_cap_sec);
    serial_write(" sectors (");
    {
        uint64_t mb = (g_cap_sec / 2048); /* sectors / 2048 = MiB */
        char buf[16]; int i = 0;
        if (!mb) buf[i++]='0';
        else { uint64_t t=mb; while(t){buf[i++]=(char)('0'+t%10);t/=10;} }
        while(i--) serial_write_char(buf[i]);
    }
    serial_write(" MB)\r\n");
    return 0;
}

/* ---- Submit a single request and poll completion ---------------------- */
static int do_request(uint32_t type, uint64_t sector, void *buf, size_t len) {
    if (!g_ready) return -1;

    /* Build request header */
    g_req_hdr.type   = type;
    g_req_hdr.ioprio = 0;
    g_req_hdr.sector = sector;
    g_req_stat[0]    = 0xFF;   /* pending */

    /* Descriptor chain: [0]=header [1]=data [2]=status */
    uint16_t d0 = 0, d1 = 1, d2 = 2;

    G_DESC[d0].addr  = (uint64_t)(uintptr_t)&g_req_hdr;
    G_DESC[d0].len   = sizeof(g_req_hdr);
    G_DESC[d0].flags = VDESC_F_NEXT;
    G_DESC[d0].next  = d1;

    G_DESC[d1].addr  = (uint64_t)(uintptr_t)buf;
    G_DESC[d1].len   = (uint32_t)len;
    G_DESC[d1].flags = VDESC_F_NEXT | (type == VTBLK_T_IN ? VDESC_F_WRITE : 0);
    G_DESC[d1].next  = d2;

    G_DESC[d2].addr  = (uint64_t)(uintptr_t)g_req_stat;
    G_DESC[d2].len   = 1;
    G_DESC[d2].flags = VDESC_F_WRITE;
    G_DESC[d2].next  = 0;

    /* Publish to avail ring */
    uint16_t slot = G_AVAIL->idx % VQSIZE;
    G_AVAIL->ring[slot] = d0;
    __asm__ volatile("dsb sy" ::: "memory");
    G_AVAIL->idx++;
    __asm__ volatile("dsb sy" ::: "memory");

    /* Kick */
    vtw(VL_QUEUE_NOTIFY, 0);

    /* Poll used ring */
    uint32_t spins = 0;
    while (G_USED->idx == g_last_used) {
        __asm__ volatile("dsb sy; yield" ::: "memory");
        if (++spins > 5000000) {
            serial_write("[virtio-blk] timeout\r\n");
            return -1;
        }
    }
    g_last_used = G_USED->idx;

    return (g_req_stat[0] == 0) ? 0 : -1;
}

/* ---- ramdisk_device callbacks ----------------------------------------- */
static int vblk_read(void *ctx, uint64_t block_num, void *buffer, size_t count) {
    (void)ctx;
    /* ext2 calls with block_size=1024, block_num = byte_offset/block_size
     * so the disk byte offset = block_num * count. sector = offset / 512. */
    uint64_t byte_off = block_num * (uint64_t)count;
    uint64_t sector   = byte_off / 512;
    /* Virtio transfers in 512-byte multiples */
    if (count > sizeof(g_req_data)) count = sizeof(g_req_data);
    int r = do_request(VTBLK_T_IN, sector, g_req_data, count);
    if (r == 0) memcpy(buffer, g_req_data, count);
    return r;
}

static int vblk_write(void *ctx, uint64_t block_num, const void *buffer,
                       size_t count) {
    (void)ctx;
    if (count > sizeof(g_req_data)) count = sizeof(g_req_data);
    memcpy(g_req_data, buffer, count);
    uint64_t byte_off = block_num * (uint64_t)count;
    uint64_t sector   = byte_off / 512;
    return do_request(VTBLK_T_OUT, sector, g_req_data, count);
}

int virtio_blk_get_device(struct ramdisk_device *dev) {
    if (!g_ready) return -1;
    dev->data       = 0;
    dev->size       = g_cap_sec * 512;
    dev->block_size  = 1024;
    dev->block_count = dev->size / 1024;
    dev->read_fn    = vblk_read;
    dev->write_fn   = vblk_write;
    dev->io_context = 0;
    return 0;
}
