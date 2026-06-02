#pragma once

/* browser.h — Browser application class.
 *
 * Owns the window, canvas, navigation state, image cache and the layout
 * document.  The event loop (run()) is noreturn.
 *
 * Navigation history: a small ring-buffer of visited URLs lets the user
 * go back/forward with [ and ] (or the on-screen buttons).
 *
 * The class draws its own pixel canvas rather than using VexUI so that it
 * can control every pixel of the page rendering directly. */

#include <cstdint>
#include <atomic>
#include <thread>
#include <mutex>
#include "vibeos.h"
#include "vexui.h"
#include "weblayout.h"
#include "dom.h"
#include "layout_engine.h"

/* Fetch lifecycle, driven by the network worker thread and observed by the UI
 * thread. The atomic is the synchronisation barrier: the worker releases Ready
 * after it finishes touching html_buf_/layout_; the UI acquires it before it
 * reads them for rendering, so there is no data race on the page content. */
enum class FetchState : int { Idle = 0, Fetching = 1, Ready = 2, Failed = 3 };

/* Maximum canvas dimensions (must match the window server's VUI_MAX_W/H). */
static constexpr int BROW_MAX_W = 900;
static constexpr int BROW_MAX_H = 640;

/* Initial window size. */
static constexpr int BROW_INIT_W = 760;
static constexpr int BROW_INIT_H = 560;

/* Chrome dimensions. */
static constexpr int BAR_H       = 36;  /* VexUI bar height          */
static constexpr int STATUS_H    = 18;  /* status line height        */
static constexpr int CONTENT_TOP = BAR_H;
static constexpr int MARGIN      = 0;   /* VexUI handles margins     */
static constexpr int SCROLLBAR_W = 6;   /* scrollbar width           */
static constexpr int PAGE_PAD    = 14;  /* inner left padding        */

class Browser {
public:
    Browser();

    /* Open the window, set up callbacks, and run the event loop. */
    void __attribute__((noreturn)) run();

    /* Event handlers called by VexUI or global callbacks. */
    void on_key(uint32_t k);
    void on_mouse_move(int x, int y);
    void on_click(int x, int y);
    void on_scroll(int dy);
    void on_resize(int w, int h);
    void on_back();
    void on_forward();
    void on_url_enter();
    void on_tick();

    static Browser *s_instance_;  /* for static image callback */

private:
    /* ---- canvas / window ----------------------------------------------- */
    /* Framebuffer uses a fixed stride of BROW_MAX_W so the VexUI canvas can
     * read rows without depending on the current window width. */
    uint32_t    canvas_[BROW_MAX_W * BROW_MAX_H];
    vui_window *win_   = nullptr;
    int         win_w_ = BROW_INIT_W;
    int         win_h_ = BROW_INIT_H;

    /* ---- VexUI widgets ------------------------------------------------- */
    vui_widget *w_url_bar_ = nullptr;
    vui_widget *w_status_  = nullptr;
    vui_widget *w_progress_= nullptr;
    vui_widget *w_canvas_  = nullptr;
    vui_widget *w_back_    = nullptr;
    vui_widget *w_forward_ = nullptr;

    /* ---- navigation ---------------------------------------------------- */
    static constexpr int HIST_CAP = 24;
    char history_[HIST_CAP][1024];
    int  hist_count_ = 0;
    int  hist_pos_   = -1;

    char url_[1024]    = {};
    int  url_len_      = 0;

    char status_[128]  = {};
    char page_title_[128] = {};  /* extracted <title> for display */

    /* ---- threaded fetch ------------------------------------------------- */
    std::atomic<int> fetch_state_{(int)FetchState::Idle};
    int   progress_phase_ = 0;          /* marquee animation position (UI only) */
    char  pending_url_[1024] = {};      /* URL handed to the worker to load     */
    std::thread worker_;                /* the in-flight network worker, if any */

