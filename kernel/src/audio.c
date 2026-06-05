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

#include "audio.h"
#include "io.h"
#include "serial.h"
#include "string.h"
#include "timer.h"

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
static uint32_t g_underruns = 0;

/* Buffer descriptors and PCM buffers in BSS (identity-mapped = phys addr) */
static struct ac97_buffer_descriptor g_bdl[AC97_BD_COUNT] __attribute__((aligned(8)));
static uint8_t g_pcm_bufs[AC97_BD_COUNT][AC97_BD_BYTES] __attribute__((aligned(128)));

/* Index of the next free buffer descriptor to hand to the hardware. */
static int g_head_bd;

/* ---- Per-process voice ring buffers ----------------------------------------
 *
 * Each process that submits audio gets its own ring buffer (a "voice").
 * audio_tick() reads from all active voices, sums the int16 samples, clamps
 * to [-32768, 32767], and writes the result to the DMA engine.  This lets
 * multiple apps (e.g., two DOOM instances) play simultaneously without
 * corrupting each other's PCM data.
 *
 * Voice slot 0 is reserved for the legacy single-pid path if needed.
 * pid == 0 means the slot is free. */
struct audio_voice {
    uint32_t         pid;               /* owning process (0 = free) */
    uint8_t          ring[AUDIO_RING_SIZE];
    volatile uint32_t head;             /* write pointer */
    volatile uint32_t tail;             /* read pointer  */
    uint32_t         pre_roll_active;   /* 1 if we are buffering up to the threshold */
    uint64_t         last_write_tick;   /* tick of the last audio write */
};

static struct audio_voice g_voices[AUDIO_VOICES];

/* ---- Voice ring helpers -------------------------------------------------- */

static uint32_t voice_available(const struct audio_voice *v) {
    uint32_t h = v->head, t = v->tail;
    return (h >= t) ? (h - t) : (AUDIO_RING_SIZE - t + h);
}

static uint32_t voice_free(const struct audio_voice *v) {
    return AUDIO_RING_SIZE - 1 - voice_available(v);
}

static void voice_write(struct audio_voice *v, const void *data, uint32_t bytes) {
    uint32_t head = v->head;
    uint32_t first = AUDIO_RING_SIZE - head;
    if (bytes > first) {
        memcpy(v->ring + head, data, first);
        memcpy(v->ring, (const uint8_t *)data + first, bytes - first);
    } else {
        memcpy(v->ring + head, data, bytes);
    }
    v->head = (head + bytes) % AUDIO_RING_SIZE;
}

/* Read exactly 'bytes' from the voice ring; returns bytes actually read. */
static uint32_t voice_read(struct audio_voice *v, void *out, uint32_t bytes) {
    uint32_t avail = voice_available(v);
    if (bytes > avail) bytes = avail;
    if (bytes == 0) return 0;
    uint32_t tail = v->tail;
    uint32_t first = AUDIO_RING_SIZE - tail;
    if (bytes > first) {
        memcpy(out, v->ring + tail, first);
        memcpy((uint8_t *)out + first, v->ring, bytes - first);
    } else {
        memcpy(out, v->ring + tail, bytes);
    }
    v->tail = (tail + bytes) % AUDIO_RING_SIZE;
    return bytes;
}

/* Find the voice slot for pid, or allocate a free one.
 * Returns NULL if all slots are occupied by different processes. */
static struct audio_voice *voice_for_pid(uint32_t pid) {
    int i, free_slot = -1;
    for (i = 0; i < AUDIO_VOICES; i++) {
        if (g_voices[i].pid == pid) return &g_voices[i];
        if (g_voices[i].pid == 0 && free_slot < 0) free_slot = i;
    }
    if (free_slot >= 0) {
        /* Initialise the ring pointers BEFORE publishing pid, so audio_tick()
         * (which may now run from the timer IRQ and preempt this) never sees a
         * claimed voice with stale head/tail. The barrier keeps the compiler
         * from reordering the pid store ahead of the pointer stores. */
        g_voices[free_slot].head = 0;
        g_voices[free_slot].tail = 0;
        g_voices[free_slot].pre_roll_active = 1;
        g_voices[free_slot].last_write_tick = timer_tick_count();
        __atomic_thread_fence(__ATOMIC_RELEASE);
        g_voices[free_slot].pid  = pid;
        return &g_voices[free_slot];
    }
    return 0; /* no free slot — caller gets silence */
}

