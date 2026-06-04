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
static constexpr int CANVAS_MAX_W = 760;   // backing-buffer stride / max size
static constexpr int CANVAS_MAX_H = 480;
static constexpr int DEFAULT_W    = 720;
static constexpr int DEFAULT_H    = 460;
static constexpr int MAX_BARS     = 160;    // waveform bar slots
static constexpr int BTN_SVG_SZ   = 128;    // play/pause glow SVG render size

static uint32_t g_canvas[CANVAS_MAX_W * CANVAS_MAX_H];

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

static const char* SVG_PLAY = 
    "<svg viewBox=\"0 0 24 24\">"
    "<path d=\"M8 5v14l11-7Z\" fill=\"currentColor\"/>"
    "</svg>";

static const char* SVG_PAUSE = 
    "<svg viewBox=\"0 0 24 24\">"
    "<path d=\"M6 4h4v16H6Z M14 4h4v16h-4Z\" fill=\"currentColor\"/>"
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
static inline float ap_fabs(float x) { return x < 0 ? -x : x; }
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
static constexpr uint32_t COL_BG_TOP   = 0x001a3050;  // background gradient (top)
static constexpr uint32_t COL_BG_BOT   = 0x00102338;  // background gradient (bottom)
static constexpr uint32_t COL_PANEL    = 0x18273f;  // raised glass panels
static constexpr uint32_t COL_PANEL_HI = 0x2b3d5e;  // panel top highlight line
static constexpr uint32_t COL_TRACK    = 0x223047;  // empty slider / progress track
static constexpr uint32_t COL_ACCENT   = 0x4aa8ff;  // azure accent (play / sliders)
static constexpr uint32_t COL_ACCENT_HI= 0x8ccbff;  // bright accent (handles, glow)
static constexpr uint32_t COL_TEXT     = 0xeef3fb;  // primary text
static constexpr uint32_t COL_TEXT_DIM = 0x8b9bb4;  // secondary text / idle icons
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

    // event callbacks (forwarded from the C trampolines below)
    void on_tick();
    void on_click(int x, int y);
    void on_mouse_move(int x, int y);
    void on_mouse_release(int x, int y);
    void on_resize(int w, int h);

