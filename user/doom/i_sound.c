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

/* i_sound.c — AC97 PCM sound for VibeOS DOOM port.
 *
 * Converts DOOM's 8-bit unsigned mono WAD sound lumps to 16-bit signed stereo
 * at 48000 Hz and feeds them to the kernel audio driver.
 *
 * Channel mixing: up to 8 simultaneous sound effects are mixed into a small
 * accumulation buffer; I_UpdateSound flushes it to the kernel each frame.
 */

#include "i_sound.h"
#include "doomtype.h"
#include "doomstat.h"
#include "m_misc.h"
#include "w_wad.h"
#include "z_zone.h"
#include "audio.h"

#define NUM_CHANNELS   8
#define MIX_RATE       48000
/* 48000 Hz / 35 tics/sec ≈ 1371.4 → 1372 to match DOOM's tic rate */
#define FRAME_SAMPLES  ((MIX_RATE + 17) / 35)

static boolean sound_initialized = false;
static boolean use_sfx_prefix;

/* ---- DOOM-required globals (normally in i_sound.c) ---- */
int snd_sfxdevice = SNDDEVICE_NONE;
int snd_musicdevice = SNDDEVICE_NONE;
int snd_samplerate = MIX_RATE;
int snd_cachesize = 64 * 1024 * 1024;
int snd_maxslicetime_ms = 28;
char *snd_musiccmd = "";

/* ---- Per-channel state ---- */
typedef struct {
    boolean active;
    int     volume;      /* 0-127 */
    int     sep;         /* 0-254 stereo separation */
    int     pos;         /* current sample position in expanded data */
    int     len;         /* total samples in expanded data */
    int16_t *data;       /* expanded 16-bit signed mono data */
} channel_t;

static channel_t g_channels[NUM_CHANNELS];

/* ---- Cached expanded sounds ---- */
typedef struct {
    sfxinfo_t *sfxinfo;
    int16_t *data;
    int      len;       /* sample count (mono) */
    int      samplerate; /* original sample rate */
    int      use_count;
} cached_sound_t;

#define MAX_CACHED_SOUNDS 64
static cached_sound_t g_cache[MAX_CACHED_SOUNDS];
static int g_cache_count = 0;

/* ---- Accumulation buffer (one frame of stereo mixing) ---- */
static int16_t g_mix_buf[FRAME_SAMPLES * 2];
static int g_mix_pos = 0;

/* ---- Helpers ---- */
static int16_t clamp16(int32_t v) {
    if (v < -32768) return -32768;
    if (v > 32767) return 32767;
    return (int16_t)v;
}

static int find_cache(sfxinfo_t *sfx) {
    int i;
    for (i = 0; i < g_cache_count; i++) {
        if (g_cache[i].sfxinfo == sfx) return i;
    }
    return -1;
}

/* Expand a WAD sound lump to 16-bit signed mono at MIX_RATE.
 * The DMX format: offset 0=0x03, 1=0x00, 2-3=rate(LE), 4-7=len(LE),
 * then skip 16 bytes of data, skip last 16 bytes of data. */
static int expand_sound(sfxinfo_t *sfxinfo, int16_t **out) {
    int lumpnum = sfxinfo->lumpnum;
    if (lumpnum < 0) return 0;

    unsigned int lumplen = W_LumpLength(lumpnum);
    byte *data = W_CacheLumpNum(lumpnum, PU_STATIC);
    if (!data || lumplen < 8 || data[0] != 0x03 || data[1] != 0x00) {
        if (data) W_ReleaseLumpNum(lumpnum);
        return 0;
    }

    int samplerate = (data[3] << 8) | data[2];
    unsigned int length = (data[7] << 24) | (data[6] << 16) | (data[5] << 8) | data[4];
    if (length > lumplen - 8 || length <= 48) {
        W_ReleaseLumpNum(lumpnum);
        return 0;
    }

    data += 16;
    length -= 32;

    byte *samples = data + 8;
    length -= 8;
    if ((int)length <= 0) {
        W_ReleaseLumpNum(lumpnum);
        return 0;
    }

    /* Simple sample-rate conversion to MIX_RATE */
    int out_len = (int)(((uint64_t)length * MIX_RATE) / (uint32_t)samplerate);
    if (out_len < 1) out_len = 1;

    int16_t *expanded = Z_Malloc(out_len * sizeof(int16_t), PU_STATIC, 0);
    if (!expanded) {
        W_ReleaseLumpNum(lumpnum);
        return 0;
    }

    int expand_ratio = (length << 8) / out_len;
    int i;
    for (i = 0; i < out_len; i++) {
        int src = (i * expand_ratio) >> 8;
        if (src >= (int)length) src = (int)length - 1;
        int32_t s = (int32_t)samples[src];
        s = ((s << 8) | s) - 32768;
        expanded[i] = (int16_t)s;
    }

    W_ReleaseLumpNum(lumpnum);

    *out = expanded;
    return out_len;
}

/* ---- DOOM I_Sound interface ---- */
void I_InitSound(boolean sfx_prefix) {
    use_sfx_prefix = sfx_prefix;
    int i;
    for (i = 0; i < NUM_CHANNELS; i++) {
        g_channels[i].active = false;
    }
    sound_initialized = true;
}

void I_ShutdownSound(void) {
    int i;
    for (i = 0; i < g_cache_count; i++) {
        if (g_cache[i].data) Z_Free(g_cache[i].data);
    }
    g_cache_count = 0;
    sound_initialized = false;
}

