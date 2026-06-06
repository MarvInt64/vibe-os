/* VibeOS arm64 — Virtio-input driver (legacy/v1 MMIO transport).
 *
 * QEMU virtio-tablet-pci / virtio-mouse-pci use device ID 18.
 * Events arrive via virtqueue: the device writes input_event structs into
 * driver-provided buffers and the driver polls the used ring.
 *
 * Legacy MMIO register map (base + offset):
 *   0x000  MagicValue    "virt" = 0x74726976
 *   0x004  Version       1 (legacy)
 *   0x008  DeviceID      18 = input
 *   0x010  HostFeatures  (read)
 *   0x020  GuestFeatures (write)
 *   0x028  GuestPageSize (write)
 *   0x030  QueueSel      (write: select queue 0)
 *   0x034  QueueNumMax   (read)
 *   0x038  QueueNum      (write)
 *   0x03C  QueueAlign    (write)
 *   0x040  QueuePFN      (write: queue base page-frame-number)
 *   0x050  QueueNotify   (not used for input — device pushes events)
 *   0x060  InterruptStatus
 *   0x064  InterruptACK
 *   0x070  Status
 *   0x100  Config        (device-specific config space)
 *
 * Legacy virtqueue layout (all in one page-aligned buffer):
 *   [ descriptor table ] [ avail ring ] [ padding ] [ used ring ]
 *
 * Each event is 8 bytes:
 *   struct virtio_input_event { uint16_t type, code; uint32_t value; };
 *
 * We allocate a ring of 32 event buffers.  The device writes batches
 * (one or more EV_ABS/EV_KEY events followed by EV_SYN).  The driver
 * polls the used ring, copies events out, and returns buffers to the
 * avail ring so the device can reuse them.
 */
#include "arch.h"
#include "../../include/serial.h"
#include "../../include/alloc.h"
#include "../../include/string.h"

/* ---- Virtio-mmio register offsets (legacy, same as virtio_blk) -------- */
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

#define VTSTAT_ACK      1
#define VTSTAT_DRIVER   2
#define VTSTAT_DRIVER_OK 4

/* ---- Virtqueue structures (same layout as virtio_blk) ----------------- */
#define VQSIZE 32   /* power of 2, room for many events per poll */

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

/* ---- Virtio-input event (8 bytes, per spec) --------------------------- */
struct vi_event {
    uint16_t type;    /* EV_SYN=0, EV_KEY=1, EV_REL=2, EV_ABS=3 */
    uint16_t code;
    uint32_t value;
} __attribute__((packed));

/* ---- Per-device state ------------------------------------------------- */
#define PAGE_SIZE 4096

/* Queue memory: desc + avail in first page, used ring in second page.
 * For VQSIZE=32:
 *   desc  = 32 x 16 = 512 bytes
 *   avail = 2+2+32x2 = 68 bytes  -> fits in first page
 *   used  = 2+2+32x8 = 260 bytes -> starts at second page boundary
 */
static uint8_t g_qmem[2 * PAGE_SIZE] __attribute__((aligned(PAGE_SIZE)));

#define G_DESC   ((struct vq_desc  *)g_qmem)
#define G_AVAIL  ((struct vq_avail *)(g_qmem + VQSIZE * sizeof(struct vq_desc)))
#define G_USED   ((struct vq_used  *)(g_qmem + PAGE_SIZE))

/* Event buffers — one per virtqueue slot. */
static uint8_t g_evt_raw[VQSIZE * 64] __attribute__((aligned(64)));
#define G_EVT(n) ((struct vi_event *)(g_evt_raw + (n) * 64))

/* Parsed mouse state — updated by virtio_input_poll(). */
int g_mouse_x       = 0;
int g_mouse_y       = 0;
int g_mouse_buttons = 0;   /* bit 0 = left, bit 1 = right, bit 2 = middle */
int g_mouse_moved   = 0;   /* set to 1 when new data arrives, cleared by reader */

static uintptr_t g_base       = 0;
static int       g_ready      = 0;
static uint16_t  g_last_used  = 0;

static inline void vlw(uint32_t reg, uint32_t val) {
    mmio_write32(g_base + reg, val);
}
static inline uint32_t vlr(uint32_t reg) {
    return mmio_read32(g_base + reg);
}

/* ---- Find the virtio-input device ------------------------------------- */
#define VIRTIO_MMIO_BASE   0x0a000000UL
#define VIRTIO_MMIO_STRIDE 0x200
#define VIRTIO_MMIO_COUNT  32

static uintptr_t find_input(void) {
    for (int i = 0; i < VIRTIO_MMIO_COUNT; i++) {
        uintptr_t b = VIRTIO_MMIO_BASE + (uintptr_t)i * VIRTIO_MMIO_STRIDE;
        if (mmio_read32(b + VL_MAGIC)     != 0x74726976) continue;
        if (mmio_read32(b + VL_DEVICE_ID) != 18)         continue;
        return b;
    }
    return 0;
}