private:
    static AudioPlayer *instance_;

    vui_window *win_  = nullptr;
    int   win_w_      = DEFAULT_W;
    int   win_h_      = DEFAULT_H;
    char  path_[256]  = {};

    float anim_phase_ = 0.0f;   // free-running phase for sparkle/glow motion
    float level_lp_   = 0.0f;   // low-pass-filtered audio level (smooth pulsing)
    int   dragging_   = 0;      // 0 none, 1 progress bar, 2 volume slider

    // The waveform is the only part that animates every tick; it gets its own
    // canvas widget pointing INTO the waveform band of g_canvas, so we can
    // present just that rectangle (vui_canvas_flush) without repainting the
    // static chrome. Chrome is redrawn only when chrome_dirty_ is set (state or
    // the displayed second changed).
    int  wave_x_ = 0, wave_y_ = 0, wave_w_ = 0, wave_h_ = 0;  // spectrum band
    bool chrome_dirty_ = false;
    bool last_paused_ = false;
    long last_sec_ = -1;
    int  tick_count_ = 0;
    uint32_t wave_row_[CANVAS_MAX_H] = {};  // precomputed gradient per band row

    float bar_heights_[MAX_BARS] = {};   // current displayed bar heights
    float bar_peak_[MAX_BARS]    = {};   // per-band running peak (AGC normaliser)
    void  draw_svg_icon(int cx, int cy, const char *svg, uint32_t c);

    // ---- computed layout (recomputed each render from win_w_/win_h_) -------
    struct Layout {
        int album_x, album_y, album_s;
        int text_x, title_y, artist_y, meta_y;
        int heart_x, menu_x, icon_y;
        int wave_x0, wave_x1, wave_cy, wave_half;
        int prog_x0, prog_x1, prog_y;
        int time_l_x, time_r_x, time_y;
        int bar_x, bar_y, bar_w, bar_h;          // transport panel
        int ctl_y;                               // transport icon centre line
        int shuffle_x, prev_x, play_x, next_x, repeat_x;
        int play_r;
        int vol_spk_x, vol_x0, vol_x1, vol_y;
    };
    Layout layout() const;

    // ---- drawing primitives ------------------------------------------------
    inline void put(int x, int y, uint32_t c) {
        if ((unsigned)x < (unsigned)win_w_ && (unsigned)y < (unsigned)win_h_)
            g_canvas[y * CANVAS_MAX_W + x] = c;
    }
    inline void blend(int x, int y, uint32_t c, int a) {        // a: 0..255
        if ((unsigned)x >= (unsigned)win_w_ || (unsigned)y >= (unsigned)win_h_) return;
        if (a <= 0) return;
        if (a >= 255) { g_canvas[y * CANVAS_MAX_W + x] = c; return; }
        uint32_t &d = g_canvas[y * CANVAS_MAX_W + x];
        uint32_t r = ((((c >> 16) & 0xff) * a) + (((d >> 16) & 0xff) * (255 - a))) / 255;
        uint32_t g = ((((c >> 8)  & 0xff) * a) + (((d >> 8)  & 0xff) * (255 - a))) / 255;
        uint32_t b = ((((c)       & 0xff) * a) + (((d)       & 0xff) * (255 - a))) / 255;
        d = (r << 16) | (g << 8) | b;
    }
    void fill_rect(int x, int y, int w, int h, uint32_t c);
    void round_rect(int x, int y, int w, int h, int r, uint32_t c, int a = 255);
    void round_rect_outline(int x, int y, int w, int h, int r, uint32_t c, int a);
    void disc(int cx, int cy, float r, uint32_t c, int a = 255);
    void ring(int cx, int cy, float r, float thick, uint32_t c, int a);
    void thick_line(int x0, int y0, int x1, int y1, float t, uint32_t c, int a = 255);
    void tri(int x0, int y0, int x1, int y1, int x2, int y2, uint32_t c);
    void draw_text(int x, int y, const char *s, uint32_t c, int scale = 1) {
        // The kernel TTF rasteriser uses the buffer-width argument as the row
        // stride, so pass the real backing stride (CANVAS_MAX_W), not win_w_.
        if (s && *s) vos_text_draw(g_canvas, CANVAS_MAX_W, win_h_, x, y, s, c, scale);
    }
    int  text_w(const char *s, int scale = 1) { return s && *s ? vos_text_metrics(s, scale) : 0; }

    static uint32_t lerp_rgb(uint32_t a, uint32_t b, float t);

    // ---- composed pieces ---------------------------------------------------
    void render();              // full frame (chrome + waveform) — used on resize
    void render_chrome();       // everything except the waveform band
    void fill_wave_bg(const Layout &L);  // repaint the gradient under the bars
    void draw_background();
    void draw_album_art(const Layout &L);
    void draw_header(const Layout &L);
    void draw_waveform(const Layout &L);
    void draw_progress(const Layout &L);
    void draw_transport(const Layout &L);
    void draw_play_pause(int cx, int cy, int r);
    void draw_icon_shuffle(int cx, int cy, uint32_t c);
    void draw_icon_prev(int cx, int cy, uint32_t c);
    void draw_icon_next(int cx, int cy, uint32_t c);
    void draw_icon_repeat(int cx, int cy, uint32_t c);
    void draw_icon_speaker(int cx, int cy, uint32_t c);
    void draw_icon_heart(int cx, int cy, uint32_t c);
    void draw_icon_dots(int cx, int cy, uint32_t c);
    void fmt_time(long seconds, char *out);

    // ---- decode/stream worker ---------------------------------------------
    // dec and pcm are pre-allocated on the main thread to work around the
    // SYS_SBRK-from-thread bug: malloc inside a spawned thread crashes.
    struct WorkerArgs { AudioPlayer *self; mp3dec_t *dec; int16_t *pcm; };
    static void worker(WorkerArgs *args);
    static bool write_all(const int16_t *pcm, int samples);
};

AudioPlayer *AudioPlayer::instance_ = nullptr;

// ===========================================================================
// Layout
// ===========================================================================
AudioPlayer::Layout AudioPlayer::layout() const {
    const int W = win_w_, H = win_h_;
    Layout L{};

    L.album_s  = 92;
    L.album_x  = 28;
    L.album_y  = 24;
    L.text_x   = L.album_x + L.album_s + 22;
    L.title_y  = 32;
    L.artist_y = 70;
    L.meta_y   = 96;
    L.icon_y   = 40;
    L.menu_x   = W - 34;
    L.heart_x  = W - 66;

    L.wave_x0   = 44;
    L.wave_x1   = W - 44;
    L.wave_cy   = (int)(H * 0.45f);
    L.wave_half = 54;

    L.prog_y   = H - 168;
    L.time_y   = L.prog_y - 8;
    L.time_l_x = 44;
    L.time_r_x = W - 44;
    L.prog_x0  = 96;
    L.prog_x1  = W - 96;

    L.bar_x = 24;
    L.bar_w = W - 48;
    L.bar_h = 116;
    L.bar_y = H - L.bar_h - 18;
    L.ctl_y = L.bar_y + L.bar_h / 2;

    L.play_r    = 27;
    L.play_x    = W / 2;
    L.prev_x    = W / 2 - 82;
    L.next_x    = W / 2 + 82;
    L.shuffle_x = W / 2 - 162;
    L.repeat_x  = W / 2 + 162;

    L.vol_spk_x = W - 116;
    L.vol_x0    = W - 92;
    L.vol_x1    = W - 40;
    L.vol_y     = L.ctl_y;
    return L;
}

// ===========================================================================
// Drawing primitives
// ===========================================================================
uint32_t AudioPlayer::lerp_rgb(uint32_t a, uint32_t b, float t) {
    t = ap_clamp(t, 0.0f, 1.0f);
    int ar = (a >> 16) & 0xff, ag = (a >> 8) & 0xff, ab = a & 0xff;
    int br = (b >> 16) & 0xff, bg = (b >> 8) & 0xff, bb = b & 0xff;
    int r = ar + (int)((br - ar) * t);
    int g = ag + (int)((bg - ag) * t);
    int bl = ab + (int)((bb - ab) * t);
    return (r << 16) | (g << 8) | bl;
}