int I_GetSfxLumpNum(sfxinfo_t *sfx) {
    if (sfx->lumpnum >= 0) return sfx->lumpnum;

    char buf[16];
    if (use_sfx_prefix) {
        M_snprintf(buf, sizeof(buf), "ds%s", sfx->name);
    } else {
        M_snprintf(buf, sizeof(buf), "%s", sfx->name);
    }
    int lump = W_CheckNumForName(buf);
    if (lump < 0 && use_sfx_prefix) {
        M_snprintf(buf, sizeof(buf), "dp%s", sfx->name);
        lump = W_CheckNumForName(buf);
    }
    sfx->lumpnum = lump;
    return lump;
}

void I_UpdateSound(void) {
    if (!sound_initialized) return;

    /* Flush any buffered mixed samples to the kernel.
     * g_mix_buf is stereo: g_mix_pos frames × 2 channels × 2 bytes each. */
    if (g_mix_pos > 0) {
        /* Blocking write: yield until all bytes are accepted so DOOM's game
         * loop is naturally throttled to the AC97 drain rate (192 kB/s).
         * Without this, DOOM runs faster than real-time and the ring overflows,
         * truncating audio and producing noise. */
        const unsigned char *buf = (const unsigned char *)g_mix_buf;
        unsigned long remaining = (unsigned long)(g_mix_pos * 2 * sizeof(int16_t));
        while (remaining > 0) {
            int written = audio_write(buf, remaining);
            if (written > 0) {
                buf += written;
                remaining -= (unsigned long)written;
            } else {
                /* SYS_YIELD = 3 — yield CPU so the kernel can drain the ring */
#ifdef ARCH_ARM64
                register long x8 __asm__("x8") = 3;
                __asm__ volatile("svc #0" : : "r"(x8) : "memory");
#else
                __asm__ volatile("int $0x80" : : "a"(3) : "memory");
#endif
            }
        }
        g_mix_pos = 0;
    }

    /* Mix next frame of samples from active channels */
    int i, s;
    int32_t left, right;

    /* Clear mix buffer */
    for (s = 0; s < FRAME_SAMPLES * 2; s++) {
        g_mix_buf[s] = 0;
    }

    for (i = 0; i < NUM_CHANNELS; i++) {
        channel_t *ch = &g_channels[i];
        if (!ch->active || !ch->data) continue;

        int remaining = ch->len - ch->pos;
        int count = FRAME_SAMPLES;
        if (count > remaining) count = remaining;

        int vol_left  = ch->volume * (254 - ch->sep) / 254;
        int vol_right = ch->volume * ch->sep / 254;

        for (s = 0; s < count; s++) {
            int32_t sample = ch->data[ch->pos + s];
            g_mix_buf[s * 2]     = clamp16((int32_t)g_mix_buf[s * 2]     + (sample * vol_left / 127));
            g_mix_buf[s * 2 + 1] = clamp16((int32_t)g_mix_buf[s * 2 + 1] + (sample * vol_right / 127));
        }

        ch->pos += count;
        if (ch->pos >= ch->len) {
            ch->active = false;
        }
    }

    g_mix_pos = FRAME_SAMPLES;
}

void I_UpdateSoundParams(int channel, int vol, int sep) {
    if (channel < 0 || channel >= NUM_CHANNELS) return;
    g_channels[channel].volume = vol;
    g_channels[channel].sep = sep;
}

int I_StartSound(sfxinfo_t *sfxinfo, int channel, int vol, int sep) {
    if (!sound_initialized) return -1;
    if (channel < 0 || channel >= NUM_CHANNELS) return -1;

    int ci = find_cache(sfxinfo);
    if (ci < 0 && g_cache_count < MAX_CACHED_SOUNDS) {
        int16_t *data = 0;
        int len = expand_sound(sfxinfo, &data);
        if (len <= 0 || !data) return -1;
        ci = g_cache_count++;
        g_cache[ci].sfxinfo = sfxinfo;
        g_cache[ci].data = data;
        g_cache[ci].len = len;
        g_cache[ci].samplerate = MIX_RATE;
        g_cache[ci].use_count = 0;
    }
    if (ci < 0) return -1;

    sfxinfo->driver_data = &g_cache[ci];
    g_cache[ci].use_count++;

    g_channels[channel].active = true;
    g_channels[channel].volume = vol;
    g_channels[channel].sep = sep;
    g_channels[channel].pos = 0;
    g_channels[channel].len = g_cache[ci].len;
    g_channels[channel].data = g_cache[ci].data;

    return channel;
}

void I_StopSound(int channel) {
    if (channel >= 0 && channel < NUM_CHANNELS) {
        g_channels[channel].active = false;
    }
}

boolean I_SoundIsPlaying(int channel) {
    if (channel < 0 || channel >= NUM_CHANNELS) return false;
    return g_channels[channel].active;
}

void I_PrecacheSounds(sfxinfo_t *sounds, int num_sounds) {
    int i;
    for (i = 0; i < num_sounds; i++) {
        I_GetSfxLumpNum(&sounds[i]);
    }
}

/* ---- Music stubs (no MIDI support yet) ---- */
void I_InitMusic(void) {}
void I_ShutdownMusic(void) {}
void I_SetMusicVolume(int v) { (void)v; }
void I_PauseSong(void) {}
void I_ResumeSong(void) {}
void *I_RegisterSong(void *d, int l) { (void)d; (void)l; return (void *)0; }
void I_UnRegisterSong(void *h) { (void)h; }
void I_PlaySong(void *h, boolean loop) { (void)h; (void)loop; }
void I_StopSong(void) {}
boolean I_MusicIsPlaying(void) { return false; }
void I_BindSoundVariables(void) {}
