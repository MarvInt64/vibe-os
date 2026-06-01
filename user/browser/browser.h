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
#include "vibeos.h"
#include "weblayout.h"
#include "dom.h"
#include "layout_engine.h"

/* Maximum canvas dimensions (must match the window server's VUI_MAX_W/H). */
static constexpr int BROW_MAX_W = 900;
static constexpr int BROW_MAX_H = 640;

/* Initial window size. */
static constexpr int BROW_INIT_W = 600;
static constexpr int BROW_INIT_H = 400;

/* Chrome dimensions. */
static constexpr int BAR_H       = 30;  /* URL bar height            */
static constexpr int BTN_W       = 30;  /* back/forward button width */
static constexpr int STATUS_H    = 16;  /* status line height        */
static constexpr int CONTENT_TOP = BAR_H + STATUS_H + 4;
static constexpr int MARGIN      = 6;   /* page left/right margin    */
static constexpr int SCROLLBAR_W = 6;   /* scrollbar width           */
static constexpr int PAGE_PAD    = 14;  /* inner left padding        */

class Browser {
public:
    Browser();

    /* Open the window, set up callbacks, and run the event loop. */
    void __attribute__((noreturn)) run();

private:
    /* ---- canvas / window ----------------------------------------------- */
    uint32_t canvas_[BROW_MAX_W * BROW_MAX_H];
    int      win_id_  = -1;
    int      win_w_   = BROW_INIT_W;
    int      win_h_   = BROW_INIT_H;

    /* ---- navigation ---------------------------------------------------- */
    static constexpr int HIST_CAP = 24;
    char history_[HIST_CAP][1024];
    int  hist_count_ = 0;
    int  hist_pos_   = -1;

    char url_[1024]    = {};
    int  url_len_      = 0;
    bool editing_      = true;
    [[maybe_unused]] bool loading_ = false; /* reserved for async-fetch indicator */

    char status_[128]  = {};
    char page_title_[128] = {};  /* extracted <title> for display */

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
                       uint32_t color, int clip_top);
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
    void navigate(const char *url);   /* fetch + layout + render */
    void load_url(const char *url);   /* inner: resolves scheme, follows redirects */
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
    static Browser *s_instance_;  /* for static image callback */

    /* ---- link hit-test ------------------------------------------------- */
    int  link_at(int x, int y) const;
    bool hit_link(int x, int y, char *out, int cap) const;

    /* ---- rendering ----------------------------------------------------- */
    void render();

    /* ---- event handlers ------------------------------------------------ */
    void on_key(uint32_t k);
    void on_mouse_move(int x, int y);
    void on_click(int x, int y);
    void on_scroll(int dy);
    void on_resize(int w, int h);

    /* ---- URL bar editing ----------------------------------------------- */
    void url_putc(char c);
    void url_backspace();
    void url_clear();
};
