/* VibeOS arm64 — Virtio-sound driver (legacy/v1 MMIO transport).
 *
 * QEMU's `-device virtio-sound-device` on the `virt` machine exposes a
 * virtio-snd device on a virtio-mmio slot (same legacy v1 transport as
 * virtio-blk/virtio-input).  This driver brings up ONE output PCM stream
 * (stream 0) at 48000 Hz / stereo / signed-16, matching what the apps and the
 * x86 AC97 driver use, and feeds PCM via SYS_AUDIO_WRITE.
 *
 * Only the two queues we need are set up:
 *   queue 0 = controlq  (PCM_SET_PARAMS / PREPARE / START)
 *   queue 2 = txq       (PCM data, guest → device)
 *
 * The tx path is a fixed pool of TX_SLOTS buffers; each submission is a
 * 3-descriptor chain [xfer-header | pcm-data | status].  audio_snd_write copies
 * a chunk into a free slot and queues it; when all slots are busy it returns 0
 * so the caller (e.g. DOOM's blocking flush) yields and retries — giving
 * natural rate-limiting to the device's real playback speed.
 */
#include "arch.h"
#include "../../include/serial.h"
#include "../../include/string.h"
#include "../../include/timer.h"
#include "../../include/process.h"

/* ---- Virtio-mmio register offsets (legacy v1) ------------------------- */
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

#define VDESC_F_NEXT  1
#define VDESC_F_WRITE 2

#define PAGE_SIZE 4096

/* ---- virtio-snd protocol --------------------------------------------- */
#define VIRTIO_ID_SOUND 25

/* control request codes */
#define VIRTIO_SND_R_PCM_SET_PARAMS 0x0101
#define VIRTIO_SND_R_PCM_PREPARE    0x0102
#define VIRTIO_SND_R_PCM_START      0x0104
/* status codes */
#define VIRTIO_SND_S_OK             0x8000
/* PCM sample format / frame rate enums */
#define VIRTIO_SND_PCM_FMT_S16      5
#define VIRTIO_SND_PCM_RATE_48000   7

struct virtio_snd_pcm_set_params {
    uint32_t code;          /* VIRTIO_SND_R_PCM_SET_PARAMS */
    uint32_t stream_id;     /* 0 */
    uint32_t buffer_bytes;
    uint32_t period_bytes;
    uint32_t features;      /* 0 */
    uint8_t  channels;
    uint8_t  format;
    uint8_t  rate;
    uint8_t  padding;
} __attribute__((packed));

struct virtio_snd_pcm_hdr {     /* PREPARE / START request */
    uint32_t code;
    uint32_t stream_id;
} __attribute__((packed));

struct virtio_snd_hdr {         /* generic control response */
    uint32_t code;              /* status (VIRTIO_SND_S_OK) */
} __attribute__((packed));

struct virtio_snd_pcm_xfer {    /* tx data header */
    uint32_t stream_id;
} __attribute__((packed));

struct virtio_snd_pcm_status {  /* tx data trailer (device-written) */
    uint32_t status;
    uint32_t latency_bytes;
} __attribute__((packed));

/* ---- Virtqueue structures -------------------------------------------- */
struct vq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

#define CTL_QSZ 8
#define TX_QSZ  32   /* >= 3 * TX_SLOTS */

struct vq_avail_ctl { uint16_t flags, idx, ring[CTL_QSZ]; } __attribute__((packed));
struct vq_used_elem { uint32_t id, len; } __attribute__((packed));
struct vq_used_ctl  { uint16_t flags, idx; struct vq_used_elem ring[CTL_QSZ]; } __attribute__((packed));

struct vq_avail_tx  { uint16_t flags, idx, ring[TX_QSZ]; } __attribute__((packed));
struct vq_used_tx   { uint16_t flags, idx; struct vq_used_elem ring[TX_QSZ]; } __attribute__((packed));

/* Queue memory: desc table + avail in first page, used ring in second. */
static uint8_t g_ctl_q[2 * PAGE_SIZE] __attribute__((aligned(PAGE_SIZE)));
static uint8_t g_tx_q [2 * PAGE_SIZE] __attribute__((aligned(PAGE_SIZE)));

#define CTL_DESC  ((struct vq_desc *)g_ctl_q)
#define CTL_AVAIL ((struct vq_avail_ctl *)(g_ctl_q + CTL_QSZ * sizeof(struct vq_desc)))
#define CTL_USED  ((struct vq_used_ctl  *)(g_ctl_q + PAGE_SIZE))

