#include "audio.h"
#include "io.h"
#include "serial.h"
#include "string.h"

/* ---- PCI config access ---- */
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

/* ---- AC97 state ---- */
static int g_audio_present = 0;
static uint16_t g_mixer_base = 0;
static uint16_t g_bm_base = 0;
static uint16_t g_vendor_id = 0;
static uint16_t g_device_id = 0;
static uint8_t  g_pci_bus = 0;
static uint8_t  g_pci_slot = 0;
/* Running count of buffers the engine drained that we had to pad with silence
 * (ring underruns). Surfaced via audio_get_info() for diagnostics. */
static uint32_t g_underruns = 0;

/* Buffer descriptors and PCM buffers in BSS (identity-mapped = phys addr) */
static struct ac97_buffer_descriptor g_bdl[AC97_BD_COUNT] __attribute__((aligned(8)));
static uint8_t g_pcm_bufs[AC97_BD_COUNT][AC97_BD_BYTES] __attribute__((aligned(128)));

/* Software ring buffer for userspace PCM submission */
static uint8_t g_ring[AUDIO_RING_SIZE];
static volatile uint32_t g_ring_head;
static volatile uint32_t g_ring_tail;

/* Index of the next free buffer descriptor to hand to the hardware. The buffers
 * the DMA still has to play are [CIV .. g_head_bd-1]. */
static int g_head_bd;

/* ---- AC97 helpers ---- */
static inline void mixer_write16(uint16_t reg, uint16_t val) {
    serial_write("Audio: mixer_write16 reg=");
    serial_write_hex_u64(reg);
    serial_write(" val=");
    serial_write_hex_u64(val);
    serial_write("\n");
    outw(g_mixer_base + reg, val);
}

static inline uint16_t mixer_read16(uint16_t reg) {
    return inw(g_mixer_base + reg);
}

static inline void bm_write32(uint16_t reg, uint32_t val) {
    outl(g_bm_base + reg, val);
}

static inline uint16_t bm_read16(uint16_t reg) {
    return inw(g_bm_base + reg);
}

static inline void bm_write16(uint16_t reg, uint16_t val) {
    outw(g_bm_base + reg, val);
}

static inline uint8_t bm_read8(uint16_t reg) {
    return inb(g_bm_base + reg);
}

static inline void bm_write8(uint16_t reg, uint8_t val) {
    outb(g_bm_base + reg, val);
}

/* ---- Software ring buffer ----
 * head = write pointer (producer), tail = read pointer (consumer).
 * Available (unread) bytes: from tail to head. */
static uint32_t ring_available(void) {
    uint32_t head = g_ring_head;
    uint32_t tail = g_ring_tail;
    if (head >= tail) return head - tail;
    return (AUDIO_RING_SIZE - tail) + head;
}

static uint32_t ring_free(void) {
    return AUDIO_RING_SIZE - 1 - ring_available();
}

static void ring_write(const void *data, uint32_t bytes) {
    uint32_t head = g_ring_head;
    uint32_t first = AUDIO_RING_SIZE - head;
    if (bytes > first) {
        memcpy(g_ring + head, data, first);
        memcpy(g_ring, (const uint8_t *)data + first, bytes - first);
    } else {
        memcpy(g_ring + head, data, bytes);
    }
    g_ring_head = (head + bytes) % AUDIO_RING_SIZE;
}

static uint32_t ring_read(void *data, uint32_t bytes) {
    uint32_t avail = ring_available();
    if (bytes > avail) bytes = avail;
    if (bytes == 0) return 0;
    uint32_t tail = g_ring_tail;
    uint32_t first = AUDIO_RING_SIZE - tail;
    if (bytes > first) {
        memcpy(data, g_ring + tail, first);
        memcpy((uint8_t *)data + first, g_ring, bytes - first);
    } else {
        memcpy(data, g_ring + tail, bytes);
    }
    g_ring_tail = (tail + bytes) % AUDIO_RING_SIZE;
    return bytes;
}

/* ---- Called periodically from the kernel main loop ----
 *
 * Producer/consumer model: the DMA engine plays the buffer-descriptor ring from
 * CIV up to LVI and then stops (DCH). Each tick we hand it as many *complete*
 * 512-byte PCM buffers as the software ring currently holds, advancing LVI to
 * the last one we filled. We never pad a buffer with silence mid-stream — that
 * was the source of the scratching — so the DAC only ever sees an unbroken run
 * of real samples. If the producer can't keep up the engine simply halts on a
 * buffer boundary (clean gap), and we restart it the moment data returns. */
static uint32_t g_tick_count = 0;
static uint32_t g_write_count = 0;