/* Mix all active voices into one DMA-sized buffer of int16 stereo PCM.
 * Voices with insufficient data contribute silence for the missing portion. */
#define AUDIO_PRE_ROLL_BYTES          8192
#define AUDIO_PRE_ROLL_TIMEOUT_TICKS  10

static void mix_voices(uint8_t *out) {
    /* Accumulate in int32 to avoid overflow when summing multiple voices. */
    int32_t acc[AC97_BD_BYTES / 2];
    uint8_t  tmp[AC97_BD_BYTES];
    int i, v, active = 0;

    for (i = 0; i < AC97_BD_BYTES / 2; i++) acc[i] = 0;

    for (v = 0; v < AUDIO_VOICES; v++) {
        if (g_voices[v].pid == 0) continue;

        uint32_t avail = voice_available(&g_voices[v]);
        if (avail == 0) {
            /* If the voice ran completely dry, re-enable pre-rolling. */
            g_voices[v].pre_roll_active = 1;
            continue;
        }

        if (g_voices[v].pre_roll_active) {
            uint64_t now = timer_tick_count();
            if (avail < AUDIO_PRE_ROLL_BYTES && (now - g_voices[v].last_write_tick) < AUDIO_PRE_ROLL_TIMEOUT_TICKS) {
                /* Still buffering/pre-rolling, contribute silence. */
                continue;
            }
            g_voices[v].pre_roll_active = 0;
        }

        uint32_t to_read = (avail < AC97_BD_BYTES) ? avail : AC97_BD_BYTES;
        to_read &= ~3u; /* Align to stereo 16-bit frame size (4 bytes) */
        if (to_read == 0) continue;

        memset(tmp, 0, AC97_BD_BYTES);
        voice_read(&g_voices[v], tmp, to_read);
        {
            const int16_t *src = (const int16_t *)tmp;
            for (i = 0; i < AC97_BD_BYTES / 2; i++)
                acc[i] += (int32_t)src[i];
        }
        active++;
    }

    /* Normalise by the number of contributing voices so summing N full-scale
     * streams doesn't clip.  With one voice the gain is 1.0 (unchanged);
     * with two voices each contributes at 0.5 scale, preventing hard
     * clipping which sounds like buzzing/crackling. */
    if (active > 1) {
        for (i = 0; i < AC97_BD_BYTES / 2; i++)
            acc[i] /= active;
    }

    /* Clamp (should rarely be needed after normalisation) and write to DMA. */
    {
        int16_t *dst = (int16_t *)out;
        for (i = 0; i < AC97_BD_BYTES / 2; i++) {
            int32_t s = acc[i];
            if      (s >  32767) s =  32767;
            else if (s < -32768) s = -32768;
            dst[i] = (int16_t)s;
        }
    }
}

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

/* ---- Called periodically from the kernel main loop ----------------------- */

