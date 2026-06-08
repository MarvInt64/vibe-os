/* VibeOS arm64 — Virtio-input driver (legacy/v1 MMIO transport).
 *
 * Handles both virtio-tablet-device (mouse) and virtio-keyboard-device.
 * Each device gets its own virtqueue; we poll both and merge events.
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
 *   0x050  QueueNotify   (not used for input)
 *   0x060  InterruptStatus
 *   0x064  InterruptACK
 *   0x070  Status
 *   0x100  Config        (device-specific config space)
 */
#include "arch.h"
#include "../../include/serial.h"
#include "../../include/alloc.h"
#include "../../include/string.h"

/* ---- Virtio-mmio register offsets (legacy) ----------------------------- */
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

/* ---- Virtqueue structures ---------------------------------------------- */
#define VQSIZE 32
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

/* ---- Virtio-input event (8 bytes) ------------------------------------- */
struct vi_event {
    uint16_t type;    /* EV_SYN=0, EV_KEY=1, EV_REL=2, EV_ABS=3 */
    uint16_t code;
    uint32_t value;
} __attribute__((packed));

/* ---- Per-device state -------------------------------------------------- */
#define PAGE_SIZE 4096
#define MAX_INPUT_DEVS 2

struct vi_device {
    uintptr_t base;
    int       ready;
    uint16_t  last_used;
    uint8_t  *qmem;       /* 2 * PAGE_SIZE, page-aligned */
    uint8_t  *evt_raw;    /* VQSIZE * 64, 64-byte aligned */
};

#define DEV_DESC(d)  ((struct vq_desc  *)(d)->qmem)
#define DEV_AVAIL(d) ((struct vq_avail *)((d)->qmem + VQSIZE * sizeof(struct vq_desc)))
#define DEV_USED(d)  ((struct vq_used  *)((d)->qmem + PAGE_SIZE))
#define DEV_EVT(d,n) ((struct vi_event *)((d)->evt_raw + (n) * 64))

static struct vi_device g_devs[MAX_INPUT_DEVS];
static int g_num_devs = 0;

/* ---- Parsed state — updated by virtio_input_poll() -------------------- */
int g_mouse_x       = 0;
int g_mouse_y       = 0;
int g_mouse_buttons = 0;   /* bit 0 = left, bit 1 = right, bit 2 = middle */
int g_mouse_moved   = 0;

/* Keyboard ring buffer — filled by virtio_input_poll, drained by compositor */
#define KBD_BUF_SIZE 64
static char  g_kbd_buf[KBD_BUF_SIZE];
static int   g_kbd_head = 0;   /* next write position */
static int   g_kbd_tail = 0;   /* next read position */
int g_kbd_count = 0;           /* number of chars available */

static inline void vlw(struct vi_device *d, uint32_t reg, uint32_t val) {
    mmio_write32(d->base + reg, val);
}
static inline uint32_t vlr(struct vi_device *d, uint32_t reg) {
    return mmio_read32(d->base + reg);
}

/* ---- Find virtio-input devices ---------------------------------------- */
#define VIRTIO_MMIO_BASE   0x0a000000UL
#define VIRTIO_MMIO_STRIDE 0x200
#define VIRTIO_MMIO_COUNT  32

static uintptr_t find_input(int skip) {
    int found = 0;
    for (int i = 0; i < VIRTIO_MMIO_COUNT; i++) {
        uintptr_t b = VIRTIO_MMIO_BASE + (uintptr_t)i * VIRTIO_MMIO_STRIDE;
        if (mmio_read32(b + VL_MAGIC)     != 0x74726976) continue;
        if (mmio_read32(b + VL_DEVICE_ID) != 18)         continue;
        if (found < skip) { found++; continue; }
        return b;
    }
    return 0;
}

