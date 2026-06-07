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

#ifdef ARCH_ARM64
/* arm64: drive the virtio-sound device via syscalls (svc #0, x8=number).
 * The kernel returns bytes-accepted (0 when the tx ring is full → the caller's
 * blocking flush yields and retries, pacing playback to real time).  With no
 * audio device the kernel accepts-and-discards, so writers never hang. */
static inline int audio_write(const void *data, unsigned long bytes) {
    register long x8 __asm__("x8") = SYS_AUDIO_WRITE;
    register long x0 __asm__("x0") = (long)data;
    register long x1 __asm__("x1") = (long)bytes;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1) : "memory");
    return (int)x0;
}
static inline int audio_info(struct audio_info *out) {
    register long x8 __asm__("x8") = SYS_AUDIO_INFO;
    register long x0 __asm__("x0") = (long)out;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory");
    return (int)x0;
}
static inline int audio_ioctl(int request, unsigned int value) {
    register long x8 __asm__("x8") = SYS_AUDIO_IOCTL;
    register long x0 __asm__("x0") = (long)request;
    register long x1 __asm__("x1") = (long)value;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1) : "memory");
    return (int)x0;
}
#else
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

static inline int audio_ioctl(int request, unsigned int value) {
    int ret;
    __asm__ volatile("int $0x80"
        : "=a"(ret)
        : "a"((long)SYS_AUDIO_IOCTL), "D"((long)request), "S"((long)&value)
        : "rcx", "r11", "memory");
    return ret;
}
#endif /* ARCH_ARM64 */

#endif