void audio_tick(void) {
    if (!g_audio_present) return;

    /* TEMP: every ~1s dump DMA underruns + each active voice's ring fill level
     * (in ms of buffered audio) to see whether the stutter is DMA-feed or
     * voice-production (doom not keeping the ring full). */
    static uint32_t s_dbg = 0;
    if ((++s_dbg % 100u) == 0) {
        serial_write("AUDIO dbg: underruns=");
        serial_write_hex_u64(g_underruns);
        for (int vv = 0; vv < AUDIO_VOICES; vv++) {
            if (g_voices[vv].pid == 0) continue;
            uint32_t avail = voice_available(&g_voices[vv]);
            /* bytes / (48000*4) * 1000 = ms; 192 bytes per ms */
            serial_write(" v"); serial_write_hex_u64(g_voices[vv].pid);
            serial_write("="); serial_write_hex_u64(avail / 192u);
            serial_write("ms");
        }
        serial_write("\n");
    }

    uint8_t  civ = bm_read8(AC97_BM_PCM_OUT_CIV);
    uint16_t sr  = bm_read16(AC97_BM_PCM_OUT_SR);

    int queued = (g_head_bd - (int)civ + AC97_BD_COUNT) % AC97_BD_COUNT;
    int filled = 0;

    /* Fill available DMA slots.  mix_voices() sums all active voice rings into
     * a single 512-byte int16 buffer; silence is written if all voices are dry
     * so the DMA engine never restarts (restarts cause a brief buzz). */
    while (queued < AC97_BD_COUNT - 1) {
        mix_voices(g_pcm_bufs[g_head_bd]);
        g_head_bd = (g_head_bd + 1) % AC97_BD_COUNT;
        ++queued;
        ++filled;
    }

    if (filled > 0) {
        bm_write8(AC97_BM_PCM_OUT_LVI,
                  (uint8_t)((g_head_bd + AC97_BD_COUNT - 1) % AC97_BD_COUNT));
        if (sr & AC97_SR_DCH) {
            ++g_underruns;
            bm_write8(AC97_BM_PCM_OUT_CR, AC97_CR_RPBM);
        }
    }

    if (sr & (AC97_SR_BCIS | AC97_SR_LVBCI))
        bm_write16(AC97_BM_PCM_OUT_SR, sr & (AC97_SR_BCIS | AC97_SR_LVBCI));
}

/* ---- Public API ---------------------------------------------------------- */

/* Write PCM data for the given process pid into its dedicated voice ring.
 * If no voice is allocated yet, the first free slot is claimed.
 * Returns bytes accepted (may be less than 'bytes' if the ring is full). */
int audio_write(uint32_t pid, const void *data, uint32_t bytes) {
    if (!g_audio_present || !data || bytes == 0) return 0;

    struct audio_voice *v = voice_for_pid(pid);
    if (!v) return 0;  /* all voice slots occupied — caller gets silence */

    uint32_t free = voice_free(v);
    if (bytes > free) bytes = free;
    if (bytes == 0) return 0;

    voice_write(v, data, bytes);
    v->last_write_tick = timer_tick_count();
    return (int)bytes;
}

/* Release the voice for pid (call when the process exits). */
void audio_release_voice(uint32_t pid) {
    int i;
    for (i = 0; i < AUDIO_VOICES; i++) {
        if (g_voices[i].pid == pid) {
            g_voices[i].pid  = 0;
            g_voices[i].head = 0;
            g_voices[i].tail = 0;
            g_voices[i].pre_roll_active = 0;
            g_voices[i].last_write_tick = 0;
            return;
        }
    }
}

int audio_present(void) {
    return g_audio_present;
}

void audio_get_info(struct audio_info *info) {
    int i;
    uint32_t total_used = 0, total_free = 0;
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
    /* Report aggregate ring usage across all active voices. */
    for (i = 0; i < AUDIO_VOICES; i++) {
        if (g_voices[i].pid == 0) continue;
        total_used += voice_available(&g_voices[i]);
        total_free += voice_free(&g_voices[i]);
    }
    info->ring_size   = AUDIO_RING_SIZE * AUDIO_VOICES;
    info->ring_used   = total_used;
    info->ring_free   = total_free ? total_free : (AUDIO_RING_SIZE - 1);
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
    /* Do NOT call settings_save() here — a synchronous disk write inside an
     * audio ioctl leaves the IDE controller in an intermediate state that
     * caused the next disk read to fail with EIO.  The root cause of that
     * issue was thread->syscalls.fd_table not being set for spawned threads;
     * now fixed in process_create_thread.  Settings are persisted normally. */
    return 0;
}