void AudioPlayer::draw_svg_icon(int cx, int cy, const char *svg, uint32_t c) {
    uint32_t tmp[24 * 24];
    svg_render_rgba(svg, tmp, 24, c & 0xffffff);
    int x0 = cx - 12;
    int y0 = cy - 12;
    for (int y = 0; y < 24; ++y) {
        for (int x = 0; x < 24; ++x) {
            uint32_t px = tmp[y * 24 + x];
            int a = (px >> 24) & 0xff;
            if (a > 0) {
                blend(x0 + x, y0 + y, px & 0xffffff, a);
            }
        }
    }
}

void AudioPlayer::fill_rect(int x, int y, int w, int h, uint32_t c) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > win_w_) w = win_w_ - x;
    if (y + h > win_h_) h = win_h_ - y;
    for (int iy = 0; iy < h; ++iy) {
        uint32_t *row = &g_canvas[(y + iy) * CANVAS_MAX_W + x];
        for (int ix = 0; ix < w; ++ix) row[ix] = c;
    }
}

// Filled rounded rectangle with per-corner anti-aliasing, optional alpha.
void AudioPlayer::round_rect(int x, int y, int w, int h, int r, uint32_t c, int a) {
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    for (int iy = 0; iy < h; ++iy) {
        for (int ix = 0; ix < w; ++ix) {
            // distance into the nearest corner; outside the corner radius → skip
            int dx = 0, dy = 0;
            if (ix < r)            dx = r - ix;
            else if (ix >= w - r)  dx = ix - (w - r - 1);
            if (iy < r)            dy = r - iy;
            else if (iy >= h - r)  dy = iy - (h - r - 1);
            int cov = a;
            if (dx && dy) {
                float dist = ap_fabs((float)r) - 0.0f;
                float d = (float)(dx * dx + dy * dy);
                float edge = (float)(r * r);
                if (d > edge) {
                    float over = (d - edge) / (2.0f * r + 1.0f);
                    if (over > 1.0f) continue;            // fully outside
                    cov = (int)(a * (1.0f - over));
                }
                (void)dist;
            }
            blend(x + ix, y + iy, c, cov);
        }
    }
}

void AudioPlayer::round_rect_outline(int x, int y, int w, int h, int r, uint32_t c, int a) {
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    // top & bottom straight runs
    for (int ix = x + r; ix < x + w - r; ++ix) { blend(ix, y, c, a); blend(ix, y + h - 1, c, a); }
    for (int iy = y + r; iy < y + h - r; ++iy) { blend(x, iy, c, a); blend(x + w - 1, iy, c, a); }
    // four corner quarter-arcs
    for (int t = 0; t <= 90; ++t) {
        float rad = t * 3.14159265f / 180.0f;
        int cxv = (int)(r * ap_cos(rad)), cyv = (int)(r * ap_sin(rad));
        blend(x + w - r - 1 + cxv, y + r - cyv,         c, a);
        blend(x + r - cxv,         y + r - cyv,         c, a);
        blend(x + w - r - 1 + cxv, y + h - r - 1 + cyv, c, a);
        blend(x + r - cxv,         y + h - r - 1 + cyv, c, a);
    }
}

// Anti-aliased filled circle (coverage from the sub-pixel distance to centre).
void AudioPlayer::disc(int cx, int cy, float r, uint32_t c, int a) {
    int ri = (int)(r + 2.0f);
    for (int dy = -ri; dy <= ri; ++dy) {
        for (int dx = -ri; dx <= ri; ++dx) {
            float d = (float)dx * dx + (float)dy * dy;
            float rr = r * r;
            if (d <= rr) {
                blend(cx + dx, cy + dy, c, a);
            } else {
                float over = (d - rr) / (2.0f * r + 1.0f);
                if (over < 1.0f) blend(cx + dx, cy + dy, c, (int)(a * (1.0f - over)));
            }
        }
    }
}

// Anti-aliased ring (stroked circle) of the given thickness.
void AudioPlayer::ring(int cx, int cy, float r, float thick, uint32_t c, int a) {
    int ri = (int)(r + thick + 2.0f);
    float r_out = r + thick * 0.5f, r_in = r - thick * 0.5f;
    for (int dy = -ri; dy <= ri; ++dy) {
        for (int dx = -ri; dx <= ri; ++dx) {
            float dist = 0.0f; { float dd = (float)dx * dx + (float)dy * dy;
                // cheap sqrt via Newton (one step is plenty for AA coverage)
                float g = dd > 0 ? dd : 1.0f; float s = g; s = 0.5f * (s + dd / s); s = 0.5f * (s + dd / s); dist = s; }
            float cov = 1.0f;
            cov = ap_clamp(r_out - dist + 0.5f, 0.0f, 1.0f) *
                  ap_clamp(dist - r_in + 0.5f, 0.0f, 1.0f);
            if (cov > 0.0f) blend(cx + dx, cy + dy, c, (int)(a * cov));
        }
    }
}