#define TX_DESC   ((struct vq_desc *)g_tx_q)
#define TX_AVAIL  ((struct vq_avail_tx *)(g_tx_q + TX_QSZ * sizeof(struct vq_desc)))
#define TX_USED   ((struct vq_used_tx  *)(g_tx_q + PAGE_SIZE))

/* ---- TX buffer pool -------------------------------------------------- */
#define TX_SLOTS     8
#define TX_BUF_BYTES 8192

struct tx_slot {
    struct virtio_snd_pcm_xfer   xfer;   /* header (device-readable)   */
    struct virtio_snd_pcm_status status; /* trailer (device-writable)  */
    uint8_t                      data[TX_BUF_BYTES];
    int                          busy;   /* queued, awaiting completion */
} __attribute__((aligned(64)));
static struct tx_slot g_tx[TX_SLOTS] __attribute__((aligned(PAGE_SIZE)));

/* ---- Per-process voice mixing ---------------------------------------- *
 * The device has ONE output stream, but several apps (e.g. two DOOMs) may play
 * at once.  Each process gets its own ring buffer ("voice"); virtio_snd_mix_tick
 * sums all active voices into the device tx buffers, so apps don't contend for
 * the single 48 kHz sink (which made them mutually throttle and stutter).
 * Mirrors the x86 AC97 mixer (kernel/src/audio.c). */
#define SND_VOICES        4
#define SND_RING_SIZE     32768          /* per-voice ring (power of two-ish) */
#define SND_PERIOD_BYTES  4096           /* mix granularity (1024 stereo frames) */
#define SND_PREROLL_BYTES 8192           /* buffer this much before a voice starts */
#define SND_PREROLL_TIMEOUT 10           /* ...or start anyway after N ticks */

static uint32_t g_snd_rate   = 48000;    /* current sample rate (Hz) */
static uint32_t g_snd_volume = 80;       /* master volume 0–100 */

struct snd_voice {
    uint32_t pid;                /* owner (0 = free) */
    uint8_t  ring[SND_RING_SIZE];
    uint32_t head, tail;         /* write / read pointers */
    int      pre_roll;           /* 1 = still buffering up to threshold */
    uint64_t last_write_tick;
} __attribute__((aligned(64)));
static struct snd_voice g_voices[SND_VOICES];