    /* Progressive rendering: the worker re-lays-out the page as each image
     * arrives and raises images_dirty_; the UI thread repaints. layout_mutex_
     * guards layout_ and the image cache against the concurrent re-layout. */
    std::mutex        layout_mutex_;
    std::atomic<bool> images_dirty_{false};
    /* Set once the page text is laid out; the UI then renders content even
     * while the worker keeps fetching images (state stays Fetching for the
     * whole load, which keeps navigation single-worker and race-free). */
    std::atomic<bool> text_ready_{false};

    /* Current page base (for resolving relative URLs). */
    bool cur_secure_   = false;
    char cur_host_[256] = {};
    char cur_path_[512] = {};

    /* ---- layout -------------------------------------------------------- */
    char     *html_buf_ = nullptr;
    int       html_len_ = 0;
    wl_doc    layout_   = {};
    int       scroll_   = 0;
    int       hover_link_ = -1;

    /* ---- form fields --------------------------------------------------- */
    /* Editable values persist across re-layouts (keyed by field index, which
     * is stable for a given page); seeded once from the HTML's value=. */
    static constexpr int MAX_FIELDS = 32;
    char field_values_[MAX_FIELDS][128] = {};
    bool fields_seeded_ = false;
    int  focused_field_ = -1;   /* index into layout_.fields, or -1 */

    LayoutEngine layout_engine_;

    /* ---- image cache --------------------------------------------------- */
    static constexpr int MAX_IMGS = 14;
    struct ImgEntry { char src[256]; uint32_t *px; int w, h; };
    ImgEntry images_[MAX_IMGS] = {};
    int      n_imgs_   = 0;

    /* ---- helpers: canvas drawing --------------------------------------- */
    void fill(int x, int y, int w, int h, uint32_t c);
    void draw_glyph(int x, int y, char ch, uint32_t c);
    void draw_text(int x, int y, const char *s, uint32_t c);
    int  draw_text_n(int x, int y, const char *s, int n, uint32_t c);
    void blit_glyph_aa(const struct af_glyph *g, int ox, int oy,
                       uint32_t color);
    void draw_run_text(int rx, int sy, const wl_run *r);
    void present();

    /* ---- helpers: HTTP ------------------------------------------------- */
    static bool parse_ipv4(const char *s, uint32_t *out);
    static int  http_status(const char *raw, int n);
    static bool find_header(const char *raw, int n, const char *name,
                             char *out, int cap);
    static int  dechunk(char *buf, int len);
    static bool is_chunked(const char *raw, int n);
    static int  fetch_raw(const char *url, char *out, int cap, int *bodyoff);

    /* ---- helpers: navigation ------------------------------------------- */
    void push_history(const char *url);
    void navigate(const char *url);   /* hand off to the worker thread */
    void load_url(const char *url);   /* inner: resolves scheme, follows redirects */
    void fetch_worker();              /* runs on the worker thread: load pending_url_ */
    void load_images();               /* worker thread: fetch images, re-layout each */
    void poll_fetch();                /* UI thread: react to worker completion */
    void layout_current();
    void resolve_href(const char *href, char *out, int cap) const;
    void clamp_scroll();
    int  content_h() const { return win_h_ - CONTENT_TOP; }
    int  page_w()    const { return win_w_ - 2 * MARGIN - SCROLLBAR_W; }
    int  content_w() const { return page_w() - 2 * PAGE_PAD; }

    /* ---- helpers: images ----------------------------------------------- */
    void images_clear();
    void images_collect(dom_node *node);
    static int  img_sizer_cb(const char *src, int maxw, int *w, int *h);
    static ImgEntry *img_find(const char *src);
    int  image_load(const char *src);

    /* ---- link hit-test ------------------------------------------------- */
    int  link_at(int x, int y) const;
    bool hit_link(int x, int y, char *out, int cap) const;

    /* ---- form fields --------------------------------------------------- */
    void seed_fields();              /* copy initial value= into field_values_ */
    int  field_at(int x, int y) const;   /* WL_FIELD run index at point, or -1 */
    void submit_form(int field_idx); /* build query from the field's form + go */

    /* ---- rendering ----------------------------------------------------- */
    void render();

    /* ---- URL bar editing (deprecated, now handled by VexUI) ------------- */
};