/* ---- Initialise one device --------------------------------------------- */
static int vi_dev_init(struct vi_device *d, uintptr_t base) {
    d->base  = base;
    d->ready = 0;

    /* Allocate queue memory (2 pages, aligned) */
    d->qmem = (uint8_t *)kmalloc(2 * PAGE_SIZE + PAGE_SIZE);
    if (!d->qmem) { serial_write("[vi] OOM for qmem\r\n"); return -1; }
    d->qmem = (uint8_t *)(((uintptr_t)d->qmem + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));

    /* Allocate event buffer array */
    d->evt_raw = (uint8_t *)kmalloc(VQSIZE * 64 + 64);
    if (!d->evt_raw) { serial_write("[vi] OOM for evt\r\n"); return -1; }
    d->evt_raw = (uint8_t *)(((uintptr_t)d->evt_raw + 63) & ~63);

    /* Reset */
    vlw(d, VL_STATUS, 0);
    vlw(d, VL_STATUS, VTSTAT_ACK);
    vlw(d, VL_STATUS, VTSTAT_ACK | VTSTAT_DRIVER);
    vlw(d, VL_GUEST_FEAT, 0);
    vlw(d, VL_GUEST_PGS, PAGE_SIZE);

    /* Set up queue 0 */
    vlw(d, VL_QUEUE_SEL, 0);
    uint32_t qmax = vlr(d, VL_QUEUE_NMAX);
    uint32_t qnum = (qmax >= VQSIZE) ? VQSIZE : qmax;
    vlw(d, VL_QUEUE_NUM,   qnum);
    vlw(d, VL_QUEUE_ALIGN, PAGE_SIZE);
    uint64_t qpa = (uint64_t)(uintptr_t)d->qmem;
    vlw(d, VL_QUEUE_PFN, (uint32_t)(qpa / PAGE_SIZE));

    vlw(d, VL_STATUS, VTSTAT_ACK | VTSTAT_DRIVER | VTSTAT_DRIVER_OK);

    /* Clear and pre-fill avail ring */
    memset(d->qmem, 0, 2 * PAGE_SIZE);
    d->last_used = 0;

    for (int i = 0; i < (int)qnum; i++) {
        DEV_DESC(d)[i].addr  = (uint64_t)(uintptr_t)DEV_EVT(d, i);
        DEV_DESC(d)[i].len   = sizeof(struct vi_event);
        DEV_DESC(d)[i].flags = VDESC_F_WRITE;
        DEV_DESC(d)[i].next  = 0;
        DEV_AVAIL(d)->ring[i] = (uint16_t)i;
    }
    DEV_AVAIL(d)->idx = (uint16_t)qnum;
    __asm__ volatile("dsb sy" ::: "memory");

    d->ready = 1;
    return 0;
}

/* ---- Translate Linux keycode to ASCII (simple US layout) --------------- */
static char keycode_to_ascii(uint16_t code) {
    /* Only handle simple key presses (value=1 in EV_KEY).
     * This is a minimal US keyboard mapping. */
    switch (code) {
    /* Letters */
    case 30: return 'a'; case 31: return 's'; case 32: return 'd';
    case 33: return 'f'; case 34: return 'g'; case 35: return 'h';
    case 36: return 'j'; case 37: return 'k'; case 38: return 'l';
    case 44: return 'z'; case 45: return 'x'; case 46: return 'c';
    case 47: return 'v'; case 48: return 'b'; case 49: return 'n';
    case 50: return 'm';
    case 16: return 'q'; case 17: return 'w'; case 18: return 'e';
    case 19: return 'r'; case 20: return 't'; case 21: return 'y';
    case 22: return 'u'; case 23: return 'i'; case 24: return 'o';
    case 25: return 'p';
    /* Digits */
    case 2: return '1'; case 3: return '2'; case 4: return '3';
    case 5: return '4'; case 6: return '5'; case 7: return '6';
    case 8: return '7'; case 9: return '8'; case 10: return '9';
    case 11: return '0';
    /* Special */
    case 57: return ' ';        /* space */
    case 28: return '\n';       /* enter */
    case 14: return '\b';       /* backspace */
    case 12: return '-';        /* minus */
    case 13: return '=';        /* equals */
    case 39: return ';';        /* semicolon */
    case 40: return '\'';       /* apostrophe */
    case 51: return ',';        /* comma */
    case 52: return '.';        /* period */
    case 53: return '/';        /* slash */
    case 43: return '\\';       /* backslash */
    case 26: return '[';        /* left bracket */
    case 27: return ']';        /* right bracket */
    case 41: return '`';        /* grave */
    case 15: return '\t';       /* tab */
    case 1:  return 0x1b;       /* escape */
    default: return 0;          /* unmapped */
    }
}

