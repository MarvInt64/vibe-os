/* Host test harness for the MP3 decoder — runs natively on macOS for fast
 * iteration (no QEMU).  Decodes the given file and prints per-frame peak. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "../lib/mp3/mp3dec.h"

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s file.mp3\n", argv[0]); return 1; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("open"); return 1; }

    static mp3dec_t dec;
    mp3dec_init(&dec);

    static unsigned char in[4096];
    static int16_t pcm[MP3DEC_MAX_SAMPLES * 2];
    int sr=0, ch=0, br=0, frame=0;
    long total_samples=0, total_peak=0;

    size_t n;
    while ((n = fread(in, 1, sizeof in, f)) > 0) {
        int consumed = mp3dec_feed(&dec, in, (int)n);
        (void)consumed;
        int s;
        while ((s = mp3dec_decode(&dec, pcm, &sr, &ch, &br)) > 0) {
            int peak = 0;
            for (int k = 0; k < s; k++) {
                int a = pcm[k] < 0 ? -pcm[k] : pcm[k];
                if (a > peak) peak = a;
            }
            if (frame < 20)
                printf("frame %d: peak=%d samples=%d sr=%d ch=%d br=%d\n",
                       frame, peak, s, sr, ch, br);
            total_peak += peak;
            total_samples += s;
            frame++;
        }
    }
    fclose(f);
    printf("--- %d frames, %ld samples, avg_peak=%ld ---\n",
           frame, total_samples, frame ? total_peak/frame : 0);
    return 0;
}
