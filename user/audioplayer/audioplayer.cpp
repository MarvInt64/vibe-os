// audioplayer — a VexUI music player for VibeOS.
//
// A single full-window canvas hosts a hand-rendered, dark "glass" UI modelled
// on the reference mockup: album art, track metadata, a colourful mirrored
// waveform, a glowing progress bar and a rounded transport bar (shuffle /
// previous / play-pause / next / repeat / volume).
//
// Audio is decoded and streamed on a background thread (mp3dec + the AC97
// audio syscalls), exactly like the browser's <audio> worker; the UI thread
// only reads a handful of atomics (elapsed samples, peak level, state) to
// drive the animation, so rendering never blocks on decoding.
//
//   /bin/audioplayer [file.mp3]      (defaults to /music/becorbal-town.mp3)

#include "vexui.h"
#include <vibeos.h>
#include <audio.h>
#include <mp3dec.h>
#include <svg.h>

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <atomic>
#include <thread>

// ───────────────────────────────────────────────────────────────────────────
// Canvas geometry
// ───────────────────────────────────────────────────────────────────────────
static constexpr int DEFAULT_W    = 720;
static constexpr int DEFAULT_H    = 460;
static constexpr int MAX_BARS     = 160;    // waveform bar slots

// The whole UI is built from retained VexUI widgets (labels, SVG icons,
// sliders); the ONLY hand-drawn pixel surface is the waveform, which lives in
// its own canvas widget with a matching stride. A small second canvas holds the
// procedural album art (rendered once).
static constexpr int WAVE_MAX_W = 720;      // waveform canvas backing size
static constexpr int WAVE_MAX_H = 140;
static uint32_t g_wave[WAVE_MAX_W * WAVE_MAX_H];
static constexpr int ALBUM_S = 92;          // album-art tile side
static uint32_t g_album[ALBUM_S * ALBUM_S];

// ───────────────────────────────────────────────────────────────────────────
// SVGs for player icons
// ───────────────────────────────────────────────────────────────────────────
static const char* SVG_SHUFFLE = 
    "<svg viewBox=\"0 0 24 24\">"
    "<path d=\"M16 3h5v5M4 20L21 3M21 16v5h-5M4 4l5 5M15 15l6 6\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"2\"/>"
    "</svg>";

static const char* SVG_PREV = 
    "<svg viewBox=\"0 0 24 24\">"
    "<path d=\"M19 20L9 12L19 4Z M4 5h2v14H4Z\" fill=\"currentColor\"/>"
    "</svg>";

static const char* SVG_NEXT = 
    "<svg viewBox=\"0 0 24 24\">"
    "<path d=\"M5 4l10 8l-10 8Z M18 5h2v14h-2Z\" fill=\"currentColor\"/>"
    "</svg>";

// Arc-free repeat icon: two arrows forming a loop with sharp corners.
// Repeat: a horizontal loop with rounded corners + two arrowheads. libsvg has
// no arc support, so the rounded corners are cubic-bezier quarter circles.
static const char* SVG_REPEAT =
    "<svg viewBox=\"0 0 24 24\">"
    "<path d=\"M17 1L21 5L17 9\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"2\"/>"
    "<path d=\"M3 11V9C3 6.79 4.79 5 7 5H21\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"2\"/>"
    "<path d=\"M7 23L3 19L7 15\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"2\"/>"
    "<path d=\"M21 13V15C21 17.21 19.21 19 17 19H3\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"2\"/>"
    "</svg>";

// Speaker (lucide volume-2): cone + two concentric sound-wave arcs. Now that
// svg.c renders elliptical arcs, these use the original A commands directly.
static const char* SVG_SPEAKER =
    "<svg viewBox=\"0 0 24 24\">"
    "<path d=\"M11 5L6 9H2v6h4l5 4V5Z\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"2\"/>"
    "<path d=\"M15.54 8.46a5 5 0 0 1 0 7.07\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"2\"/>"
    "<path d=\"M19.07 4.93a10 10 0 0 1 0 14.14\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"2\"/>"
    "</svg>";

// Heart (lucide): the classic two-arc outline, rendered via real arc support.
static const char* SVG_HEART =
    "<svg viewBox=\"0 0 24 24\">"
    "<path d=\"M20.84 4.61a5.5 5.5 0 0 0-7.78 0L12 5.67l-1.06-1.06a5.5 5.5 0 0 0-7.78 7.78l1.06 1.06L12 21.23l7.78-7.78 1.06-1.06a5.5 5.5 0 0 0 0-7.78z\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"2\"/>"
    "</svg>";