/* ---- Initialise ------------------------------------------------------- */
int virtio_input_init(void) {
    uintptr_t base = find_input();
    if (!base) {
        serial_write("[virtio-input] not found\r\n");
        return -1;
    }
    serial_write("[virtio-input] found at ");
    serial_write_hex_u64(base);
    serial_write("\r\n");

    g_base = base;

    /* 1. Reset */
    vlw(VL_STATUS, 0);
    /* 2. Acknowledge */
    vlw(VL_STATUS, VTSTAT_ACK);
    /* 3. Driver */
    vlw(VL_STATUS, VTSTAT_ACK | VTSTAT_DRIVER);
    /* 4. Accept features (none required) */
    vlw(VL_GUEST_FEAT, 0);
    /* 5. Guest page size */
    vlw(VL_GUEST_PGS, PAGE_SIZE);
    /* 6. Set up queue 0 (event queue) */
    vlw(VL_QUEUE_SEL, 0);
    uint32_t qmax = vlr(VL_QUEUE_NMAX);
    uint32_t qnum = (qmax >= VQSIZE) ? VQSIZE : qmax;
    vlw(VL_QUEUE_NUM,   qnum);
    vlw(VL_QUEUE_ALIGN, PAGE_SIZE);
    uint64_t qpa = (uint64_t)(uintptr_t)g_qmem;
    vlw(VL_QUEUE_PFN, (uint32_t)(qpa / PAGE_SIZE));
    /* 7. Driver OK */
    vlw(VL_STATUS, VTSTAT_ACK | VTSTAT_DRIVER | VTSTAT_DRIVER_OK);

    /* Clear queue memory and pre-fill the avail ring with empty buffers
     * so the device can write events into them. */
    memset(g_qmem, 0, sizeof(g_qmem));
    g_last_used = 0;

    for (int i = 0; i < (int)qnum; i++) {
        G_DESC[i].addr  = (uint64_t)(uintptr_t)G_EVT(i);
        G_DESC[i].len   = sizeof(struct vi_event);
        G_DESC[i].flags = VDESC_F_WRITE;  /* device writes into this buffer */
        G_DESC[i].next  = 0;

        G_AVAIL->ring[i] = (uint16_t)i;
    }
    G_AVAIL->idx = (uint16_t)qnum;
    __asm__ volatile("dsb sy" ::: "memory");

    g_ready = 1;
    serial_write("[virtio-input] ready\r\n");
    return 0;
}

/* ---- Poll the used ring for new events -------------------------------- */
void virtio_input_poll(void) {
    if (!g_ready) return;

    int mx = g_mouse_x, my = g_mouse_y, mb = g_mouse_buttons;
    int any = 0;

    /* Barrier: read used->idx after device writes. */
    __asm__ volatile("dsb sy" ::: "memory");
    uint16_t used_idx = G_USED->idx;

    while (g_last_used != used_idx) {
        uint16_t slot = g_last_used % VQSIZE;
        uint32_t desc_id = G_USED->ring[slot].id;
        struct vi_event *ev = G_EVT(desc_id);

        switch (ev->type) {
        case 3:   /* EV_ABS */
            if (ev->code == 0x00)       mx = (int)ev->value;      /* ABS_X */
            else if (ev->code == 0x01)  my = (int)ev->value;      /* ABS_Y */
            break;
        case 1:   /* EV_KEY */
            if (ev->code == 0x110) {
                if (ev->value) mb |= 1;   else mb &= ~1;
            } else if (ev->code == 0x111) {
                if (ev->value) mb |= 2;   else mb &= ~2;
            } else if (ev->code == 0x112) {
                if (ev->value) mb |= 4;   else mb &= ~4;
            }
            break;
        case 0:   /* EV_SYN — frame boundary, apply accumulated state */
            if (mx != g_mouse_x || my != g_mouse_y || mb != g_mouse_buttons) {
                g_mouse_x = mx;
                g_mouse_y = my;
                g_mouse_buttons = mb;
                g_mouse_moved = 1;
                any = 1;
            }
            break;
        default:
            break;
        }

        /* Recycle: put descriptor back in avail ring */
        G_AVAIL->ring[G_AVAIL->idx % VQSIZE] = desc_id;
        G_AVAIL->idx++;
        __asm__ volatile("dsb sy" ::: "memory");

        g_last_used++;
    }

    static int first = 1;
    if (any && first) {
        serial_write("[virtio-input] first event at (");
        { char b[16]; int i=0, t=g_mouse_x; if(!t)b[i++]='0';
          else while(t){b[i++]=(char)('0'+t%10);t/=10;}
          while(i--) serial_write_char(b[i]); }
        serial_write(",");
        { char b[16]; int i=0, t=g_mouse_y; if(!t)b[i++]='0';
          else while(t){b[i++]=(char)('0'+t%10);t/=10;}
          while(i--) serial_write_char(b[i]); }
        serial_write(")\r\n");
        first = 0;
    }
}

int virtio_input_is_ready(void) { return g_ready; }