static uint32_t voice_avail(const struct snd_voice *v) {
    return (v->head >= v->tail) ? (v->head - v->tail)
                                : (SND_RING_SIZE - v->tail + v->head);
}
static uint32_t voice_free(const struct snd_voice *v) {
    return SND_RING_SIZE - 1 - voice_avail(v);
}
static void voice_push(struct snd_voice *v, const uint8_t *data, uint32_t n) {
    uint32_t first = SND_RING_SIZE - v->head;
    if (n > first) {
        for (uint32_t i = 0; i < first; i++) v->ring[v->head + i] = data[i];
        for (uint32_t i = 0; i < n - first; i++) v->ring[i] = data[first + i];
    } else {
        for (uint32_t i = 0; i < n; i++) v->ring[v->head + i] = data[i];
    }
    v->head = (v->head + n) % SND_RING_SIZE;
}
static uint32_t voice_pull(struct snd_voice *v, uint8_t *out, uint32_t n) {
    uint32_t avail = voice_avail(v);
    if (n > avail) n = avail;
    if (n == 0) return 0;
    uint32_t first = SND_RING_SIZE - v->tail;
    if (n > first) {
        for (uint32_t i = 0; i < first; i++) out[i] = v->ring[v->tail + i];
        for (uint32_t i = 0; i < n - first; i++) out[first + i] = v->ring[i];
    } else {
        for (uint32_t i = 0; i < n; i++) out[i] = v->ring[v->tail + i];
    }
    v->tail = (v->tail + n) % SND_RING_SIZE;
    return n;
}
static struct snd_voice *voice_for_pid(uint32_t pid) {
    int free_slot = -1;
    for (int i = 0; i < SND_VOICES; i++) {
        if (g_voices[i].pid == pid) return &g_voices[i];
        if (g_voices[i].pid == 0 && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) return 0;
    g_voices[free_slot].head = g_voices[free_slot].tail = 0;
    g_voices[free_slot].pre_roll = 1;
    g_voices[free_slot].last_write_tick = timer_tick_count();
    g_voices[free_slot].pid = pid;
    return &g_voices[free_slot];
}

/* ---- Control command scratch (one at a time at init) ----------------- */
static struct virtio_snd_pcm_set_params g_ctl_req __attribute__((aligned(64)));
static struct virtio_snd_hdr            g_ctl_resp __attribute__((aligned(64)));

/* ---- Device state ---------------------------------------------------- */
static uintptr_t g_base = 0;
static int       g_ready = 0;
static uint16_t  g_tx_last_used = 0;
static uint16_t  g_tx_avail_idx = 0;
static uint16_t  g_ctl_avail_idx = 0;   /* free-running; never reset */
static uint16_t  g_ctl_last_used = 0;

static inline void vtw(uint32_t reg, uint32_t val) { mmio_write32(g_base + reg, val); }
static inline uint32_t vtr(uint32_t reg) { return mmio_read32(g_base + reg); }

#define VIRTIO_MMIO_BASE   0x0a000000UL
#define VIRTIO_MMIO_STRIDE 0x200
#define VIRTIO_MMIO_COUNT  32

static uintptr_t find_snd(void) {
    for (int i = 0; i < VIRTIO_MMIO_COUNT; i++) {
        uintptr_t b = VIRTIO_MMIO_BASE + (uintptr_t)i * VIRTIO_MMIO_STRIDE;
        if (mmio_read32(b + VL_MAGIC)     != 0x74726976) continue;
        if (mmio_read32(b + VL_DEVICE_ID) != VIRTIO_ID_SOUND) continue;
        return b;
    }
    return 0;
}

static void setup_queue(int sel, void *qmem, uint32_t qsz) {
    vtw(VL_QUEUE_SEL, (uint32_t)sel);
    uint32_t qmax = vtr(VL_QUEUE_NMAX);
    if (qmax < qsz) { serial_write("[virtio-snd] queue too small\r\n"); }
    vtw(VL_QUEUE_NUM,   qsz);
    vtw(VL_QUEUE_ALIGN, PAGE_SIZE);
    vtw(VL_QUEUE_PFN, (uint32_t)((uint64_t)(uintptr_t)qmem / PAGE_SIZE));
}

/* Submit one control command (request → response) on the control queue and
 * poll for completion.  Returns the device status code (VIRTIO_SND_S_OK on
 * success), or -1 on timeout. */
static int ctl_cmd(const void *req, uint32_t req_len) {
    g_ctl_resp.code = 0;

    /* Descriptors 0/1 reused each command (one in flight at a time). */
    CTL_DESC[0].addr  = (uint64_t)(uintptr_t)req;
    CTL_DESC[0].len   = req_len;
    CTL_DESC[0].flags = VDESC_F_NEXT;
    CTL_DESC[0].next  = 1;
    CTL_DESC[1].addr  = (uint64_t)(uintptr_t)&g_ctl_resp;
    CTL_DESC[1].len   = sizeof(g_ctl_resp);
    CTL_DESC[1].flags = VDESC_F_WRITE;
    CTL_DESC[1].next  = 0;

    /* avail.idx is a free-running 16-bit counter — never reset it, or the
     * device sees no new work after the first command. */
    CTL_AVAIL->ring[g_ctl_avail_idx % CTL_QSZ] = 0;
    __asm__ volatile("dsb sy" ::: "memory");
    g_ctl_avail_idx++;
    CTL_AVAIL->idx = g_ctl_avail_idx;
    __asm__ volatile("dsb sy" ::: "memory");
    vtw(VL_QUEUE_SEL, 0);
    vtw(VL_QUEUE_NOTIFY, 0);

    for (volatile uint32_t spin = 0; spin < 200000000u; spin++) {
        __asm__ volatile("dsb sy" ::: "memory");
        if (CTL_USED->idx != g_ctl_last_used) {
            g_ctl_last_used = CTL_USED->idx;
            vtw(VL_IRQ_ACK, vtr(VL_IRQ_STATUS));
            return (int)g_ctl_resp.code;
        }
    }
    serial_write("[virtio-snd] ctl timeout\r\n");
    return -1;
}

/* Reclaim completed tx buffers from the used ring. */
static void tx_reclaim(void) {
    __asm__ volatile("dsb sy" ::: "memory");
    while (g_tx_last_used != TX_USED->idx) {
        uint32_t id = TX_USED->ring[g_tx_last_used % TX_QSZ].id;
        uint32_t slot = id / 3;
        if (slot < TX_SLOTS) g_tx[slot].busy = 0;
        g_tx_last_used++;
    }
}

int virtio_snd_init(void) {
    uintptr_t base = find_snd();
    if (!base) { serial_write("[virtio-snd] not found\r\n"); return -1; }
    if (vtr(base + VL_VERSION) != 1) {
        serial_write("[virtio-snd] unsupported version\r\n"); return -1;
    }
    g_base = base;
    serial_write("[virtio-snd] found at "); serial_write_hex_u64(base); serial_write("\r\n");

    /* Reset → ack → driver → features=0 → page size */
    vtw(VL_STATUS, 0);
    vtw(VL_STATUS, VTSTAT_ACK);
    vtw(VL_STATUS, VTSTAT_ACK | VTSTAT_DRIVER);
    vtw(VL_GUEST_FEAT, 0);
    vtw(VL_GUEST_PGS, PAGE_SIZE);

    memset(g_ctl_q, 0, sizeof(g_ctl_q));
    memset(g_tx_q,  0, sizeof(g_tx_q));
    setup_queue(0, g_ctl_q, CTL_QSZ);   /* controlq */
    setup_queue(2, g_tx_q,  TX_QSZ);    /* txq      */

    vtw(VL_STATUS, VTSTAT_ACK | VTSTAT_DRIVER | VTSTAT_DRIVER_OK);

    /* Configure output stream 0: 48000 Hz / stereo / S16. */
    g_ctl_req.code         = VIRTIO_SND_R_PCM_SET_PARAMS;
    g_ctl_req.stream_id    = 0;
    g_ctl_req.buffer_bytes = TX_SLOTS * TX_BUF_BYTES;
    g_ctl_req.period_bytes = TX_BUF_BYTES;
    g_ctl_req.features     = 0;
    g_ctl_req.channels     = 2;
    g_ctl_req.format       = VIRTIO_SND_PCM_FMT_S16;
    g_ctl_req.rate         = VIRTIO_SND_PCM_RATE_48000;
    g_ctl_req.padding      = 0;
    if (ctl_cmd(&g_ctl_req, sizeof(g_ctl_req)) != VIRTIO_SND_S_OK) {
        serial_write("[virtio-snd] SET_PARAMS failed\r\n"); return -1;
    }

    struct virtio_snd_pcm_hdr ph;
    ph.code = VIRTIO_SND_R_PCM_PREPARE; ph.stream_id = 0;
    if (ctl_cmd(&ph, sizeof(ph)) != VIRTIO_SND_S_OK) {
        serial_write("[virtio-snd] PREPARE failed\r\n"); return -1;
    }
    ph.code = VIRTIO_SND_R_PCM_START; ph.stream_id = 0;
    if (ctl_cmd(&ph, sizeof(ph)) != VIRTIO_SND_S_OK) {
        serial_write("[virtio-snd] START failed\r\n"); return -1;
    }

    g_tx_last_used = 0;
    g_tx_avail_idx = 0;
    g_ready = 1;
    serial_write("[virtio-snd] ready (48000/stereo/S16)\r\n");
    return 0;
}

int virtio_snd_ready(void) { return g_ready; }

/* Submit one already-mixed PCM buffer (n bytes) to the device tx queue.
 * Returns 0 on success, -1 if no free slot. */
static int tx_submit(const uint8_t *pcm, uint32_t n) {
    int slot = -1;
    for (int i = 0; i < TX_SLOTS; i++) if (!g_tx[i].busy) { slot = i; break; }
    if (slot < 0) return -1;
    if (n > TX_BUF_BYTES) n = TX_BUF_BYTES;
    for (uint32_t i = 0; i < n; i++) g_tx[slot].data[i] = pcm[i];

    g_tx[slot].xfer.stream_id = 0;
    g_tx[slot].status.status  = 0;
    g_tx[slot].busy           = 1;

    uint16_t d0 = (uint16_t)(slot * 3);
    uint16_t d1 = (uint16_t)(slot * 3 + 1);
    uint16_t d2 = (uint16_t)(slot * 3 + 2);
    TX_DESC[d0].addr = (uint64_t)(uintptr_t)&g_tx[slot].xfer;
    TX_DESC[d0].len  = sizeof(struct virtio_snd_pcm_xfer);
    TX_DESC[d0].flags = VDESC_F_NEXT; TX_DESC[d0].next = d1;
    TX_DESC[d1].addr = (uint64_t)(uintptr_t)g_tx[slot].data;
    TX_DESC[d1].len  = n;
    TX_DESC[d1].flags = VDESC_F_NEXT; TX_DESC[d1].next = d2;
    TX_DESC[d2].addr = (uint64_t)(uintptr_t)&g_tx[slot].status;
    TX_DESC[d2].len  = sizeof(struct virtio_snd_pcm_status);
    TX_DESC[d2].flags = VDESC_F_WRITE; TX_DESC[d2].next = 0;

    TX_AVAIL->ring[g_tx_avail_idx % TX_QSZ] = d0;
    __asm__ volatile("dsb sy" ::: "memory");
    g_tx_avail_idx++;
    TX_AVAIL->idx = g_tx_avail_idx;
    __asm__ volatile("dsb sy" ::: "memory");
    vtw(VL_QUEUE_SEL, 2);
    vtw(VL_QUEUE_NOTIFY, 2);
    return 0;
}

/* Queue up to `bytes` of PCM (stereo S16) from process `pid` into its voice
 * ring.  Returns bytes accepted, or 0 if the ring is full (caller yields and
 * retries — this paces each app to real time independently of the others). */
int virtio_snd_write(uint32_t pid, const void *buf, unsigned long bytes) {
    if (!g_ready || !buf || bytes == 0) return 0;
    struct snd_voice *v = voice_for_pid(pid);
    if (!v) return (int)bytes;   /* no free voice — discard, don't hang */
    uint32_t freeb = voice_free(v);
    if (freeb == 0) return 0;
    uint32_t n = (bytes < freeb) ? (uint32_t)bytes : freeb;
    n &= ~3u;                    /* whole stereo S16 frames */
    if (n == 0) return 0;
    voice_push(v, (const uint8_t *)buf, n);
    v->last_write_tick = timer_tick_count();
    return (int)n;
}

/* Mix all active voices and feed the device.  Called once per render frame;
 * fills every free tx slot, so playback self-clocks to the device drain rate. */
void virtio_snd_mix_tick(void) {
    if (!g_ready) return;
    tx_reclaim();

    /* Reap voices whose process has exited and whose ring has drained. */
    for (int i = 0; i < SND_VOICES; i++) {
        if (g_voices[i].pid && voice_avail(&g_voices[i]) == 0 &&
            !process_pid_alive(g_voices[i].pid)) {
            g_voices[i].pid = 0;
        }
    }

    int32_t acc[SND_PERIOD_BYTES / 2];
    uint8_t tmp[SND_PERIOD_BYTES];

    for (;;) {
        /* Stop if no free tx slot. */
        int have_slot = 0;
        for (int i = 0; i < TX_SLOTS; i++) if (!g_tx[i].busy) { have_slot = 1; break; }
        if (!have_slot) break;

        int active = 0;
        for (int i = 0; i < SND_PERIOD_BYTES / 2; i++) acc[i] = 0;

        for (int vi = 0; vi < SND_VOICES; vi++) {
            struct snd_voice *v = &g_voices[vi];
            if (v->pid == 0) continue;
            uint32_t avail = voice_avail(v);
            if (avail == 0) { v->pre_roll = 1; continue; }
            if (v->pre_roll) {
                uint64_t now = timer_tick_count();
                if (avail < SND_PREROLL_BYTES &&
                    (now - v->last_write_tick) < SND_PREROLL_TIMEOUT)
                    continue;   /* still buffering */
                v->pre_roll = 0;
            }
            uint32_t to_read = (avail < SND_PERIOD_BYTES) ? avail : SND_PERIOD_BYTES;
            to_read &= ~3u;
            if (to_read == 0) continue;
            memset(tmp, 0, sizeof(tmp));
            voice_pull(v, tmp, to_read);
            const int16_t *src = (const int16_t *)tmp;
            for (int i = 0; i < SND_PERIOD_BYTES / 2; i++) acc[i] += (int32_t)src[i];
            active++;
        }

        if (active == 0) break;   /* nothing to play right now */

        /* Average so N summed full-scale streams don't clip. */
        if (active > 1)
            for (int i = 0; i < SND_PERIOD_BYTES / 2; i++) acc[i] /= active;

        int16_t mixed[SND_PERIOD_BYTES / 2];
        for (int i = 0; i < SND_PERIOD_BYTES / 2; i++) {
            int32_t s = acc[i];
            if (g_snd_volume < 100) s = s * (int32_t)g_snd_volume / 100;
            if (s > 32767) s = 32767; else if (s < -32768) s = -32768;
            mixed[i] = (int16_t)s;
        }
        if (tx_submit((const uint8_t *)mixed, SND_PERIOD_BYTES) != 0) break;
    }
}

/* Number of tx slots currently queued (for audio_info ring_used). */
int virtio_snd_busy_slots(void) {
    tx_reclaim();
    int n = 0;
    for (int i = 0; i < TX_SLOTS; i++) if (g_tx[i].busy) n++;
    return n;
}

/* ---- Rate / Volume IOCTL ----------------------------------------------- */
uint32_t virtio_snd_get_rate(void)   { return g_snd_rate; }
void     virtio_snd_set_rate(uint32_t hz) {
    if (hz >= 8000 && hz <= 192000) g_snd_rate = hz;
}
uint32_t virtio_snd_get_volume(void) { return g_snd_volume; }
void     virtio_snd_set_volume(uint32_t vol) {
    if (vol <= 100) g_snd_volume = vol;
}