// Play/pause buttons: the designer's glow SVG (radial bg glow, gradient ring,
// feDropShadow halo, gradient-filled glyph). Now that svg.c renders radial
// gradients + feDropShadow these draw exactly as authored. The pause variant
// reuses the same defs/ring and swaps the triangle for two bars.
static const char* SVG_DEFS =
    "<defs>"
    "<radialGradient id=\"bgGlow\" cx=\"0\" cy=\"0\" r=\"1\" gradientUnits=\"userSpaceOnUse\" gradientTransform=\"translate(64 64) scale(52)\">"
      "<stop offset=\"0\" stop-color=\"#4DA3FF\" stop-opacity=\"0.10\"/>"
      "<stop offset=\"0.72\" stop-color=\"#4DA3FF\" stop-opacity=\"0.04\"/>"
      "<stop offset=\"1\" stop-color=\"#4DA3FF\" stop-opacity=\"0\"/>"
    "</radialGradient>"
    "<linearGradient id=\"ringStroke\" x1=\"30\" y1=\"24\" x2=\"98\" y2=\"104\" gradientUnits=\"userSpaceOnUse\">"
      "<stop offset=\"0\" stop-color=\"#7CC7FF\"/>"
      "<stop offset=\"0.55\" stop-color=\"#4DA3FF\"/>"
      "<stop offset=\"1\" stop-color=\"#9B7CFF\"/>"
    "</linearGradient>"
    "<linearGradient id=\"playFill\" x1=\"52\" y1=\"44\" x2=\"80\" y2=\"84\" gradientUnits=\"userSpaceOnUse\">"
      "<stop offset=\"0\" stop-color=\"#F3F8FD\"/>"
      "<stop offset=\"1\" stop-color=\"#BED8F3\"/>"
    "</linearGradient>"
    "<filter id=\"outerGlow\">"
      "<feDropShadow dx=\"0\" dy=\"0\" stdDeviation=\"7\" flood-color=\"#4DA3FF\" flood-opacity=\"0.55\"/>"
    "</filter>"
    "</defs>"
    "<circle cx=\"64\" cy=\"64\" r=\"52\" fill=\"url(#bgGlow)\"/>"
    "<g filter=\"url(#outerGlow)\">"
      "<circle cx=\"64\" cy=\"64\" r=\"32.5\" fill=\"none\" stroke=\"url(#ringStroke)\" stroke-width=\"2.5\"/>"
    "</g>";

// Glyphs assembled with SVG_DEFS at runtime (see draw_play_pause).
static const char* SVG_PLAY_GLYPH =
    "<path d=\"M55 46.5C55 45.32 56.32 44.6 57.32 45.25L79.7 59.92C80.61 60.52 81.16 61.53 81.16 62.62C81.16 63.7 80.61 64.72 79.7 65.32L57.32 79.98C56.32 80.63 55 79.92 55 78.74Z\" fill=\"url(#playFill)\"/>";
static const char* SVG_PAUSE_GLYPH =
    "<rect x=\"53\" y=\"47\" width=\"8\" height=\"34\" fill=\"url(#playFill)\"/>"
    "<rect x=\"67\" y=\"47\" width=\"8\" height=\"34\" fill=\"url(#playFill)\"/>";

static const char* SVG_DOTS = 
    "<svg viewBox=\"0 0 24 24\">"
    "<circle cx=\"12\" cy=\"12\" r=\"2\" fill=\"currentColor\"/>"
    "<circle cx=\"19\" cy=\"12\" r=\"2\" fill=\"currentColor\"/>"
    "<circle cx=\"5\" cy=\"12\" r=\"2\" fill=\"currentColor\"/>"
    "</svg>";

// ───────────────────────────────────────────────────────────────────────────
// Tiny freestanding trig (no libm). Good enough for layout / noise / glow.
// ───────────────────────────────────────────────────────────────────────────
static float ap_sin(float x) {
    constexpr float PI = 3.14159265f, TWO_PI = 6.28318531f;
    while (x < -PI) x += TWO_PI;
    while (x >  PI) x -= TWO_PI;
    const float x2 = x * x;            // 7th-order minimax, |err| < 2e-4
    return x * (0.9999966f + x2 * (-0.16664824f + x2 * (0.00830629f - x2 * 0.00018363f)));
}
static inline float ap_cos(float x) { return ap_sin(x + 1.57079633f); }
static inline float ap_clamp(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// ───────────────────────────────────────────────────────────────────────────
// Shared visualization data & Cooley-Tukey FFT implementation
// ───────────────────────────────────────────────────────────────────────────
// volatile forces the compiler to re-read each element from memory on every
// access, preventing the UI thread from seeing a stale cached copy of the
// samples that the decode worker wrote.
static volatile int16_t g_vis_samples[512] = {0};

struct ap_complex {
    float r, i;
    ap_complex(float r = 0.0f, float i = 0.0f) : r(r), i(i) {}
    ap_complex operator+(const ap_complex& b) const { return ap_complex(r + b.r, i + b.i); }
    ap_complex operator-(const ap_complex& b) const { return ap_complex(r - b.r, i - b.i); }
    ap_complex operator*(const ap_complex& b) const { return ap_complex(r * b.r - i * b.i, r * b.i + i * b.r); }
};

static void ap_fft(ap_complex *x, int n) {
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            ap_complex tmp = x[i];
            x[i] = x[j];
            x[j] = tmp;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        float angle = -2.0f * 3.14159265f / len;
        ap_complex wlen(ap_cos(angle), ap_sin(angle));
        for (int i = 0; i < n; i += len) {
            ap_complex w(1.0f, 0.0f);
            for (int j = 0; j < len / 2; ++j) {
                ap_complex u = x[i + j];
                ap_complex v = x[i + j + len / 2] * w;
                x[i + j] = u + v;
                x[i + j + len / 2] = u - v;
                w = w * wlen;
            }
        }
    }
}