void audio_tick(void) {
    if (!g_audio_present) return;

    uint8_t  civ = bm_read8(AC97_BM_PCM_OUT_CIV);
    uint16_t sr  = bm_read16(AC97_BM_PCM_OUT_SR);

    /* Buffers still owned by the hardware (queued, not yet played). */
    int queued = (g_head_bd - (int)civ + AC97_BD_COUNT) % AC97_BD_COUNT;
    int filled = 0;

    /* Queue every buffer. Real audio from the ring when available; silence
     * otherwise so the DMA engine keeps running without restarts (restarts
     * cause a ~35 Hz buzz from the CIV-replay at each stop/start). */
    while (queued < AC97_BD_COUNT - 1)
    {
        if (ring_available() >= AC97_BD_BYTES) {
            ring_read(g_pcm_bufs[g_head_bd], AC97_BD_BYTES);
        } else {
            memset(g_pcm_bufs[g_head_bd], 0, AC97_BD_BYTES);
        }
        g_head_bd = (g_head_bd + 1) % AC97_BD_COUNT;
        ++queued;
        ++filled;
    }

    if (filled > 0) {
        bm_write8(AC97_BM_PCM_OUT_LVI,
                  (uint8_t)((g_head_bd + AC97_BD_COUNT - 1) % AC97_BD_COUNT));
        /* The engine had drained and stopped — resuming it after a real
         * starvation counts as an underrun for diagnostics. */
        if (sr & AC97_SR_DCH) {
            ++g_underruns;
            bm_write8(AC97_BM_PCM_OUT_CR, AC97_CR_RPBM);
        }
    }

    if (sr & (AC97_SR_BCIS | AC97_SR_LVBCI)) {
        bm_write16(AC97_BM_PCM_OUT_SR, sr & (AC97_SR_BCIS | AC97_SR_LVBCI));
    }

    /* Log every ~500 ticks so we can see the engine state without flooding */
    // if ((g_tick_count % 500) == 0) {
    //     serial_write("AUDIO_TICK: tick=");
    //     serial_write_hex_u64(g_tick_count);
    //     serial_write(" civ=");
    //     serial_write_hex_u64(civ);
    //     serial_write(" lvi=");
    //     serial_write_hex_u64(bm_read8(AC97_BM_PCM_OUT_LVI));
    //     serial_write(" sr=");
    //     serial_write_hex_u64(sr);
    //     serial_write(" ring=");
    //     serial_write_hex_u64(ring_available());
    //     serial_write(" head=");
    //     serial_write_hex_u64((uint64_t)(unsigned)g_head_bd);
    //     serial_write(" writes=");
    //     serial_write_hex_u64(g_write_count);
    //     serial_write("\n");
    // }
    ++g_tick_count;
}

/* ---- Public API ---- */
int audio_write(const void *data, uint32_t bytes) {
    if (!g_audio_present || !data || bytes == 0) return 0;

    uint32_t free = ring_free();
    uint32_t avail_before = ring_available();

    if (bytes > free) bytes = free;
    if (bytes == 0) return 0;

    ring_write(data, bytes);

    /* Log first call and every 35th (≈ once per second at 35 tics/s) */
    if (g_write_count == 0 || (g_write_count % 35) == 0) {
        serial_write("AUDIO_WRITE: call=");
        serial_write_hex_u64(g_write_count);
        serial_write(" bytes=");
        serial_write_hex_u64(bytes);
        serial_write(" ring_before=");
        serial_write_hex_u64(avail_before);
        serial_write(" ring_after=");
        serial_write_hex_u64(ring_available());
        serial_write(" free_was=");
        serial_write_hex_u64(free);
        serial_write("\n");
    }
    ++g_write_count;
    return (int)bytes;
}

int audio_present(void) {
    return g_audio_present;
}

void audio_get_info(struct audio_info *info) {
    if (!info) return;
    memset(info, 0, sizeof(*info));
    info->present     = (uint32_t)g_audio_present;
    info->vendor_id   = g_vendor_id;
    info->device_id   = g_device_id;
    info->pci_bus     = g_pci_bus;
    info->pci_slot    = g_pci_slot;
    info->mixer_base  = g_mixer_base;
    info->bm_base     = g_bm_base;
    info->sample_rate = AC97_PLAYBACK_RATE;
    info->channels    = 2;
    info->bits        = 16;
    info->bd_count    = AC97_BD_COUNT;
    info->bd_bytes    = AC97_BD_BYTES;
    info->ring_size   = AUDIO_RING_SIZE;
    info->ring_used   = ring_available();
    info->ring_free   = ring_free();
    info->underruns   = g_underruns;
    if (g_audio_present) {
        info->civ = bm_read8(AC97_BM_PCM_OUT_CIV);
        info->lvi = bm_read8(AC97_BM_PCM_OUT_LVI);
        info->sr  = bm_read16(AC97_BM_PCM_OUT_SR);
    }
}

