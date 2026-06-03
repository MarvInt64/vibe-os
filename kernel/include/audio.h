#ifndef VIBEOS_AUDIO_H
#define VIBEOS_AUDIO_H

#include "types.h"

/* AC97 Audio Controller (Intel 82801AA / QEMU) */

#define AC97_VENDOR_ID  0x8086u
#define AC97_DEVICE_ID  0x2415u

/* Mixer registers (BAR0 = NAM, 16-bit I/O) */
#define AC97_MIX_RESET             0x00u
#define AC97_MIX_MASTER_VOL        0x02u
#define AC97_MIX_PCM_VOL           0x18u
#define AC97_MIX_PCM_FRONT_DAC_RATE 0x2Cu

/* Bus master registers (BAR1 = NABM, offsets from QEMU ac97.c) */
#define AC97_BM_PCM_OUT_BASE 0x10u
#define AC97_BM_PCM_OUT_CIV  0x14u
#define AC97_BM_PCM_OUT_LVI  0x15u
#define AC97_BM_PCM_OUT_SR   0x16u
#define AC97_BM_PCM_OUT_PICB 0x18u
#define AC97_BM_PCM_OUT_PIV  0x1Au
#define AC97_BM_PCM_OUT_CR   0x1Bu

#define AC97_SR_DCH   0x01u
#define AC97_SR_CELV  0x02u
#define AC97_SR_LVBCI 0x04u
#define AC97_SR_BCIS  0x08u
#define AC97_SR_FIFOE 0x10u

#define AC97_CR_RPBM  0x01u
#define AC97_CR_RR    0x02u
#define AC97_CR_LVBIE 0x04u
#define AC97_CR_FEIE  0x08u
#define AC97_CR_IOCE  0x10u

/* BD control bits — QEMU ac97.c:
 *   bits 0-15: length (in 2-byte PICB units)
 *   bit 30:    BUP  (Buffer Underrun Policy)
 *   bit 31:    IOC  (Interrupt On Completion) */
#define AC97_BD_IOC  0x80000000u
#define AC97_BD_BUP  0x40000000u

#define AC97_PLAYBACK_RATE  48000
#define AC97_SAMPLE_SIZE    4       /* 16-bit stereo = 4 bytes */
#define AC97_FRAME_SIZE     2       /* 2 bytes per channel sample (PICB unit) */
#define AC97_BD_COUNT       32
#define AC97_BD_SAMPLES     256    /* PICB value: 512 bytes / 2 */
#define AC97_BD_BYTES       (AC97_BD_SAMPLES * AC97_FRAME_SIZE)

struct ac97_buffer_descriptor {
    uint32_t pointer;
    uint32_t control;
} __attribute__((packed));

/* Per-process voice mixer: each process that submits audio gets an
 * independent ring buffer.  audio_tick() sums all active voices together
 * before feeding the DMA engine so multiple apps play simultaneously. */
#define AUDIO_VOICES     4      /* max simultaneous audio-producing processes */
#define AUDIO_RING_SIZE  32768  /* per-voice ring: ~170 ms at 48kHz stereo 16-bit */

/* Diagnostic snapshot of the audio device + driver state. Mirrored byte-for-byte
 * by struct audio_info in user/libc/include/audio.h (SYS_AUDIO_INFO). */
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

void audio_init(void);
/* Write PCM for the given process pid.  Each pid gets its own voice slot so
 * multiple processes can play audio simultaneously without corruption. */
int  audio_write(uint32_t pid, const void *data, uint32_t bytes);
/* Release the voice held by pid (called when a process exits). */
void audio_release_voice(uint32_t pid);
void audio_tick(void);
int  audio_present(void);
void audio_get_info(struct audio_info *info);

#define AUDIO_IOCTL_SET_RATE         0
#define AUDIO_IOCTL_SET_BUFFER_SIZE  1
#define AUDIO_IOCTL_SET_BUFFER_COUNT 2
#define AUDIO_IOCTL_SET_VOLUME       3
int  audio_ioctl(int request, void *arg);

#endif