static inline float ap_sqrt(float x) {
    if (x <= 0.0f) return 0.0f;
    int32_t i;
    memcpy(&i, &x, sizeof(i));
    i = 0x1fbd1df5 + (i >> 1);
    float val;
    memcpy(&val, &i, sizeof(val));
    val = 0.5f * (val + x / val);
    val = 0.5f * (val + x / val);
    return val;
}

// 2^x for x >= 0 — used for the logarithmic FFT-bin axis. Integer part via a
// shift, fractional part via a 3rd-order polynomial (|err| < 1e-3 on [0,1]).
static inline float ap_exp2(float x) {
    if (x < 0.0f) x = 0.0f;
    int   xi = (int)x;
    float xf = x - (float)xi;
    float p  = 1.0f + xf * (0.6931472f + xf * (0.2402265f + xf * 0.0558610f));
    return (float)(1 << xi) * p;
}

// ───────────────────────────────────────────────────────────────────────────
// Palette (0x00RRGGBB; the canvas is opaque so the alpha byte is unused)
// ───────────────────────────────────────────────────────────────────────────
// Background gradient harmonised with the VexUI window chrome (theme bg
// 0x15273c) so the canvas no longer reads as a different blue from the frame.
static constexpr uint32_t COL_BG       = 0x00142840;  // solid window background
static constexpr uint32_t COL_ACCENT   = 0x4aa8ff;  // azure accent (play / sliders)
static constexpr uint32_t COL_ACCENT_HI= 0x8ccbff;  // bright accent (handles, glow)
static constexpr uint32_t COL_TEXT     = 0xeef3fb;  // primary text
static constexpr uint32_t COL_TEXT_DIM = 0x8b9bb4;  // secondary text (artist/meta)
static constexpr uint32_t COL_ICON     = 0xc4d0de;  // idle transport icons (bright
                                                    // enough to read on the dark
                                                    // panel without a hover wash)
static constexpr uint32_t COL_WAVE_L   = 0x33d4e6;  // waveform gradient: left  (cyan)
static constexpr uint32_t COL_WAVE_M   = 0x4a86ff;  // waveform gradient: middle (blue)
static constexpr uint32_t COL_WAVE_R   = 0xb15cf0;  // waveform gradient: right (violet)

// ───────────────────────────────────────────────────────────────────────────
// Shared playback state — written by the decode worker, read by the UI thread.
// ───────────────────────────────────────────────────────────────────────────
static std::atomic<long> g_played{0};    // samples (per channel) played so far
static std::atomic<long> g_total{1};      // estimated total samples (per channel)
static std::atomic<int>  g_rate{44100};   // sample rate (Hz)
static std::atomic<int>  g_bitrate{0};    // kbps (for the metadata line)
static std::atomic<int>  g_level{0};      // recent peak amplitude, 0..100
static std::atomic<int>  g_volume{80};    // master volume, 0..100
static std::atomic<int>  g_seek{-1};      // UI→worker seek request, 0..1000 ‰
static std::atomic<bool> g_paused{false};
static std::atomic<bool> g_repeat{false};
static std::atomic<bool> g_shuffle{false};
static std::atomic<bool> g_stop{false};
static std::atomic<bool> g_restart{false}; // UI→worker "restart from 0" request



// Deterministic 1-D hash, used for visual sparkle effects.
static float hash01(int n) {
    n = (n << 13) ^ n;
    int m = (n * (n * n * 15731 + 789221) + 1376312589) & 0x7fffffff;
    return (float)m / 2147483647.0f;
}

// ───────────────────────────────────────────────────────────────────────────
// AudioPlayer — owns the window, canvas rendering and input handling.
// ───────────────────────────────────────────────────────────────────────────
class AudioPlayer {
public:
    static AudioPlayer *instance() { return instance_; }
    void run(const char *path);
    void on_tick();                       // called from the VexUI tick trampoline

