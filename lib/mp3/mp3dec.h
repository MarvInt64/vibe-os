/*
 * mp3dec.h — MPEG-1 Layer 3 decoder public API for VibeOS.
 *
 * MIT License
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
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * Usage:
 *
 *   mp3dec_t dec;
 *   mp3dec_init(&dec);
 *
 *   // feed compressed data in chunks
 *   int consumed = mp3dec_feed(&dec, compressed, len);
 *
 *   // decode one frame at a time
 *   int16_t pcm[MP3DEC_MAX_SAMPLES * 2];
 *   int sr, ch, br;
 *   int n = mp3dec_decode(&dec, pcm, &sr, &ch, &br);
 *   // n == 1152 (stereo) or 576 (mono granule — but we always return full frame)
 *   // interleaved: pcm[0]=L, pcm[1]=R, pcm[2]=L, ...
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * Supported:
 *   MPEG-1, Layer 3 (standard MP3), 44100 / 48000 / 32000 Hz.
 *   Stereo, joint stereo (MS + intensity), mono.
 *   Bitrates 32..320 kbps.
 *
 * Not supported:
 *   MPEG-2 / 2.5 LSF, Layer 1 / 2, free-format bitstreams, ID3v2 tags
 *   (caller must skip them), encoder padding (gapless playback).
 */

#ifndef MP3DEC_H
#define MP3DEC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum PCM samples per decoded frame (1152 stereo pairs). */
#define MP3DEC_MAX_SAMPLES 1152

/* ── Opaque state ──────────────────────────────────────────────────────────── */

/* Forward declaration; actual struct is defined in mp3dec.c.
 * Callers must not access any field directly. */
typedef struct mp3dec mp3dec_t;

/* Returns the sizeof the state struct so callers can allocate correctly
 * without needing to see the full struct definition. */
size_t mp3dec_size(void);

/* Initialise (or re-initialise after a seek) the decoder state.
 * dec must point to at least mp3dec_size() bytes of writable memory. */
void   mp3dec_init(mp3dec_t *dec);

/* ── Data feeding ──────────────────────────────────────────────────────────── */

/* Feed up to len bytes of compressed MP3 data into the decoder's input buffer.
 * Returns the number of bytes actually consumed (0..len).  Call repeatedly
 * until the buffer is full, then call mp3dec_decode(). */
int    mp3dec_feed(mp3dec_t *dec, const uint8_t *data, int len);

/* ── Decoding ──────────────────────────────────────────────────────────────── */

/* Attempt to decode one complete MP3 frame (2 granules × 576 samples = 1152
 * samples per channel) from the internal buffer.
 *
 * out[]        — caller-provided output; must hold at least
 *                MP3DEC_MAX_SAMPLES * 2 int16_t values (interleaved L,R).
 *                For mono, only samples[0..1151] are written (no duplication).
 *
 * info_sample_rate — filled with the decoded sample rate (32000/44100/48000).
 *                    Pass NULL to ignore.
 * info_channels    — filled with channel count (1 or 2).  Pass NULL to ignore.
 * info_bitrate     — filled with bitrate in kbps.  Pass NULL to ignore.
 *
 * Returns:
 *   > 0  — number of int16_t samples written to out[]
 *            (1152 * channels, i.e. 2304 for stereo, 1152 for mono).
 *     0  — not enough data yet; feed more bytes and try again.
 *    -1  — sync lost or corrupt frame; decoder re-syncs automatically.
 */
int    mp3dec_decode(mp3dec_t *dec, int16_t *out,
                     int *info_sample_rate, int *info_channels, int *info_bitrate);

#ifdef __cplusplus
}
#endif

#endif /* MP3DEC_H */