void AudioPlayer::thick_line(int x0, int y0, int x1, int y1, float t, uint32_t c, int a) {
    int dx = x1 - x0, dy = y1 - y0;
    int steps = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy)
              ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy);
    if (steps == 0) steps = 1;
    int half = (int)(t * 0.5f);
    for (int i = 0; i <= steps; ++i) {
        int px = x0 + dx * i / steps, py = y0 + dy * i / steps;
        for (int oy = -half; oy <= half; ++oy)
            for (int ox = -half; ox <= half; ++ox)
                blend(px + ox, py + oy, c, a);
    }
}

// Solid triangle (scanline fill); used for the play / skip glyphs.
void AudioPlayer::tri(int x0, int y0, int x1, int y1, int x2, int y2, uint32_t c) {
    int ymin = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2);
    int ymax = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);
    for (int y = ymin; y <= ymax; ++y) {
        int xs[3], n = 0;
        auto edge = [&](int ax, int ay, int bx, int by) {
            if ((ay <= y && by > y) || (by <= y && ay > y))
                xs[n++] = ax + (bx - ax) * (y - ay) / (by - ay);
        };
        edge(x0, y0, x1, y1); edge(x1, y1, x2, y2); edge(x2, y2, x0, y0);
        if (n >= 2) {
            int l = xs[0] < xs[1] ? xs[0] : xs[1];
            int r = xs[0] > xs[1] ? xs[0] : xs[1];
            for (int x = l; x <= r; ++x) put(x, y, c);
        }
    }
}

// ===========================================================================
// Composed UI
// ===========================================================================
void AudioPlayer::draw_background() {
    // Vertical gradient + a soft radial lift toward the upper centre.
    for (int y = 0; y < win_h_; ++y) {
        uint32_t base = lerp_rgb(COL_BG_TOP, COL_BG_BOT, (float)y / win_h_);
        uint32_t *row = &g_canvas[y * CANVAS_MAX_W];
        for (int x = 0; x < win_w_; ++x) row[x] = base;
    }
}

// Procedural "album art": a smooth deep-blue gradient crossed by two soft,
// glowing sine waves. The waves use a per-pixel distance falloff (not 1px
// lines) so they read as soft light, not the grainy hairlines of before, and
// the whole tile is clipped to rounded corners to match the mockup.
void AudioPlayer::draw_album_art(const Layout &L) {
    const int s   = L.album_s;
    const int ax  = L.album_x, ay = L.album_y;
    const int rad = 12;

    for (int ix = 0; ix < s; ++ix) {
        float p   = (float)ix / (float)s;
        float wy1 = s * 0.48f + ap_sin(p * 6.2f + 0.4f) * s * 0.20f;
        float wy2 = s * 0.58f + ap_sin(p * 4.3f + 2.3f) * s * 0.14f;

        for (int iy = 0; iy < s; ++iy) {
            /* rounded-corner clip */
            int dx = 0, dy = 0;
            if (ix < rad)          dx = rad - ix;
            else if (ix >= s - rad) dx = ix - (s - rad - 1);
            if (iy < rad)          dy = rad - iy;
            else if (iy >= s - rad) dy = iy - (s - rad - 1);
            if (dx && dy && dx * dx + dy * dy > rad * rad) continue;

            int px = ax + ix, py = ay + iy;

            /* base: diagonal + vertical blended gradient for depth */
            float t = (float)(ix + iy) / (2.0f * s);
            float v = (float)iy / (float)s;
            g_canvas[py * CANVAS_MAX_W + px] =
                lerp_rgb(0x081a36, 0x1d4f86, t * 0.65f + v * 0.35f);

            /* soft glow from the two flowing wave centrelines */
            float d1 = (float)iy - wy1; if (d1 < 0) d1 = -d1;
            if (d1 < 6.0f) blend(px, py, COL_WAVE_L, (int)(210.0f * (1.0f - d1 / 6.0f)));
            float d2 = (float)iy - wy2; if (d2 < 0) d2 = -d2;
            if (d2 < 5.0f) blend(px, py, COL_WAVE_R, (int)(150.0f * (1.0f - d2 / 5.0f)));
        }
    }
    round_rect_outline(ax, ay, s, s, rad, COL_PANEL_HI, 140); // soft frame
}

void AudioPlayer::draw_header(const Layout &L) {
    draw_album_art(L);
    draw_text(L.text_x, L.title_y, "Midnight Drive", COL_TEXT, 2);
    draw_text(L.text_x, L.artist_y, "VibeWave", COL_TEXT_DIM, 1);

    char meta[96];
    int br = g_bitrate.load(), rate = g_rate.load();
    snprintf(meta, sizeof meta, "MP3  %d.%d kHz  %d kbps",
             rate / 1000, (rate % 1000) / 100, br);
    draw_text(L.text_x, L.meta_y, meta, COL_TEXT_DIM, 1);

    draw_icon_heart(L.heart_x, L.icon_y + 6, COL_TEXT_DIM);
    draw_icon_dots(L.menu_x, L.icon_y + 6, COL_TEXT_DIM);
}