    // Retained chrome widgets that interaction/state updates poke at runtime.
    vui_widget *play_ic_    = nullptr;
    vui_widget *shuffle_ic_ = nullptr;
    vui_widget *repeat_ic_  = nullptr;
    vui_widget *progress_   = nullptr;
    vui_widget *volume_     = nullptr;

private:
    static AudioPlayer *instance_;

    vui_window *win_ = nullptr;
    char  path_[256] = {};

    vui_widget *wave_canvas_ = nullptr;   // the spectrum
    vui_widget *title_lbl_ = nullptr, *artist_lbl_ = nullptr, *meta_lbl_ = nullptr;
    vui_widget *time_l_lbl_ = nullptr, *time_r_lbl_ = nullptr;

    int   wave_w_ = 0, wave_h_ = 0;       // waveform canvas size (own stride)
    int   tick_count_ = 0;
    float level_lp_ = 0.0f;
    float bar_heights_[MAX_BARS] = {};    // current displayed bar heights
    float bar_peak_[MAX_BARS]    = {};    // per-band running peak (AGC)

    // chrome-change tracking (labels/slider/icon only refreshed when these move)
    long last_sec_     = -1;
    bool last_paused_  = false;
    int  last_bitrate_ = -1;

    static uint32_t lerp_rgb(uint32_t a, uint32_t b, float t);
    static void fmt_time(long seconds, char *out);
    void draw_album_art();                // render once into g_album
    void draw_spectrum();                 // per-tick into g_wave
    void update_chrome();                 // refresh widgets whose value changed

    // ---- decode/stream worker (unchanged) ---------------------------------
    struct WorkerArgs { AudioPlayer *self; mp3dec_t *dec; int16_t *pcm; };
    static void worker(WorkerArgs *args);
    static bool write_all(const int16_t *pcm, int samples);
};

AudioPlayer *AudioPlayer::instance_ = nullptr;

// Self-contained alpha blend into an arbitrary RGBX buffer (the waveform/album
// canvases own their pixels; we no longer share a chrome canvas).
static inline void blend_px(uint32_t *buf, int stride, int w, int h,
                            int x, int y, uint32_t c, int a) {
    if ((unsigned)x >= (unsigned)w || (unsigned)y >= (unsigned)h || a <= 0) return;
    uint32_t &d = buf[(long)y * stride + x];
    if (a >= 255) { d = c & 0xffffff; return; }
    uint32_t r = ((((c >> 16) & 0xff) * a) + (((d >> 16) & 0xff) * (255 - a))) / 255;
    uint32_t g = ((((c >> 8)  & 0xff) * a) + (((d >> 8)  & 0xff) * (255 - a))) / 255;
    uint32_t b = ((((c)       & 0xff) * a) + (((d)       & 0xff) * (255 - a))) / 255;
    d = (r << 16) | (g << 8) | b;
}

// Assemble the glow play/pause SVG (shared defs + glyph) into a reusable buffer.
static const char *glow_svg(bool play_icon) {
    static char buf[1600];
    snprintf(buf, sizeof buf, "<svg viewBox=\"0 0 128 128\">%s%s</svg>",
             SVG_DEFS, play_icon ? SVG_PLAY_GLYPH : SVG_PAUSE_GLYPH);
    return buf;
}

// ===========================================================================
// Layout
// ===========================================================================
// ===========================================================================
// Colour + time helpers
// ===========================================================================
uint32_t AudioPlayer::lerp_rgb(uint32_t a, uint32_t b, float t) {
    t = ap_clamp(t, 0.0f, 1.0f);
    int ar = (a >> 16) & 0xff, ag = (a >> 8) & 0xff, ab = a & 0xff;
    int br = (b >> 16) & 0xff, bg = (b >> 8) & 0xff, bb = b & 0xff;
    int r  = ar + (int)((br - ar) * t);
    int g  = ag + (int)((bg - ag) * t);
    int bl = ab + (int)((bb - ab) * t);
    return (r << 16) | (g << 8) | bl;
}

void AudioPlayer::fmt_time(long seconds, char *out) {
    if (seconds < 0) seconds = 0;
    long m = seconds / 60, s = seconds % 60;
    snprintf(out, 16, "%02ld:%02ld", m, s);
}

