#ifndef VIBEOS_AUDIO_H
#define VIBEOS_AUDIO_H

#include <stdint.h>

#define SYS_AUDIO_WRITE  49
#define SYS_AUDIO_INFO   50
#define SYS_AUDIO_IOCTL  60

/* Request codes for audio_ioctl() — match kernel AUDIO_IOCTL_* constants. */
#define AUDIO_IOCTL_SET_RATE         0  /* change sample rate (Hz) */
#define AUDIO_IOCTL_SET_BUFFER_SIZE  1  /* change DMA buffer size (bytes) */
#define AUDIO_IOCTL_SET_BUFFER_COUNT 2  /* change number of DMA buffers */
#define AUDIO_IOCTL_SET_VOLUME       3  /* master volume 0–100 */

/* Mirrors struct audio_info in kernel/include/audio.h. */
struct audio_info {
    uint32_t present;       /* 1 if an AC97 device was found and initialized */
    uint16_t vendor_id;     /* PCI vendor (0x8086 = Intel) */
    uint16_t device_id;     /* PCI device (0x2415 = 82801AA AC97) */
    uint8_t  pci_bus;
    uint8_t  pci_slot;
    uint8_t  bits;          /* sample bit depth (16) */
    uint8_t  channels;      /* channel count (2 = stereo) */
    uint32_t mixer_base;    /* NAM I/O base (BAR0) */
    uint32_t bm_base;       /* NABM bus-master I/O base (BAR1) */
    uint32_t sample_rate;   /* playback rate in Hz (48000) */
    uint32_t bd_count;      /* DMA buffer descriptors in the ring */
    uint32_t bd_bytes;      /* bytes per DMA buffer */
    uint32_t ring_size;     /* software ring capacity in bytes */
    uint32_t ring_used;     /* bytes currently queued */
    uint32_t ring_free;     /* bytes free for submission */
    uint32_t underruns;     /* fully-silent buffers emitted (ring starvation) */
    uint8_t  civ;           /* current index value (buffer being played) */
    uint8_t  lvi;           /* last valid index */
    uint16_t sr;            /* PCM-out status register */
};

static inline int audio_write(const void *data, unsigned long bytes) {
    int ret;
    __asm__ volatile("int $0x80"
        : "=a"(ret)
        : "a"((long)SYS_AUDIO_WRITE), "D"((long)data), "S"((long)bytes)
        : "rcx", "r11", "memory");
    return ret;
}

static inline int audio_info(struct audio_info *out) {
    int ret;
    __asm__ volatile("int $0x80"
        : "=a"(ret)
        : "a"((long)SYS_AUDIO_INFO), "D"((long)out)
        : "rcx", "r11", "memory");
    return ret;
}

/* Send an ioctl to the AC97 driver.  'request' is one of AUDIO_IOCTL_*;
 * 'value' is written to the kernel's audio settings and takes effect
 * immediately (the DMA engine is restarted as needed). */
static inline int audio_ioctl(int request, unsigned int value) {
    int ret;
    __asm__ volatile("int $0x80"
        : "=a"(ret)
        : "a"((long)SYS_AUDIO_IOCTL), "D"((long)request), "S"((long)&value)
        : "rcx", "r11", "memory");
    return ret;
}

#endif