// The hero element: a mirrored bar waveform with a position colour gradient
// (cyan → violet → blue, the magenta peak biased centre-right like the mockup)
// and sparkle dots over the tallest bars.
void AudioPlayer::draw_waveform(const Layout &L) {
    const int x0 = L.wave_x0, x1 = L.wave_x1, cy = L.wave_cy;
    const int width = x1 - x0;
    const int step  = 6;                              // px per bar (bar+gap)
    int bars = width / step; if (bars > MAX_BARS) bars = MAX_BARS;
    const int barw = 4;

    for (int i = 0; i < bars; ++i) {
        float p = (bars > 1) ? (float)i / (float)(bars - 1) : 0.0f;
        int half = (int)bar_heights_[i];
        if (half < 2) half = 2;
        if (half > L.wave_half) half = L.wave_half;

        // Position gradient: cyan → magenta peak (~65% across) → blue, matching
        // the reference's left-cyan / centre-violet / right-blue colouring.
        uint32_t col = (p < 0.65f)
            ? lerp_rgb(COL_WAVE_L, COL_WAVE_R, p / 0.65f)
            : lerp_rgb(COL_WAVE_R, COL_WAVE_M, (p - 0.65f) / 0.35f);

        int bx = x0 + i * step;
        round_rect(bx, cy - half, barw, half * 2, barw / 2, col, 255);

        // sparkle dots above the tallest bars for a touch of life
        if (half > 16) {
            float r = hash01(i * 7 + 1);
            if (r > 0.5f) {
                int sy = cy - half - 4 - (int)(r * 9.0f);
                int a  = 110 + (int)(hash01(i * 3) * 110.0f);
                blend(bx + barw / 2, sy, COL_ACCENT_HI, a);
                blend(bx + barw / 2, sy - 1, COL_ACCENT_HI, a / 2);
            }
        }
    }
}

void AudioPlayer::draw_progress(const Layout &L) {
    long played = g_played.load(), total = g_total.load();
    int rate = g_rate.load(); if (rate <= 0) rate = 44100;
    float prog = total > 0 ? ap_clamp((float)played / (float)total, 0.0f, 1.0f) : 0.0f;

    char tl[16], tr[16];
    fmt_time(played / rate, tl);
    fmt_time(total / rate, tr);
    draw_text(L.time_l_x, L.time_y, tl, COL_TEXT_DIM, 1);
    draw_text(L.time_r_x - text_w(tr), L.time_y, tr, COL_TEXT_DIM, 1);

    // track + filled portion + glowing handle
    int y = L.prog_y, x0 = L.prog_x0, x1 = L.prog_x1;
    round_rect(x0, y - 2, x1 - x0, 4, 2, COL_TRACK, 255);
    int fillw = (int)((x1 - x0) * prog);
    round_rect(x0, y - 2, fillw, 4, 2, COL_ACCENT, 255);
    int hx = x0 + fillw;
    disc(hx, y, 9.0f, COL_ACCENT, 70);     // glow halo
    disc(hx, y, 5.0f, COL_ACCENT_HI, 255); // handle
}

void AudioPlayer::draw_transport(const Layout &L) {
    // rounded glass panel
    round_rect(L.bar_x, L.bar_y, L.bar_w, L.bar_h, 18, COL_PANEL, 235);
    for (int x = L.bar_x + 18; x < L.bar_x + L.bar_w - 18; ++x)
        blend(x, L.bar_y, COL_PANEL_HI, 110);   // top highlight line

    draw_icon_shuffle(L.shuffle_x, L.ctl_y, g_shuffle.load() ? COL_ACCENT : COL_TEXT_DIM);
    draw_icon_prev(L.prev_x, L.ctl_y, COL_TEXT);
    draw_play_pause(L.play_x, L.ctl_y, L.play_r);
    draw_icon_next(L.next_x, L.ctl_y, COL_TEXT);
    draw_icon_repeat(L.repeat_x, L.ctl_y, g_repeat.load() ? COL_ACCENT : COL_TEXT_DIM);

    // volume: speaker glyph + slider
    draw_icon_speaker(L.vol_spk_x, L.ctl_y, COL_TEXT_DIM);
    int vol = g_volume.load();
    round_rect(L.vol_x0, L.vol_y - 2, L.vol_x1 - L.vol_x0, 4, 2, COL_TRACK, 255);
    int vw = (L.vol_x1 - L.vol_x0) * vol / 100;
    round_rect(L.vol_x0, L.vol_y - 2, vw, 4, 2, COL_ACCENT, 255);
    disc(L.vol_x0 + vw, L.vol_y, 4.0f, COL_ACCENT_HI, 255);
}