// ===========================================================================
// Album art — procedural glow tile, rendered once into its own canvas buffer.
// ===========================================================================
void AudioPlayer::draw_album_art() {
    const int s = ALBUM_S, rad = 12;
    for (int ix = 0; ix < s; ++ix) {
        float p   = (float)ix / (float)s;
        float wy1 = s * 0.48f + ap_sin(p * 6.2f + 0.4f) * s * 0.20f;
        float wy2 = s * 0.58f + ap_sin(p * 4.3f + 2.3f) * s * 0.14f;
        for (int iy = 0; iy < s; ++iy) {
            int dx = 0, dy = 0;                       // rounded-corner clip
            if (ix < rad)           dx = rad - ix;
            else if (ix >= s - rad) dx = ix - (s - rad - 1);
            if (iy < rad)           dy = rad - iy;
            else if (iy >= s - rad) dy = iy - (s - rad - 1);
            if (dx && dy && dx * dx + dy * dy > rad * rad) { g_album[iy * s + ix] = COL_BG; continue; }

            float t = (float)(ix + iy) / (2.0f * s);
            float v = (float)iy / (float)s;
            g_album[iy * s + ix] = lerp_rgb(0x081a36, 0x1d4f86, t * 0.65f + v * 0.35f);

            float d1 = (float)iy - wy1; if (d1 < 0) d1 = -d1;
            if (d1 < 6.0f) blend_px(g_album, s, s, s, ix, iy, COL_WAVE_L, (int)(210.0f * (1.0f - d1 / 6.0f)));
            float d2 = (float)iy - wy2; if (d2 < 0) d2 = -d2;
            if (d2 < 5.0f) blend_px(g_album, s, s, s, ix, iy, COL_WAVE_R, (int)(150.0f * (1.0f - d2 / 5.0f)));
        }
    }
}

// ===========================================================================
// Spectrum — the only per-frame pixel work, drawn into the waveform canvas.
// ===========================================================================
void AudioPlayer::draw_spectrum() {
    const int W = wave_w_, H = wave_h_;
    const int cy = H / 2;
    const int half_max = H / 2 - 3;
    for (int i = 0; i < W * H; ++i) g_wave[i] = COL_BG;   // opaque background

    const int step = 6, barw = 4;
    int bars = W / step; if (bars > MAX_BARS) bars = MAX_BARS;
    for (int i = 0; i < bars; ++i) {
        float p = (bars > 1) ? (float)i / (float)(bars - 1) : 0.0f;
        int half = (int)bar_heights_[i];
        if (half < 2)        half = 2;
        if (half > half_max) half = half_max;
        uint32_t col = (p < 0.65f)
            ? lerp_rgb(COL_WAVE_L, COL_WAVE_R, p / 0.65f)
            : lerp_rgb(COL_WAVE_R, COL_WAVE_M, (p - 0.65f) / 0.35f);
        int bx = i * step;
        for (int yy = cy - half; yy < cy + half; ++yy)
            for (int xx = bx; xx < bx + barw; ++xx)
                blend_px(g_wave, W, W, H, xx, yy, col, 255);

        if (half > 16) {                                 // sparkle on tall bars
            float r = hash01(i * 7 + 1);
            if (r > 0.5f) {
                int sy = cy - half - 4 - (int)(r * 9.0f);
                int a  = 110 + (int)(hash01(i * 3) * 110.0f);
                blend_px(g_wave, W, W, H, bx + barw / 2, sy,     COL_ACCENT_HI, a);
                blend_px(g_wave, W, W, H, bx + barw / 2, sy - 1, COL_ACCENT_HI, a / 2);
            }
        }
    }
}


// ===========================================================================
// Decode / stream worker
// ===========================================================================
// Blocking write to the audio ring; yields while full and honours pause/stop.
bool AudioPlayer::write_all(const int16_t *pcm, int samples) {
    const unsigned char *p = (const unsigned char *)pcm;
    unsigned long remaining = (unsigned long)samples * sizeof(int16_t);
    while (remaining > 0) {
        if (g_stop.load()) return false;
        while (g_paused.load() && !g_stop.load()) vos_sleep_ms(1);
        int written = audio_write(p, (unsigned)remaining);
        if (written > 0) { p += written; remaining -= (unsigned)written; }
        else vos_sleep_ms(1);   /* ring full: sleep instead of busy-spinning
                                    * on vos_yield(), which kept a thread always
                                    * runnable and pinned the CPU near 100%. The
                                    * ring holds ~185ms, so a 1-tick nap never
                                    * underruns. */
    }
    return true;
}

// Feed new PCM data into the ring buffer that the UI thread reads for the FFT.
// memmove cannot be used on a volatile array, so both the shift and the fill
// use explicit element-by-element loops. The volatile qualifier on g_vis_samples
// guarantees the writes are visible to the UI thread without a lock.
static void feed_visualization(const int16_t *pcm, int samples, int channels) {
    if (samples <= 0) return;
    int step = channels > 0 ? channels : 2;
    int mono_samples = samples / step;
    if (mono_samples <= 0) return;

    // Shift existing samples toward the front of the ring buffer.
    int keep = 512 - mono_samples;
    if (keep > 0) {
        for (int i = 0; i < keep; ++i)
            g_vis_samples[i] = g_vis_samples[i + mono_samples];
    } else {
        keep = 0;
    }

    // Append the newest mono samples at the end.
    int available = mono_samples < 512 ? mono_samples : 512;
    int src_start = (mono_samples - available) * step;
    for (int i = 0; i < available; ++i)
        g_vis_samples[keep + i] = pcm[src_start + i * step];
}