void audio_init(void) {
    uint16_t bus;
    uint8_t slot;
    int found = 0;

    for (bus = 0; bus < 256u && !found; ++bus) {
        for (slot = 0; slot < 32u && !found; ++slot) {
            uint32_t id = pci_read32((uint8_t)bus, slot, 0u, 0x00u);
            uint16_t vendor = (uint16_t)(id & 0xffffu);
            uint16_t device = (uint16_t)((id >> 16) & 0xffffu);
            if (vendor == 0xffffu) continue;
            if (vendor == AC97_VENDOR_ID && device == AC97_DEVICE_ID) {
                uint32_t bar0 = pci_read32((uint8_t)bus, slot, 0u, 0x10u);
                uint32_t bar1 = pci_read32((uint8_t)bus, slot, 0u, 0x14u);
                uint32_t command = pci_read32((uint8_t)bus, slot, 0u, 0x04u);
                command |= 0x05u;
                pci_write32((uint8_t)bus, slot, 0u, 0x04u, command);

                /* QEMU: BAR0 = NAM (mixer), BAR1 = NABM (bus master) */
                g_mixer_base = (uint16_t)(bar0 & 0xfffcu);
                g_bm_base = (uint16_t)(bar1 & 0xfffcu);
                g_vendor_id = vendor;
                g_device_id = device;
                g_pci_bus = (uint8_t)bus;
                g_pci_slot = slot;
                found = 1;

                serial_write("AC97: NAM=");
                serial_write_hex_u64((uint64_t)g_mixer_base);
                serial_write(" BM=");
                serial_write_hex_u64((uint64_t)g_bm_base);
                serial_write("\n");

                serial_write("AC97: found\n");
            }
        }
    }

    if (!found) {
        serial_write("AC97: not found\n");
        return;
    }

    /* Reset bus master (CR is byte-access only in QEMU) */
    bm_write8(AC97_BM_PCM_OUT_CR, AC97_CR_RR);
    {
        volatile int w = 0;
        while (bm_read8(AC97_BM_PCM_OUT_CR) & AC97_CR_RR) {
            if (++w > 10000) break;
        }
    }

    /* Reset mixer, unmute all, set volume to max (0 = no attenuation) */
    mixer_write16(AC97_MIX_RESET, 0x0000u);
    {
        volatile int w = 0;
        while (w < 1000) ++w;
    }
    mixer_write16(AC97_MIX_MASTER_VOL, 0x0000u);
    mixer_write16(AC97_MIX_PCM_VOL, 0x0000u);

    /* Enable Variable Rate Audio (VRA) if available */
    mixer_write16(0x2Au, mixer_read16(0x2Au) | 0x01u);
    mixer_write16(AC97_MIX_PCM_FRONT_DAC_RATE, AC97_PLAYBACK_RATE);

    /* Set up BDL */
    int i;
    for (i = 0; i < AC97_BD_COUNT; i++) {
        memset(g_pcm_bufs[i], 0, AC97_BD_BYTES);
        g_bdl[i].pointer = (uint32_t)(uintptr_t)g_pcm_bufs[i];
        g_bdl[i].control = (uint32_t)(AC97_BD_SAMPLES & 0xFFFFu) | AC97_BD_IOC;
    }

    g_head_bd = 0;

    bm_write32(AC97_BM_PCM_OUT_BASE, (uint32_t)(uintptr_t)g_bdl);
    /* No audio queued yet: point LVI at CIV(0) so the engine immediately parks
     * (DCH) instead of looping stale silence. audio_tick() queues real buffers
     * and restarts it as soon as a process submits PCM. */
    bm_write8(AC97_BM_PCM_OUT_LVI, 0u);
    bm_write16(AC97_BM_PCM_OUT_SR, 0x0000u);

    /* Start PCM Out engine (it will halt on buffer 0 until we feed it). */
    bm_write8(AC97_BM_PCM_OUT_CR, AC97_CR_RPBM);

    g_audio_present = 1;
    serial_write("AC97: ready\n");
}

#include "settings.h"


static void audio_reinit(void) {
    /* Stop DMA engine */
    bm_write8(AC97_BM_PCM_OUT_CR, 0);

    struct os_settings *s = settings_get();

    /* Reconfigure Sample Rate */
    mixer_write16(AC97_MIX_PCM_FRONT_DAC_RATE, (uint16_t)s->audio_sample_rate);
    
    /* Apply Volume */
    mixer_write16(AC97_MIX_MASTER_VOL, (uint16_t)((100 - s->master_volume) * 0x1F / 100));

    /* Reset Buffer Head Index */
    g_head_bd = 0;
    bm_write8(AC97_BM_PCM_OUT_LVI, 0u);
    bm_write16(AC97_BM_PCM_OUT_SR, 0x0000u);

    /* Restart DMA engine */
    bm_write8(AC97_BM_PCM_OUT_CR, AC97_CR_RPBM);
    serial_write("Audio: reinitialized\n");
}

int audio_ioctl(int request, void *arg) {
    serial_write("Audio: ioctl request=");
    serial_write_hex_u64((uint64_t)request);
    serial_write("\n");

    struct os_settings *s = settings_get();
    uint32_t val = *(uint32_t *)arg;

    switch (request) {
        case AUDIO_IOCTL_SET_RATE:
            s->audio_sample_rate = val;
            audio_reinit();
            break;
        case AUDIO_IOCTL_SET_BUFFER_SIZE:
            s->audio_buffer_size = val;
            break;
        case AUDIO_IOCTL_SET_BUFFER_COUNT:
            s->dma_buffer_count = val;
            break;
        case AUDIO_IOCTL_SET_VOLUME:
            s->master_volume = val;
            audio_reinit();
            break;
        default:
            return -1;
    }
    settings_save();
    return 0;
}
