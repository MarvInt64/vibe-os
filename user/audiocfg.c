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
 * audiocfg — configure the AC97 audio driver at runtime
 *
 * Usage: audiocfg <command> <value>
 *
 * Commands:
 *   set_rate   <hz>      Change playback sample rate (e.g. 48000, 44100)
 *   set_volume <0-100>   Set master volume (0 = mute, 100 = max)
 *   info                 Print current driver state
 *
 * Uses the audio_ioctl() and audio_info() helpers from <audio.h> — no raw
 * syscall numbers or inline assembly needed in user-space code.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <audio.h>

static void print_info(void) {
    struct audio_info ai;
    if (audio_info(&ai) != 0) {
        fputs("audiocfg: audio_info failed\n", stderr);
        return;
    }
    if (!ai.present) {
        puts("Audio device: not present");
        return;
    }
    printf("Audio device : present (%04x:%04x)\n", ai.vendor_id, ai.device_id);
    printf("Sample rate  : %u Hz\n",  ai.sample_rate);
    printf("DMA ring     : %u buffers x %u bytes\n", ai.bd_count, ai.bd_bytes);
    printf("Ring usage   : %u / %u bytes\n", ai.ring_used, ai.ring_size);
    printf("Underruns    : %u\n", ai.underruns);
    printf("Engine       : CIV=%u  LVI=%u  SR=0x%x\n", ai.civ, ai.lvi, ai.sr);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fputs("Usage: audiocfg <command> [value]\n"
              "Commands: info, set_rate <hz>, set_volume <0-100>\n", stderr);
        return 1;
    }

    if (strcmp(argv[1], "info") == 0) {
        print_info();
        return 0;
    }

    /* All remaining commands need a numeric value. */
    if (argc < 3) {
        fprintf(stderr, "audiocfg: '%s' requires a value\n", argv[1]);
        return 1;
    }

    unsigned int val = (unsigned int)atoi(argv[2]);
    int request;

    if      (strcmp(argv[1], "set_rate")   == 0) request = AUDIO_IOCTL_SET_RATE;
    else if (strcmp(argv[1], "set_volume") == 0) request = AUDIO_IOCTL_SET_VOLUME;
    else {
        fprintf(stderr, "audiocfg: unknown command '%s'\n", argv[1]);
        return 1;
    }

    if (audio_ioctl(request, val) == 0) {
        printf("audiocfg: %s set to %u\n", argv[1], val);
    } else {
        fprintf(stderr, "audiocfg: %s failed\n", argv[1]);
        return 1;
    }
    return 0;
}