void AudioPlayer::worker(WorkerArgs *args) {
    AudioPlayer *self = args->self;
    mp3dec_t    *dec  = args->dec;
    int16_t     *pcm  = args->pcm;

    audio_ioctl(AUDIO_IOCTL_SET_VOLUME, (unsigned)g_volume.load());

    for (;;) {                                  // outer loop enables repeat/restart
        if (g_stop.load()) break;

        int fd = open(self->path_, O_RDONLY);
        if (fd < 0) break;

        long file_size = lseek(fd, 0, SEEK_END);
        lseek(fd, 0, SEEK_SET);

        mp3dec_init(dec);
        int sr = 0, ch = 0, br = 0, first = 1;
        long played = 0;
        g_played.store(0);

        unsigned char in[4096];
        ssize_t n;
        bool aborted = false;
        while (!g_stop.load() && (n = read(fd, in, sizeof in)) > 0) {
            // Honour a pending seek by jumping to the requested byte offset and
            // letting the decoder re-sync on the next frame header.
            int seek = g_seek.exchange(-1);
            if (seek >= 0 && file_size > 0) {
                long off = file_size * seek / 1000;
                lseek(fd, off, SEEK_SET);
                mp3dec_init(dec);
                played = (g_total.load() * seek) / 1000;
                g_played.store(played);
                continue;
            }
            if (g_restart.exchange(false)) { aborted = true; break; }

            mp3dec_feed(dec, in, (int)n);
            int s;
            while ((s = mp3dec_decode(dec, pcm, &sr, &ch, &br)) > 0) {
                if (first) {
                    if (sr > 0) { audio_ioctl(AUDIO_IOCTL_SET_RATE, (unsigned)sr); g_rate.store(sr); }
                    g_bitrate.store(br);
                    // estimate the full duration from file size + bitrate
                    if (br > 0)
                        g_total.store((long)((double)file_size * 8.0 / br / 1000.0 * sr));
                    first = 0;
                }
                // peak level for the reactive waveform (per-frame max amplitude)
                int peak = 0;
                for (int k = 0; k < s; k += 17) { int a = pcm[k] < 0 ? -pcm[k] : pcm[k]; if (a > peak) peak = a; }
                g_level.store(peak * 100 / 32768);

                feed_visualization(pcm, s, ch);

                if (!write_all(pcm, s)) { aborted = true; break; }
                played += s / (ch > 0 ? ch : 2);
                g_played.store(played);
            }
            if (aborted) break;
        }
        close(fd);

        if (g_stop.load()) break;
        if (aborted) continue;                 // restart/seek requested → re-open
        if (g_repeat.load()) continue;         // loop the track
        break;                                  // reached the end
    }

    // Leave the engine at the system default rate for other apps.
    audio_ioctl(AUDIO_IOCTL_SET_RATE, 48000);
    g_level.store(0);
    // Buffers are owned by run() — do not free here.
}