/* ---- Initialise all devices -------------------------------------------- */
int virtio_input_init(void) {
    g_num_devs = 0;
    for (int idx = 0; idx < MAX_INPUT_DEVS; idx++) {
        uintptr_t base = find_input(idx);
        if (!base) break;
        serial_write("[virtio-input] dev ");
        serial_write_hex_u64((uint64_t)idx);
        serial_write(" at ");
        serial_write_hex_u64(base);
        serial_write("\r\n");
        if (vi_dev_init(&g_devs[idx], base) == 0) {
            g_num_devs++;
        }
    }
    if (g_num_devs == 0) {
        serial_write("[virtio-input] no devices found\r\n");
        return -1;
    }
    serial_write("[virtio-input] ");
    serial_write_hex_u64((uint64_t)g_num_devs);
    serial_write(" device(s) ready\r\n");
    return 0;
}

/* ---- Poll one device's used ring --------------------------------------- */
static void vi_poll_one(struct vi_device *d) {
    if (!d->ready) return;

    int mx = g_mouse_x, my = g_mouse_y, mb = g_mouse_buttons;

    __asm__ volatile("dsb sy" ::: "memory");
    uint16_t used_idx = DEV_USED(d)->idx;

    while (d->last_used != used_idx) {
        uint16_t slot = d->last_used % VQSIZE;
        uint32_t desc_id = DEV_USED(d)->ring[slot].id;
        struct vi_event *ev = DEV_EVT(d, desc_id);

        switch (ev->type) {
        case 3:   /* EV_ABS — absolute position (tablet) */
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
            } else if (ev->value == 1) {
                /* Keyboard key press */
                char ascii = keycode_to_ascii(ev->code);
                if (ascii && g_kbd_count < KBD_BUF_SIZE) {
                    g_kbd_buf[g_kbd_head] = ascii;
                    g_kbd_head = (g_kbd_head + 1) % KBD_BUF_SIZE;
                    g_kbd_count++;
                }
            }
            break;
        case 0:   /* EV_SYN — frame boundary, apply accumulated state */
            if (mx != g_mouse_x || my != g_mouse_y || mb != g_mouse_buttons) {
                g_mouse_x = mx;
                g_mouse_y = my;
                g_mouse_buttons = mb;
                g_mouse_moved = 1;
            }
            break;
        default:
            break;
        }

        /* Recycle buffer */
        DEV_AVAIL(d)->ring[DEV_AVAIL(d)->idx % VQSIZE] = desc_id;
        DEV_AVAIL(d)->idx++;
        __asm__ volatile("dsb sy" ::: "memory");

        d->last_used++;
    }
}

/* ---- Poll all devices -------------------------------------------------- */
void virtio_input_poll(void) {
    static int first = 1;
    int was_moved = 0;

    for (int i = 0; i < g_num_devs; i++) {
        int old_moved = g_mouse_moved;
        vi_poll_one(&g_devs[i]);
        if (g_mouse_moved && !old_moved) was_moved = 1;
    }

    if (was_moved && first) {
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

int virtio_input_is_ready(void) { return g_num_devs > 0; }

/* ---- Keyboard buffer access (called from render loop) ------------------ */
int virtio_kbd_read(char *out) {
    if (g_kbd_count == 0) return 0;
    *out = g_kbd_buf[g_kbd_tail];
    g_kbd_tail = (g_kbd_tail + 1) % KBD_BUF_SIZE;
    g_kbd_count--;
    return 1;
}