// Play/pause button: render the designer's glow SVG (radial bg glow, gradient
// ring, feDropShadow halo, gradient glyph) at button size and blit it centred.
// The SVG is assembled from the shared defs + the play or pause glyph.
void AudioPlayer::draw_play_pause(int cx, int cy, int r) {
    // The glow button (radialGradient + feDropShadow over 128x128) is by far
    // the most expensive thing on the frame. It only changes on play<->pause,
    // so render the SVG once per state and blit the cached pixels every frame.
    static uint32_t buf[BTN_SVG_SZ * BTN_SVG_SZ];
    static int cached_state = -1;          // -1 none, 0 playing(pause icon), 1 paused(play icon)
    int state = g_paused.load() ? 1 : 0;
    if (state != cached_state) {
        char svg[1600];
        snprintf(svg, sizeof svg, "<svg viewBox=\"0 0 128 128\">%s%s</svg>",
                 SVG_DEFS, state ? SVG_PLAY_GLYPH : SVG_PAUSE_GLYPH);
        svg_render_rgba(svg, buf, BTN_SVG_SZ, 0xffffff);
        cached_state = state;
    }

    int x0 = cx - BTN_SVG_SZ / 2, y0 = cy - BTN_SVG_SZ / 2;
    for (int iy = 0; iy < BTN_SVG_SZ; ++iy) {
        for (int ix = 0; ix < BTN_SVG_SZ; ++ix) {
            uint32_t px = buf[iy * BTN_SVG_SZ + ix];
            int a = (px >> 24) & 0xff;
            if (a > 0) blend(x0 + ix, y0 + iy, px & 0xffffff, a);
        }
    }
    (void)r;
}

void AudioPlayer::draw_icon_shuffle(int cx, int cy, uint32_t c) {
    draw_svg_icon(cx, cy, SVG_SHUFFLE, c);
}

void AudioPlayer::draw_icon_prev(int cx, int cy, uint32_t c) {
    draw_svg_icon(cx, cy, SVG_PREV, c);
}

void AudioPlayer::draw_icon_next(int cx, int cy, uint32_t c) {
    draw_svg_icon(cx, cy, SVG_NEXT, c);
}

void AudioPlayer::draw_icon_repeat(int cx, int cy, uint32_t c) {
    draw_svg_icon(cx, cy, SVG_REPEAT, c);
}

void AudioPlayer::draw_icon_speaker(int cx, int cy, uint32_t c) {
    draw_svg_icon(cx, cy, SVG_SPEAKER, c);
}

void AudioPlayer::draw_icon_heart(int cx, int cy, uint32_t c) {
    draw_svg_icon(cx, cy, SVG_HEART, c);
}

void AudioPlayer::draw_icon_dots(int cx, int cy, uint32_t c) {
    draw_svg_icon(cx, cy, SVG_DOTS, c);
}

void AudioPlayer::fmt_time(long seconds, char *out) {
    if (seconds < 0) seconds = 0;
    long m = seconds / 60, s = seconds % 60;
    snprintf(out, 16, "%02ld:%02ld", m, s);
}

void AudioPlayer::render() {
    Layout L = layout();
    render_chrome();
    draw_waveform(L);
}

// Everything except the per-tick waveform band. Drawn once at startup and only
// when chrome_dirty_ is set (play/pause, seek, volume, or the second changed) —
// this is where the expensive bits live (full-window gradient, the glow-SVG
// play button), so keeping it off the per-frame path is the main CPU win.
void AudioPlayer::render_chrome() {
    Layout L = layout();
    draw_background();
    draw_header(L);
    draw_progress(L);
    draw_transport(L);
}