// ===========================================================================
// Event handling
// ===========================================================================
// ===========================================================================
// Per-tick spectrum update (the only animated surface)
// ===========================================================================
void AudioPlayer::on_tick() {
    // VexUI ticks ~16 Hz; an 8 Hz spectrum looks just as alive and halves the
    // draw+present cost.
    // if ((++tick_count_ & 1) == 0) return;

    int lvl = g_level.load();
    level_lp_ += ((float)lvl - level_lp_) * 0.25f;

    const int step = 6;
    int bars = wave_w_ / step;
    if (bars > MAX_BARS) bars = MAX_BARS;
    if (bars < 2)        bars = 2;
    const float half_max = (float)(wave_h_ / 2 - 3);

    // Hann-windowed FFT of the most recent 256 mono samples.
    ap_complex x[256];
    for (int i = 0; i < 256; ++i) {
        float w   = 0.5f * (1.0f - ap_cos(2.0f * 3.14159265f * i / 255.0f));
        float val = (float)g_vis_samples[256 + i] / 32768.0f;
        x[i] = ap_complex(val * w, 0.0f);
    }
    ap_fft(x, 256);
    float mags[128];
    for (int i = 0; i < 128; ++i)
        mags[i] = ap_sqrt(x[i].r * x[i].r + x[i].i * x[i].i);

    // Per-band AGC + log frequency axis: every band swings through its own
    // range so highs and lows are visible across the whole width.
    for (int i = 0; i < bars; ++i) {
        float t    = (bars > 1) ? (float)i / (float)(bars - 1) : 0.0f;
        float freq = ap_exp2(t * 7.0f);
        int   idx  = (int)freq; if (idx < 1) idx = 1; if (idx > 126) idx = 126;
        float frac = freq - (float)idx;
        float mag  = mags[idx] * (1.0f - frac) + mags[idx + 1] * frac;

        float pk = bar_peak_[i] * 0.992f;
        if (mag > pk) pk = mag;
        if (pk < 0.0025f) pk = 0.0025f;
        bar_peak_[i] = pk;

        float norm = mag / pk; if (norm > 1.0f) norm = 1.0f;
        float target_h = (0.10f + 0.90f * norm) * half_max;
        if (target_h > bar_heights_[i])
            bar_heights_[i] = bar_heights_[i] * 0.4f  + target_h * 0.6f;
        else
            bar_heights_[i] = bar_heights_[i] * 0.78f + target_h * 0.22f;
        if (bar_heights_[i] < 2.0f) bar_heights_[i] = 2.0f;
    }

    // Draw the spectrum into its own buffer and present just that region — the
    // chrome widgets are untouched (no full repaint).
    draw_spectrum();
    if (wave_canvas_) vui_canvas_flush(win_, wave_canvas_);

    // Refresh the labels/slider/icon that the playback advanced (~1 Hz). These
    // call vui_set_* which marks the window dirty; VexUI then repaints the
    // chrome widgets (and re-blits the waveform) on its next loop pass.
    update_chrome();
}

// Refresh only the widgets whose underlying value changed since last tick.
void AudioPlayer::update_chrome() {
    long played = g_played.load(), total = g_total.load();
    int  rate   = g_rate.load(); if (rate <= 0) rate = 44100;

    long sec = played / rate;
    if (sec != last_sec_) {
        last_sec_ = sec;
        char tl[16], tr[16];
        fmt_time(played / rate, tl);
        fmt_time(total  / rate, tr);
        vui_set_text(time_l_lbl_, tl);
        vui_set_text(time_r_lbl_, tr);
        if (total > 0)
            vui_set_value(progress_, (int)((long long)played * 1000 / total));
    }

    bool pz = g_paused.load();
    if (pz != last_paused_) {
        last_paused_ = pz;
        vui_set_icon_svg(play_ic_, glow_svg(pz));    // play glyph when paused
    }

    int br = g_bitrate.load();
    if (br != last_bitrate_) {
        last_bitrate_ = br;
        char m[96];
        snprintf(m, sizeof m, "MP3  %d.%d kHz  %d kbps", rate / 1000, (rate % 1000) / 100, br);
        vui_set_text(meta_lbl_, m);
    }
}

// ===========================================================================
// Widget callbacks — each control is a real VexUI widget now.
// ===========================================================================
static void cb_tick(vui_window *) { AudioPlayer::instance()->on_tick(); }

static void cb_play(vui_widget *)    { g_paused.store(!g_paused.load()); }
static void cb_prev(vui_widget *)    { g_restart.store(true); g_paused.store(false); }
static void cb_next(vui_widget *)    { g_restart.store(true); g_paused.store(false); }
static void cb_shuffle(vui_widget *w){ bool v = !g_shuffle.load(); g_shuffle.store(v);
                                       vui_set_color(w, v ? COL_ACCENT : COL_ICON); }
static void cb_repeat(vui_widget *w) { bool v = !g_repeat.load();  g_repeat.store(v);
                                       vui_set_color(w, v ? COL_ACCENT : COL_ICON); }
static void cb_seek(vui_widget *w)   { g_seek.store(vui_get_int(w)); }
static void cb_vol(vui_widget *w)    { int v = vui_get_int(w); g_volume.store(v);
                                       audio_ioctl(AUDIO_IOCTL_SET_VOLUME, (unsigned)v); }

static void cb_play_dock(vui_window *) { g_paused.store(!g_paused.load()); }
static void cb_quit(vui_window *w)     { g_stop.store(true); vui_quit(w); }

