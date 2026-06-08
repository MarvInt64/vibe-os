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

/*
 * mp3play — play an MP3 file through the VibeOS audio system
 *
 * Usage: mp3play <file.mp3>
 *
 * Reads the file in 4 KB chunks, feeds them to the decoder, and writes the
 * decoded int16 PCM to the kernel audio ring buffer via audio_write().
 * The write is blocking (spin-yield on full ring) so this process naturally
 * runs at real-time pace matching the AC97 drain rate.
 *
 * Supports MPEG-1 Layer 3 only (standard .mp3 files).
 * Sample-rate conversion is not performed: the file must be 48000 Hz for
 * correct pitch.  44100 Hz files will play slightly fast.  A future version
 * could resample or configure the AC97 sample-rate register via audiocfg.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <audio.h>
#include <mp3dec.h>

#ifdef ARCH_ARM64
static void do_yield(void) {
    register long x8 __asm__("x8") = 3; /* SYS_YIELD */
    __asm__ volatile("svc #0" : "+r"(x8) : : "memory");
}
#else
static void do_yield(void) {
    __asm__ volatile("int $0x80" : : "a"(3) : "memory");
}
#endif

/* Read buffer: larger means fewer syscalls; must be at least one MP3 frame
 * (~417-1441 bytes) but we use 4 KB for efficiency. */
#define READ_BUF  4096

/* PCM output: one stereo frame is 1152 sample pairs = 4608 bytes. */
#define PCM_BUFSZ (MP3DEC_MAX_SAMPLES * 2)   /* int16 samples, interleaved */

/*
 * Blocking audio write: keeps trying until all bytes are accepted so the
 * main loop is naturally throttled to the AC97 drain rate (~192 kB/s at
 * 48kHz stereo 16-bit).  Without this the ring would overflow and silence
 * would corrupt the stream.
 */
static void audio_write_all(const int16_t *buf, int samples) {
    const unsigned char *p = (const unsigned char *)buf;
    unsigned long remaining = (unsigned long)samples * sizeof(int16_t);
    while (remaining > 0) {
        int written = audio_write(p, (unsigned int)remaining);
        if (written > 0) {
            p         += written;
            remaining -= (unsigned long)written;
        } else {
            /* Ring full — yield CPU so audio_tick() can drain it. */
            do_yield();
        }
    }
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fputs("Usage: mp3play <file.mp3>\n", stderr);
        return 1;
    }

    /* Check that the audio device is present before trying to play. */
    struct audio_info ai;
    if (audio_info(&ai) == 0 && !ai.present) {
        fputs("mp3play: no audio device found\n", stderr);
        return 1;
    }

    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "mp3play: cannot open '%s'\n", argv[1]);
        return 1;
    }

    /* Decoder state lives on the stack — no heap allocation needed. */
    static mp3dec_t dec;   /* static so it doesn't overflow a small stack */
    mp3dec_init(&dec);

    static unsigned char in_buf[READ_BUF];
    static int16_t       pcm[PCM_BUFSZ];

    int sr = 0, ch = 0, br = 0;
    int first_frame = 1;

    printf("mp3play: playing '%s'\n", argv[1]);

    for (;;) {
        /* Fill the read buffer. */
        ssize_t n = read(fd, in_buf, sizeof(in_buf));
        if (n <= 0) break;   /* EOF or error */

        /* Feed compressed bytes to the decoder's internal buffer. */
        int consumed = mp3dec_feed(&dec, in_buf, (int)n);
        if (consumed < n) {
            /* Decoder buffer was full — this shouldn't happen with 4 KB reads
             * and an 8 KB internal buf, but handle gracefully by re-seeking. */
            lseek(fd, (off_t)(consumed - n), SEEK_CUR);
        }

        /* Drain all complete frames out of the decoder. */
        int samples;
        while ((samples = mp3dec_decode(&dec, pcm, &sr, &ch, &br)) > 0) {
            if (first_frame) {
                printf("mp3play: %d Hz, %d ch, %d kbps\n", sr, ch, br);
                /* Configure the AC97 to the file's sample rate so playback
                 * pitch is correct.  The driver supports variable-rate audio;
                 * without this a 44.1 kHz file would play ~9% too fast on the
                 * default 48 kHz engine. */
                if (sr > 0) audio_ioctl(AUDIO_IOCTL_SET_RATE, (unsigned int)sr);
                first_frame = 0;
            }
            /* samples = number of int16 values written (already interleaved). */
            audio_write_all(pcm, samples);
        }
        if (samples < 0) {
            fputs("mp3play: decoder error\n", stderr);
            break;
        }
    }

    /* Flush any remaining buffered data after EOF. */
    int samples;
    while ((samples = mp3dec_decode(&dec, pcm, &sr, &ch, &br)) > 0)
        audio_write_all(pcm, samples);

    close(fd);

    /* Restore the AC97 to the system default rate so other apps (DOOM,
     * audiotest) that assume 48 kHz are not left playing at the wrong pitch. */
    if (sr > 0 && sr != 48000) audio_ioctl(AUDIO_IOCTL_SET_RATE, 48000);

    printf("mp3play: done\n");
    return 0;
}