// Repaint just the vertical gradient under the waveform band so the previous
// frame's bars are erased before the new ones are drawn.
void AudioPlayer::fill_wave_bg(const Layout &L) {
    (void)L;
    int xmax = wave_x_ + wave_w_; if (xmax > win_w_) xmax = win_w_;
    for (int iy = 0; iy < wave_h_ && wave_y_ + iy < win_h_; ++iy) {
        uint32_t base = wave_row_[iy];          // precomputed, no per-pixel float
        uint32_t *row = &g_canvas[(wave_y_ + iy) * CANVAS_MAX_W];
        for (int x = wave_x_; x < xmax; ++x) row[x] = base;
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
        while (g_paused.load() && !g_stop.load()) vos_sleep_ticks(2);
        int written = audio_write(p, (unsigned)remaining);
        if (written > 0) { p += written; remaining -= (unsigned)written; }
        else vos_sleep_ticks(1);   /* ring full: sleep instead of busy-spinning
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
void AudioPlayer::on_tick() {
    // The VexUI loop ticks at ~16 Hz; an 8 Hz spectrum looks just as alive and
    // halves the per-second draw+present+composite cost. Chrome (time/state)
    // is still checked every other tick, which is plenty for a 1 Hz clock.
    if ((++tick_count_ & 1) == 0) return;

    anim_phase_ += 0.08f;
    // low-pass the live level so the waveform pulse is smooth, not jittery
    int lvl = g_level.load();
    level_lp_ += ((float)lvl - level_lp_) * 0.25f;

    Layout L = layout();
    const int step = 6;
    int bars = (L.wave_x1 - L.wave_x0) / step;
    if (bars > MAX_BARS) bars = MAX_BARS;
    if (bars < 2)        bars = 2;

    // Frequency spectrum.  Hann-window the most recent 256 mono samples, FFT,
    // and map the bins onto the bars with a logarithmic axis (bin = 2^(7t)).
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

    // Per-band AGC: each bar tracks its own slowly-decaying peak and is drawn
    // relative to it.  This is what stops the bass bars (huge raw magnitude)
    // from pinning the left half at full height and looking frozen — every band
    // now swings through its own full range, so highs and lows are visible
    // across the whole spectrum.
    for (int i = 0; i < bars; ++i) {
        float t    = (bars > 1) ? (float)i / (float)(bars - 1) : 0.0f;
        float freq = ap_exp2(t * 7.0f);                 // 1 .. 128
        int   idx  = (int)freq; if (idx < 1) idx = 1; if (idx > 126) idx = 126;
        float frac = freq - (float)idx;
        float mag  = mags[idx] * (1.0f - frac) + mags[idx + 1] * frac;

        // running peak with slow decay (per band), floored so silence ≠ noise
        float pk = bar_peak_[i] * 0.992f;
        if (mag > pk) pk = mag;
        if (pk < 0.0025f) pk = 0.0025f;
        bar_peak_[i] = pk;

        float norm = mag / pk;                          // 0 .. 1, self-scaled
        if (norm > 1.0f) norm = 1.0f;
        float target_h = (0.10f + 0.90f * norm) * (float)L.wave_half;

        // fast rise, slow fall for a lively but smooth meter
        if (target_h > bar_heights_[i])
            bar_heights_[i] = bar_heights_[i] * 0.4f + target_h * 0.6f;
        else
            bar_heights_[i] = bar_heights_[i] * 0.78f + target_h * 0.22f;
        if (bar_heights_[i] < 2.0f) bar_heights_[i] = 2.0f;
    }

    // Chrome (album art, labels, all SVG transport icons, the glow play button,
    // progress + volume) is static between state changes — render it into
    // g_canvas only when the displayed second advances or an interaction
    // changed something. This keeps every SVG render off the per-frame path.
    long played = g_played.load(), total = g_total.load(); (void)total;
    int  rate   = g_rate.load(); if (rate <= 0) rate = 44100;
    long sec    = played / rate;
    if (sec != last_sec_)            { last_sec_ = sec; chrome_dirty_ = true; }
    bool pz = g_paused.load();
    if (pz != last_paused_)          { last_paused_ = pz; chrome_dirty_ = true; }
    if (chrome_dirty_) { chrome_dirty_ = false; render_chrome(); }

    // Per frame: erase the band with the precomputed gradient and draw the bars.
    // The chrome already in g_canvas is left intact. Then present the whole
    // window (correct stride; the compositor blit is cheap).
    fill_wave_bg(L);
    draw_waveform(L);
    if (win_) vui_request_repaint(win_);
}

void AudioPlayer::on_click(int x, int y) {
    Layout L = layout();

    auto hit = [&](int px, int py, int r) {
        int dx = x - px, dy = y - py; return dx * dx + dy * dy <= r * r;
    };

    chrome_dirty_ = true;   // any control click changes something the chrome shows
    if (hit(L.play_x, L.ctl_y, L.play_r + 4)) { g_paused.store(!g_paused.load()); return; }
    if (hit(L.prev_x, L.ctl_y, 16))    { g_restart.store(true); g_paused.store(false); return; }
    if (hit(L.next_x, L.ctl_y, 16))    { g_restart.store(true); g_paused.store(false); return; }
    if (hit(L.shuffle_x, L.ctl_y, 16)) { g_shuffle.store(!g_shuffle.load()); return; }
    if (hit(L.repeat_x, L.ctl_y, 16))  { g_repeat.store(!g_repeat.load()); return; }

    // progress bar — click or start dragging to seek
    if (y >= L.prog_y - 10 && y <= L.prog_y + 10 && x >= L.prog_x0 - 6 && x <= L.prog_x1 + 6) {
        float f = ap_clamp((float)(x - L.prog_x0) / (L.prog_x1 - L.prog_x0), 0.0f, 1.0f);
        g_seek.store((int)(f * 1000));
        dragging_ = 1;
        return;
    }
    // volume slider
    if (y >= L.vol_y - 10 && y <= L.vol_y + 10 && x >= L.vol_x0 - 6 && x <= L.vol_x1 + 6) {
        int v = (int)(ap_clamp((float)(x - L.vol_x0) / (L.vol_x1 - L.vol_x0), 0.0f, 1.0f) * 100);
        g_volume.store(v);
        audio_ioctl(AUDIO_IOCTL_SET_VOLUME, (unsigned)v);
        dragging_ = 2;
        return;
    }
    chrome_dirty_ = false;  // click hit nothing interactive
}

void AudioPlayer::on_mouse_move(int x, int /*y*/) {
    if (!dragging_) return;
    Layout L = layout();
    if (dragging_ == 1) {
        float f = ap_clamp((float)(x - L.prog_x0) / (L.prog_x1 - L.prog_x0), 0.0f, 1.0f);
        g_seek.store((int)(f * 1000));
    } else if (dragging_ == 2) {
        int v = (int)(ap_clamp((float)(x - L.vol_x0) / (L.vol_x1 - L.vol_x0), 0.0f, 1.0f) * 100);
        g_volume.store(v);
        audio_ioctl(AUDIO_IOCTL_SET_VOLUME, (unsigned)v);
    }
    chrome_dirty_ = true;   // reflect the drag in the chrome next tick
}

void AudioPlayer::on_mouse_release(int, int) { dragging_ = 0; }

void AudioPlayer::on_resize(int w, int h) {
    win_w_ = w > CANVAS_MAX_W ? CANVAS_MAX_W : w;
    win_h_ = h > CANVAS_MAX_H ? CANVAS_MAX_H : h;
    render();
}

// ---- C trampolines for the VexUI callbacks --------------------------------
static void cb_tick(vui_window *)            { AudioPlayer::instance()->on_tick(); }
static void cb_click(vui_window *, int x, int y)   { AudioPlayer::instance()->on_click(x, y); }
static void cb_move(vui_window *, int x, int y)    { AudioPlayer::instance()->on_mouse_move(x, y); }
static void cb_release(vui_window *, int x, int y) { AudioPlayer::instance()->on_mouse_release(x, y); }
static void cb_resize(vui_window *, int w, int h)  { AudioPlayer::instance()->on_resize(w, h); }
static void cb_playpause(vui_window *) { g_paused.store(!g_paused.load()); }
static void cb_quit(vui_window *w) { g_stop.store(true); vui_quit(w); }

// ===========================================================================
// Setup + run
// ===========================================================================
void AudioPlayer::run(const char *path) {
    instance_ = this;
    strncpy(path_, path, sizeof(path_) - 1);

    win_ = vui_window_open("Audio Player", DEFAULT_W, DEFAULT_H);
    win_w_ = vui_window_width(win_);
    win_h_ = vui_window_height(win_);
    if (win_w_ > CANVAS_MAX_W) win_w_ = CANVAS_MAX_W;
    if (win_h_ > CANVAS_MAX_H) win_h_ = CANVAS_MAX_H;

    // Single full-window canvas. The whole frame is rendered into g_canvas and
    // presented each tick. Cheap now because the expensive bits are off the
    // per-frame path: the glow play button is cached (rendered only on state
    // change) and the band gradient is precomputed; combined with the 8 Hz tick
    // throttle and the worker no longer busy-spinning, CPU stays modest while
    // the spectrum stays smooth. (Region-present was dropped: g_canvas uses a
    // CANVAS_MAX_W stride that differs from the window's content width, so a
    // partial flush mixed strides and only worked when they happened to match.)
    vui_canvas_ex(win_, 0, 0, win_w_, win_h_, g_canvas, CANVAS_MAX_W);

    // Waveform band geometry + precomputed per-row background gradient, so the
    // only per-frame cost is erasing the band (a cheap precomputed fill) and
    // drawing the bars — the chrome (album art, labels, SVG icons, the glow
    // play button) is rendered only when it changes.
    {
        Layout L = layout();
        wave_y_ = L.wave_cy - L.wave_half - 16;
        wave_h_ = 2 * L.wave_half + 28;
        wave_x_ = L.wave_x0 - 6;
        wave_w_ = (L.wave_x1 - L.wave_x0) + 12;
        if (wave_y_ < 0) wave_y_ = 0;
        if (wave_x_ < 0) wave_x_ = 0;
        for (int iy = 0; iy < wave_h_ && iy < CANVAS_MAX_H; ++iy)
            wave_row_[iy] = lerp_rgb(COL_BG_TOP, COL_BG_BOT,
                                     (float)(wave_y_ + iy) / (float)win_h_);
    }

    vui_on_tick(win_, cb_tick);
    vui_on_mouse_click(win_, cb_click);
    vui_on_mouse_move(win_, cb_move);
    vui_on_mouse_release(win_, cb_release);
    vui_on_resize(win_, cb_resize);

    // dock-icon context menu
    vui_add_dock_item(win_, "Play / Pause", cb_playpause);
    vui_add_dock_item(win_, "Quit", cb_quit);

    // Allocate decode buffers on the main thread — SYS_SBRK from a spawned
    // thread crashes (known VibeOS limitation), so malloc must happen here.
    static mp3dec_t dec_buf;
    static int16_t  pcm_buf[MP3DEC_MAX_SAMPLES * 2];
    static WorkerArgs wargs;
    wargs = WorkerArgs{ this, &dec_buf, pcm_buf };

    std::thread([]() { worker(&wargs); }).detach();

    render();
    vui_request_repaint(win_);
    vui_run(win_);
}

int main(int argc, char *argv[]) {
    static AudioPlayer app;
    const char *path = (argc > 1) ? argv[1] : "/music/becorbal-town.mp3";
    app.run(path);
    return 0;
}