// ===========================================================================
// Setup + run
// ===========================================================================
void AudioPlayer::run(const char *path) {
    instance_ = this;
    strncpy(path_, path, sizeof(path_) - 1);

    win_ = vui_window_open("Audio Player", DEFAULT_W, DEFAULT_H);
    /* Dock context menu: right-click the dock icon → Quit */
    vui_add_dock_item(win_, "Quit", cb_quit);
    int W = vui_window_width(win_);
    int H = vui_window_height(win_);
    vui_set_clear_color(win_, COL_BG);

    // --- header: album art + track text + corner icons --------------------
    draw_album_art();
    vui_canvas_ex(win_, 28, 24, ALBUM_S, ALBUM_S, g_album, ALBUM_S);

    title_lbl_  = vui_label(win_, 142, 30, "Midnight Drive");
    vui_set_text_scale(title_lbl_, 2); vui_set_color(title_lbl_, COL_TEXT);
    artist_lbl_ = vui_label(win_, 142, 70, "VibeWave");        vui_set_color(artist_lbl_, COL_TEXT_DIM);
    meta_lbl_   = vui_label(win_, 142, 96, "MP3");             vui_set_color(meta_lbl_, COL_TEXT_DIM);

    vui_widget *heart = vui_image(win_, W - 70, 32, 28);
    vui_set_icon_svg(heart, SVG_HEART);  vui_set_color(heart, COL_ICON);
    vui_widget *dots  = vui_image(win_, W - 38, 32, 28);
    vui_set_icon_svg(dots, SVG_DOTS);    vui_set_color(dots, COL_ICON);

    // --- waveform canvas (its own buffer; stride == width) ----------------
    int wx = 44, wy = (int)(H * 0.40f), ww = W - 88, wh = 96;
    if (ww > WAVE_MAX_W) ww = WAVE_MAX_W;
    if (wh > WAVE_MAX_H) wh = WAVE_MAX_H;
    wave_w_ = ww; wave_h_ = wh;
    for (int i = 0; i < ww * wh; ++i) g_wave[i] = COL_BG;
    wave_canvas_ = vui_canvas_ex(win_, wx, wy, ww, wh, g_wave, ww);

    // --- progress slider + time labels ------------------------------------
    int py = H - 150;
    time_l_lbl_ = vui_label(win_, 44, py - 6, "00:00");        vui_set_color(time_l_lbl_, COL_TEXT_DIM);
    time_r_lbl_ = vui_label(win_, W - 92, py - 6, "00:00");    vui_set_color(time_r_lbl_, COL_TEXT_DIM);
    progress_   = vui_slider(win_, 96, py - 8, W - 192, 16, 1000);
    vui_on_click(progress_, cb_seek);

    // --- transport row ----------------------------------------------------
    int cy = H - 92;
    shuffle_ic_ = vui_image(win_, W / 2 - 178, cy - 15, 30);
    vui_set_icon_svg(shuffle_ic_, SVG_SHUFFLE); vui_set_color(shuffle_ic_, COL_ICON); vui_on_click(shuffle_ic_, cb_shuffle);
    vui_widget *prev = vui_image(win_, W / 2 - 98, cy - 15, 30);
    vui_set_icon_svg(prev, SVG_PREV); vui_set_color(prev, COL_TEXT); vui_on_click(prev, cb_prev);

    // Play/pause: the designer glow SVG as a normal W_ICON. VexUI now
    // supersamples icon SVGs, so the thin ring + drop-shadow stay crisp.
    play_ic_ = vui_image(win_, W / 2 - 44, cy - 44, 88);
    vui_set_icon_svg(play_ic_, glow_svg(false)); vui_on_click(play_ic_, cb_play);

    vui_widget *next = vui_image(win_, W / 2 + 68, cy - 15, 30);
    vui_set_icon_svg(next, SVG_NEXT); vui_set_color(next, COL_TEXT); vui_on_click(next, cb_next);
    repeat_ic_ = vui_image(win_, W / 2 + 148, cy - 15, 30);
    vui_set_icon_svg(repeat_ic_, SVG_REPEAT); vui_set_color(repeat_ic_, COL_ICON); vui_on_click(repeat_ic_, cb_repeat);

    // --- volume -----------------------------------------------------------
    vui_widget *spk = vui_image(win_, W - 152, cy - 15, 30);
    vui_set_icon_svg(spk, SVG_SPEAKER); vui_set_color(spk, COL_ICON);
    volume_ = vui_slider(win_, W - 112, cy - 8, 72, 16, 100);
    vui_set_value(volume_, g_volume.load()); vui_on_click(volume_, cb_vol);

    vui_on_tick(win_, cb_tick);
    vui_add_dock_item(win_, "Play / Pause", cb_play_dock);
    vui_add_dock_item(win_, "Quit", cb_quit);

    // Decode buffers must be allocated on the main thread (SYS_SBRK from a
    // spawned thread crashes); hand them to the worker.
    static mp3dec_t dec_buf;
    static int16_t  pcm_buf[MP3DEC_MAX_SAMPLES * 2];
    static WorkerArgs wargs;
    wargs = WorkerArgs{ this, &dec_buf, pcm_buf };
    std::thread([]() { worker(&wargs); }).detach();

    vui_run(win_);
}

int main(int argc, char *argv[]) {
    static AudioPlayer app;
    const char *path = (argc > 1) ? argv[1] : "/music/becorbal-town.mp3";
    app.run(path);
    return 0;
}
