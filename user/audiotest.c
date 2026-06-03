/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Marvin Kicha <https://github.com/MarvInt64/> */

/* audiotest — emit a clean 440 Hz sine at 48000 Hz / 16-bit / stereo and
 * print a diagnostic dump of the AC97 device + driver state. */
#include <stdio.h>
#include <unistd.h>
#include "audio.h"

#define RATE      48000
#define FREQ      440
#define DURATION  3              /* seconds */
#define AMP       9000           /* peak amplitude (~28% of full scale) */

#define CHUNK_FRAMES 512         /* stereo frames submitted per chunk */
static short g_buf[CHUNK_FRAMES * 2];

/* Parabolic sine approximation (no libm): for phase 0..65535 = one full cycle,
 * returns a value in [-AMP, AMP]. sin(x) ~= 4x(N-|x|)/N^2 over [-pi,pi]. */
static short isin(unsigned int phase) {
    const long N = 32768;        /* pi */
    long x = (long)(phase & 0xFFFFu) - N;   /* -pi..pi */
    long absx = x < 0 ? -x : x;
    long long y = (long long)4 * x * (N - absx);   /* in [-N^2, N^2] */
    return (short)(((long long)AMP * y) / ((long long)N * N));
}

static void print_audio_info(void) {
    struct audio_info ai;
    if (audio_info(&ai) != 0) {
        printf("audiotest: SYS_AUDIO_INFO failed\n");
        return;
    }
    printf("=== Audio device ===\n");
    if (!ai.present) {
        printf("  status      : NO AC97 device found (silent)\n");
        printf("  hint        : start QEMU with  -device AC97  (or -soundhw ac97)\n");
        return;
    }
    printf("  status      : present\n");
    printf("  pci         : bus %u slot %u  %04x:%04x (AC97)\n",
           ai.pci_bus, ai.pci_slot, ai.vendor_id, ai.device_id);
    printf("  driver      : AC97 / Intel 82801AA, BDL DMA\n");
    printf("  mixer (NAM) : io 0x%x\n", ai.mixer_base);
    printf("  busmstr(BM) : io 0x%x\n", ai.bm_base);
    printf("  format      : %u Hz, %u-bit, %u ch\n",
           ai.sample_rate, ai.bits, ai.channels);
    printf("  dma ring    : %u buffers x %u bytes\n", ai.bd_count, ai.bd_bytes);
    printf("  sw ring     : %u/%u bytes used (%u free)\n",
           ai.ring_used, ai.ring_size, ai.ring_free);
    printf("  engine      : CIV=%u LVI=%u SR=0x%x\n", ai.civ, ai.lvi, ai.sr);
    printf("  underruns   : %u\n", ai.underruns);
    printf("====================\n");
}

static void write_all(const void *buf, unsigned long bytes) {
    const unsigned char *p = (const unsigned char *)buf;
    unsigned long remaining = bytes;
    while (remaining > 0) {
        int written = audio_write(p, remaining);
        if (written > 0) {
            p += written;
            remaining -= (unsigned long)written;
        }
        sched_yield_();   /* let the kernel drain the ring to the DAC */
    }
}

int main(void) {
    print_audio_info();

    struct audio_info ai;
    if (audio_info(&ai) == 0 && !ai.present) {
        printf("audiotest: no audio device — nothing to play\n");
        return 1;
    }

    const unsigned int phase_inc = (unsigned int)(((unsigned long)65536 * FREQ) / RATE);
    unsigned int phase = 0;
    int total_frames = RATE * DURATION;
    int played = 0;

    printf("audiotest: playing %d Hz sine for %d s...\n", FREQ, DURATION);

    while (played < total_frames) {
        int n = CHUNK_FRAMES;
        if (n > total_frames - played) n = total_frames - played;
        int i;
        for (i = 0; i < n; i++) {
            short v = isin(phase);
            g_buf[i * 2]     = v;   /* left  */
            g_buf[i * 2 + 1] = v;   /* right */
            phase += phase_inc;
        }
        write_all(g_buf, (unsigned long)n * 4);
        played += n;
    }

    /* Report final driver counters so we can tell if the stream starved. */
    print_audio_info();
    printf("audiotest: done\n");
    return 0;
}
