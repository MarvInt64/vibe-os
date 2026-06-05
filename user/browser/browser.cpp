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

/* browser.cpp — VibeOS web browser (C++20 rewrite).
 *
 * Architecture
 * ============
 * The Browser class owns all mutable state: window, canvas, navigation
 * history, the layout document and the image cache.  The only global is a
 * static pointer used to route image-sizer callbacks back to the instance.
 *
 * Rendering pipeline
 * ------------------
 *   HTTP fetch  →  dom_parse  →  LayoutEngine::layout  →  render()
 *
 * The browser uses VexUI for its window and chrome (toolbar, status bar),
 * and draws the page content into a W_CANVAS widget.
 *
 * Navigation history
 * ------------------
 * A fixed ring of up to HIST_CAP URLs.  Press '[' or click the ← button to
 * go back, ']' or → to go forward.  A new load from the URL bar truncates
 * the forward history.
 *
 * Keyboard shortcuts
 * ------------------
 *   Enter        load URL (in URL bar)
 *   [ / ]        back / forward
 *   j / k        scroll down/up       Space / b   page down/up
 *   g / G        top / bottom         r           reload current page */

#include "browser.h"
#include "appfont.h"
#include "image.h"
#include "dom.h"
#include "../vexui_font.h"
#include "../../lib/mp3/mp3dec.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <sys/syscall.h>
#include <audio.h>
#include <unistd.h>
#include <fcntl.h>

/* The browser's identity, sent as the User-Agent on every request. */
static const char *BROWSER_USER_AGENT = "VibeOS github.com/MarvInt64/vibe-os";

/* ---- theme colours ----------------------------------------------------- */
static const uint32_t COL_PAGE     = 0x00f7f8fcu;  /* page paper           */
static const uint32_t COL_HOVER    = 0x00d8e4ffu;  /* link hover           */
static const uint32_t COL_DIM      = 0x008396adu;  /* dimmed chrome text   */
static const uint32_t COL_ACCENT   = 0x0064f2ccu;  /* cursor / accent      */
static const uint32_t COL_SCROLLBG = 0x001a2438u;  /* scrollbar track      */

/* Diagnostic logging into the kernel journal (visible via `dmesg` / serial).
 * Used to pinpoint where a fetch fails: resolve, connect, status, empty body. */
static void blog(const char *m) { vos_log(VOS_LOG_APP, m); }
static void blogf(const char *fmt, ...) {
    static char buf[256];
    __builtin_va_list ap; __builtin_va_start(ap, fmt);
    __builtin_vsnprintf(buf, sizeof buf, fmt, ap);
    __builtin_va_end(ap);
    vos_log(VOS_LOG_APP, buf);
}

Browser *Browser::s_instance_ = nullptr;

/* Pre-grow the process heap on the main (UI) thread.
 *
 * Growing the heap via SYS_SBRK from the fetch *worker* thread faults the
 * process (observed: #PF rip=0 right after the SBRK that backs a 512 KB
 * malloc — the worker returns through a zeroed stack slot). Reserving the
 * space here, on the main thread where SBRK is known-good, means the worker's
 * allocations (raw HTTP buffer, HTML body, decoded images) are all satisfied
 * from the free list and never trigger a grow on the worker. Allocate in
 * moderate blocks so each underlying kmalloc stays small, then free them back
 * into the allocator (umalloc never returns pages to the kernel, so the heap
 * stays grown). */
static void warm_heap(unsigned long total_bytes) {
    constexpr unsigned long BLOCK = 512u * 1024u;
    int n = (int)((total_bytes + BLOCK - 1) / BLOCK);
    if (n <= 0) return;
    static void *blocks[64];
    if (n > 64) n = 64;
    int got = 0;
    for (int i = 0; i < n; ++i) { blocks[i] = __builtin_malloc(BLOCK); if (blocks[i]) ++got; }
    for (int i = 0; i < n; ++i) if (blocks[i]) __builtin_free(blocks[i]);
    blogf("browser: warm_heap reserved %d/%d blocks (%lu KB)", got, n, (BLOCK/1024)*got);
}

Browser::Browser() {
    wl_init(&layout_);
    appfont_init();
    layout_engine_.set_metrics(appfont_advance, appfont_line_height);
    layout_engine_.set_image_sizer(img_sizer_cb);
    s_instance_ = this;
    /* Pre-fill canvas with page colour so the window never flashes magentan     * (the transparent sentinel) before the first frame is rendered. */
    for (int i = 0; i < BROW_MAX_W * BROW_MAX_H; ++i) canvas_[i] = COL_PAGE;

    warm_heap(8u * 1024u * 1024u);   /* 8 MB: HTTP buffer + HTML + a few images */
}

/* ---- global callbacks -------------------------------------------------- */

void on_back_click(vui_widget *) { Browser::s_instance_->on_back(); }
void on_forward_click(vui_widget *) { Browser::s_instance_->on_forward(); }
void on_url_enter_cb(vui_widget *) { Browser::s_instance_->on_url_enter(); }
void on_tick_cb(vui_window *) { Browser::s_instance_->on_tick(); }
void on_resize_cb(vui_window *, int w, int h) { Browser::s_instance_->on_resize(w, h); }
void on_key_cb(vui_window *, unsigned key) { Browser::s_instance_->on_key(key); }
void on_scroll_cb(vui_window *, int dy) { Browser::s_instance_->on_scroll(dy); }
void on_mouse_move_cb(vui_window *, int x, int y, vui_u32 /*buttons*/) {
    if (y < BAR_H) return;
    Browser::s_instance_->on_mouse_move(x, y - BAR_H);
}
void on_click_cb(vui_window *, int x, int y, vui_u32 /*buttons*/) {
    if (y < BAR_H) return;
    Browser::s_instance_->on_click(x, y - BAR_H);
}
void on_mouse_release_cb(vui_window *, int x, int y, vui_u32 /*buttons*/) {
    Browser::s_instance_->on_mouse_release(x, y - BAR_H);
}

void Browser::on_back() {
    if (hist_pos_ > 0) {
        --hist_pos_;
        __builtin_strncpy(url_, history_[hist_pos_], sizeof url_);
        url_len_ = (int)__builtin_strlen(url_);
        vui_set_text(w_url_bar_, url_);
        navigate(url_, false);
    }
}

void Browser::on_forward() {
    if (hist_pos_ + 1 < hist_count_) {
        ++hist_pos_;
        __builtin_strncpy(url_, history_[hist_pos_], sizeof url_);
        url_len_ = (int)__builtin_strlen(url_);
        vui_set_text(w_url_bar_, url_);
        navigate(url_, false);
    }
}

void Browser::on_url_enter() {
    const char *text = vui_input_text(w_url_bar_);
    if (text && text[0]) {
        __builtin_strncpy(url_, text, sizeof url_);
        url_len_ = (int)__builtin_strlen(url_);
        navigate(url_);
    }
}

void Browser::on_tick() {
    bool dirty = false;
    if (fetch_state_.load(std::memory_order_acquire) == (int)FetchState::Fetching) {
        ++progress_phase_;
        dirty = true;
    }
    int before = fetch_state_.load(std::memory_order_acquire);
    poll_fetch();
    if (before != fetch_state_.load(std::memory_order_acquire)) dirty = true;
    if (images_dirty_.exchange(false, std::memory_order_acquire)) dirty = true;
    if (dirty) {
        render();
        vui_request_repaint(win_);
    }
}

/* ---- entry point ------------------------------------------------------- */

void __attribute__((noreturn)) Browser::run() {
    win_ = vui_window_open_ex("Browser", win_w_, win_h_, VUI_WINDOW_POSITIONED, 96, 220);

    vui_widget *bar = vui_hbox(win_, 0, 0, win_w_, BAR_H);
    vui_set_anchor(bar, VUI_ANCHOR_LEFT | VUI_ANCHOR_RIGHT | VUI_ANCHOR_TOP);
    vui_set_padding(bar, 4);
    vui_set_gap(bar, 6);

    w_back_ = vui_button(win_, 0, 0, "<");
    vui_box_add(bar, w_back_);
    vui_on_click(w_back_, on_back_click);

    w_forward_ = vui_button(win_, 0, 0, ">");
    vui_box_add(bar, w_forward_);
    vui_on_click(w_forward_, on_forward_click);

    w_url_bar_ = vui_input(win_, 0, 0, 400, "example.com");
    vui_set_expand(w_url_bar_);
    vui_box_add(bar, w_url_bar_);
    vui_on_submit(w_url_bar_, on_url_enter_cb);

    w_canvas_ = vui_canvas_ex(win_, 0, BAR_H, win_w_, win_h_ - BAR_H, canvas_, BROW_MAX_W);
    vui_set_anchor(w_canvas_, VUI_ANCHOR_LEFT | VUI_ANCHOR_RIGHT | VUI_ANCHOR_TOP | VUI_ANCHOR_BOTTOM);

    /* Menu bar — registered with the window server so the top bar shows it. */
    vui_widget *mb     = vui_menubar(win_);
    vui_widget *m_page = vui_menu(win_, mb, "Page");
    vui_on_click(vui_menuitem(win_, m_page, "Reload"),
                 [](vui_widget *) { s_instance_->layout_current(); });
    vui_menu_separator(win_, m_page);
    vui_on_click(vui_menuitem(win_, m_page, "Close"),
                 [](vui_widget *) { exit(0); });
    vui_sync_menubar(win_);

    vui_on_tick(win_, on_tick_cb);
    vui_on_resize(win_, on_resize_cb);
    vui_on_key(win_, on_key_cb);
    vui_on_scroll(win_, on_scroll_cb);
    vui_on_mouse_move(win_, on_mouse_move_cb);
    vui_on_mouse_click(win_, on_click_cb);
    vui_on_mouse_release(win_, on_mouse_release_cb);

    const char *hint = "example.com";
    __builtin_strcpy(url_, hint);
    url_len_ = (int)__builtin_strlen(hint);
    vui_set_text(w_url_bar_, url_);

    render();
    vui_run(win_);
}

/* ---- DNS cache --------------------------------------------------------- */

bool Browser::dns_lookup(const char *host, uint32_t *ip_out) {
    for (int i = 0; i < dns_cache_count_; ++i) {
        if (__builtin_strcmp(dns_cache_[i].host, host) == 0) {
            *ip_out = dns_cache_[i].ip;
            return true;
        }
    }
    return false;
}

void Browser::dns_insert(const char *host, uint32_t ip) {
    if (dns_cache_count_ < DNS_CACHE_CAP) {
        __builtin_strncpy(dns_cache_[dns_cache_count_].host, host, 255);
        dns_cache_[dns_cache_count_].host[255] = '\0';
        dns_cache_[dns_cache_count_].ip = ip;
        ++dns_cache_count_;
    } else {
        for (int i = 0; i < DNS_CACHE_CAP - 1; ++i) {
            __builtin_memcpy(&dns_cache_[i], &dns_cache_[i+1], sizeof(DnsEntry));
        }
        __builtin_strncpy(dns_cache_[DNS_CACHE_CAP - 1].host, host, 255);
        dns_cache_[DNS_CACHE_CAP - 1].host[255] = '\0';
        dns_cache_[DNS_CACHE_CAP - 1].ip = ip;
    }
}

/* ---- proportional drawing helpers for form fields --------------------- */

int Browser::text_width_proportional(const char *s, int px) {
    int w = 0;
    while (*s) {
        unsigned char cp = (unsigned char)*s;
        const af_glyph *g = appfont_get(cp, px);
        w += g ? g->advance : px / 2;
        ++s;
    }
    return w;
}

void Browser::draw_text_proportional(int rx, int sy, const char *s, int max_w, uint32_t color, int px, bool bold) {
    int pen = rx;
    int base = sy + appfont_ascent(px);
    while (*s) {
        unsigned char cp = (unsigned char)*s;
        const af_glyph *g = appfont_get(cp, px);
        int adv = g ? g->advance : px / 2;
        if (pen - rx + adv > max_w) break;
        if (g) {
            blit_glyph_aa(g, pen + g->xoff, base + g->yoff, color, 0);
            if (bold) blit_glyph_aa(g, pen + g->xoff + 1, base + g->yoff, color, 0);
        }
        pen += adv;
        ++s;
    }
}

/* ---- canvas drawing helpers -------------------------------------------- */

void Browser::fill(int x, int y, int w, int h, uint32_t c) {
    if (y < 0) { h += y; y = 0; }
    if (y + h > win_h_) h = win_h_ - y;
    if (h <= 0) return;
    for (int iy = y; iy < y + h; ++iy) {
        uint32_t *row = &canvas_[iy * BROW_MAX_W];
        int x0 = x < 0 ? 0 : x;
        int x1 = (x + w) > win_w_ ? win_w_ : (x + w);
        for (int ix = x0; ix < x1; ++ix) row[ix] = c;
    }
}

void Browser::draw_glyph(int x, int y, char ch, uint32_t c) {
    const uint8_t *g = glyph_for_char(ch);
    for (int r = 0; r < 16; ++r) {
        uint8_t b = g[r];
        int py = y + r;
        if (py < 0 || py >= win_h_) continue;
        uint32_t *row = &canvas_[py * BROW_MAX_W];
        for (int col = 0; col < 8; ++col) {
            if (!((b >> (7 - col)) & 1u)) continue;
            int px = x + col;
            if (px >= 0 && px < win_w_) row[px] = c;
        }
    }
}

void Browser::draw_text(int x, int y, const char *s, uint32_t c) {
    for (; *s; ++s, x += 8) draw_glyph(x, y, *s, c);
}

int Browser::draw_text_n(int x, int y, const char *s, int n, uint32_t c) {
    int i = 0;
    for (; i < n && s[i]; ++i, x += 8) draw_glyph(x, y, s[i], c);
    return i;
}

void Browser::blit_glyph_aa(const af_glyph *g, int ox, int oy, uint32_t color, int italic_shift) {
    if (!g || !g->cov) return;
    int sr = (color >> 16) & 255, sg = (color >> 8) & 255, sb = color & 255;
    for (int yy = 0; yy < g->h; ++yy) {
        /* italic: shift top rows right, bottom rows stay put — forward slant */
        int xs = italic_shift ? (italic_shift * (g->h - 1 - yy) / (g->h > 1 ? g->h - 1 : 1)) : 0;
        int py = oy + yy;
        if (py < 0 || py >= win_h_) continue;
        uint32_t *row = &canvas_[py * BROW_MAX_W];
        const uint8_t *cov = &g->cov[yy * g->w];
        for (int xx = 0; xx < g->w; ++xx) {
            int a = cov[xx];
            if (!a) continue;
            int px = ox + xs + xx;
            if (px < 0 || px >= win_w_) continue;
            uint32_t &d  = row[px];
            int dr = (d >> 16) & 255, dg = (d >> 8) & 255, db = d & 255;
            int r = (sr * a + dr * (255 - a)) / 255;
            int gv= (sg * a + dg * (255 - a)) / 255;
            int bv= (sb * a + db * (255 - a)) / 255;
            d = ((uint32_t)r << 16) | ((uint32_t)gv << 8) | (uint32_t)bv;
        }
    }
}

void Browser::draw_run_text(int rx, int sy, const wl_run *r) {
    int pen = rx;
    int base = sy + appfont_ascent(r->px);
    int italic_shift = r->italic ? (r->px / 4) : 0;
    for (int k = 0; k < r->len; ++k) {
        unsigned char cp = (unsigned char)layout_.pool[r->off + k];
        const af_glyph *g = appfont_get(cp, r->px);
        if (g) {
            blit_glyph_aa(g, pen + g->xoff, base + g->yoff, r->color, italic_shift);
            if (r->bold) blit_glyph_aa(g, pen + g->xoff + 1, base + g->yoff, r->color, italic_shift);
            pen += g->advance;
        } else { pen += r->px / 2; }
    }
}

void Browser::present() { /* handled by VexUI */ }

/* ---- HTTP helpers ------------------------------------------------------ */

bool Browser::parse_ipv4(const char *s, uint32_t *out) {
    uint32_t parts[4]; int pi = 0, have = 0; uint32_t v = 0;
    for (;; ++s) {
        if (*s >= '0' && *s <= '9') { v = v*10u + (uint32_t)(*s-'0'); have=1; if(v>255)return false; }
        else if (*s == '.' || *s == '\0') {
            if (!have || pi > 3) return false;
            parts[pi++] = v; v = 0; have = 0; if (*s == '\0') break;
        } else return false;
    }
    if (pi != 4) return false;
    *out = (parts[0]<<24)|(parts[1]<<16)|(parts[2]<<8)|parts[3];
    return true;
}

int Browser::http_status(const char *raw, int n) {
    int i = 0, code = 0, d = 0;
    while (i < n && raw[i] != ' ' && raw[i] != '\n') ++i;
    while (i < n && raw[i] == ' ') ++i;
    while (i < n && raw[i] >= '0' && raw[i] <= '9') { code = code*10+(raw[i]-'0'); ++i; ++d; }
    return d ? code : 0;
}

bool Browser::find_header(const char *raw, int n, const char *name, char *out, int cap) {
    int nl = (int)__builtin_strlen(name), i = 0;
    while (i < n && raw[i] != '\n') ++i; ++i;
    while (i < n) {
        if (raw[i] == '\r' || raw[i] == '\n') break;
        bool match = true;
        for (int k = 0; k < nl; ++k)
            if ((raw[i+k]|32) != (name[k]|32)) { match=false; break; }
        if (match) {
            int j = i + nl, o = 0;
            while (j < n && (raw[j]==' '||raw[j]=='\t')) ++j;
            while (j < n && raw[j]!='\r' && raw[j]!='\n' && o+1<cap) out[o++]=raw[j++];
            out[o]='\0'; return o > 0;
        }
        while (i < n && raw[i] != '\n') ++i; ++i;
    }
    return false;
}

int Browser::dechunk(char *b, int blen) {
    int ri = 0, wi = 0;
    while (ri < blen) {
        int sz = 0, any = 0;
        for (;;) {
            if (ri >= blen) break;
            char c = b[ri]; int hv = -1;
            if (c>='0'&&c<='9') hv=c-'0'; else if ((c|32)>='a'&&(c|32)<='f') hv=(c|32)-'a'+10;
            if (hv < 0) break; sz=sz*16+hv; ++ri; any=1;
        }
        while (ri < blen && b[ri] != '\n') ++ri; if (ri < blen) ++ri;
        if (!any || sz <= 0) break;
        if (ri + sz > blen) sz = blen - ri;
        __builtin_memmove(b + wi, b + ri, (unsigned)sz);
        wi += sz; ri += sz;
        while (ri < blen && (b[ri]=='\r'||b[ri]=='\n')) ++ri;
    }
    return wi;
}

bool Browser::is_chunked(const char *raw, int n) {
    char te[64];
    if (!find_header(raw, n, "transfer-encoding:", te, sizeof te)) return false;
    for (int i = 0; te[i]; ++i)
        if ((te[i]|32)=='c' && __builtin_strncmp(te+i,"chunked",7)==0) return true;
    return false;
}

int Browser::fetch_raw(const char *url, char *out, int cap, int *bodyoff) {
    static char host[256], path[512];
    const char *u = url; uint32_t ip; int h=0,p=0,i=0,n,secure=0;
    vos_http_req req;
    *bodyoff = 0;
    if (__builtin_strncmp(u,"https://",8)==0){secure=1;u+=8;}
    else if (__builtin_strncmp(u,"http://",7)==0) u+=7;
    while (u[i]&&u[i]!='/'&&h<(int)sizeof(host)-1) host[h++]=u[i++]; host[h]='\0';
    path[0]='/'; p=1;
    if (u[i]=='/'){++i; while(u[i]&&p<(int)sizeof(path)-1) path[p++]=u[i++];} path[p]='\0';
    if (!parse_ipv4(host, &ip)) {
        if (!s_instance_ || !s_instance_->dns_lookup(host, &ip)) {
            if (vos_resolve(host,&ip)<0) return -1;
            if (s_instance_) s_instance_->dns_insert(host, ip);
        }
    }
    req.ip=ip; req.port=secure?443:80; req.host=host; req.path=path; req.out=out; req.cap=cap; req.user_agent=BROWSER_USER_AGENT;
    req.timeout_ms = 4000;
    n = secure ? vos_https_get(&req) : vos_http_get(&req);
    if (n <= 0) return -1;
    for (int k=0; k+1<n; ++k) {
        if (k+3<n && out[k]=='\r'&&out[k+1]=='\n'&&out[k+2]=='\r'&&out[k+3]=='\n'){*bodyoff=k+4;break;}
        if (out[k]=='\n'&&out[k+1]=='\n'){*bodyoff=k+2;break;}
    }
    return n;
}

/* ---- navigation -------------------------------------------------------- */

void Browser::push_history(const char *url) {
    if (hist_pos_ + 1 < hist_count_) hist_count_ = hist_pos_ + 1;
    if (hist_count_ < HIST_CAP) {
        __builtin_strncpy(history_[hist_count_], url, 1023);
        history_[hist_count_][1023] = '\0';
        hist_pos_ = hist_count_++;
    } else {
        for (int i = 0; i < HIST_CAP - 1; ++i) __builtin_memcpy(history_[i], history_[i+1], 1024);
        __builtin_strncpy(history_[HIST_CAP-1], url, 1023);
        history_[HIST_CAP-1][1023] = '\0';
        hist_pos_ = HIST_CAP - 1;
    }
}

void Browser::resolve_href(const char *href, char *out, int cap) const {
    const char *scheme = cur_secure_ ? "https" : "http";
    if (__builtin_strncmp(href,"http://",7)==0 || __builtin_strncmp(href,"https://",8)==0)
        { __builtin_strncpy(out,href,(unsigned)cap); out[cap-1]='\0'; return; }
    if (href[0]=='#'){ out[0]='\0'; return; }
    if (href[0]=='/'&&href[1]=='/'){ __builtin_snprintf(out,cap,"%s:%s",scheme,href); return; }
    if (href[0]=='/') { __builtin_snprintf(out,cap,"%s://%s%s",scheme,cur_host_,href); return; }
    int dir=0,i=0; for(;cur_path_[i];++i) if(cur_path_[i]=='/') dir=i+1;
    __builtin_snprintf(out,cap,"%s://%s%.*s%s",scheme,cur_host_,dir,cur_path_,href);
}

void Browser::clamp_scroll() {
    int max = layout_.height - content_h(); if (max<0)max=0;
    if (scroll_ > max) scroll_ = max;
    if (scroll_ < 0)   scroll_ = 0;
}

void Browser::layout_current() {
    if (!html_buf_ || html_len_ <= 0) return;
    dom_doc dom; dom_init(&dom);
    dom_node *root = dom_parse(&dom, html_buf_, html_len_);
    if (!root) { dom_free(&dom); return; }
    wl_free(&layout_); wl_init(&layout_);
    layout_engine_.layout(&layout_, root, content_w());
    LayoutEngine::extract_title(root, page_title_, (int)sizeof page_title_);
    dom_free(&dom);
    clamp_scroll();
}

void Browser::navigate(const char *url, bool push_to_history) {
    if (!url || !url[0]) return;
    blogf("browser: navigate url=%s push=%d", url, push_to_history);
    /* Stop any audio from the previous page before loading a new one. */
    stop_audio();
    uint32_t gen = ++load_gen_;
    __builtin_strncpy(pending_url_, url, sizeof pending_url_);
    pending_url_[sizeof pending_url_ - 1] = '\0';
    progress_phase_ = 0;
    scroll_ = 0;
    focused_field_ = -1;
    fields_seeded_ = false;
    fetch_state_.store((int)FetchState::Fetching, std::memory_order_release);
    if (worker_.joinable()) worker_.detach();
    worker_ = std::thread([this, gen, push_to_history] { fetch_worker(gen, push_to_history); });
}

void Browser::fetch_worker(uint32_t gen, bool push_to_history) {
    blogf("browser: fetch_worker start url=%s gen=%u", pending_url_, gen);
    text_ready_.store(false, std::memory_order_release);
    load_url(pending_url_, gen, push_to_history);
    if (load_gen_.load(std::memory_order_acquire) != gen) { blog("browser: fetch_worker aborted (after load_url)"); return; }
    blogf("browser: fetch_worker after load_url html_len=%d", html_len_);
    text_ready_.store(true, std::memory_order_release);

    /* Scan the page for an <audio src="..."> tag and start playback.  Only the
     * first audio element is played (one stream at a time).  Done before image
     * loading so audio starts promptly. */
    scan_and_play_audio();

    load_images(gen);
    if (load_gen_.load(std::memory_order_acquire) != gen) { blog("browser: fetch_worker aborted (after load_images)"); return; }
    fetch_state_.store((int)FetchState::Ready, std::memory_order_release);
    blog("browser: fetch_worker done");
}

/* ---- Audio playback ------------------------------------------------------ *
 *
 * <audio src="..."> support.  The audio runs on its own background thread so
 * the UI and network worker stay responsive.  The thread either streams from
 * a local disk file (src resolves to an openable path) or HTTP-fetches the
 * whole file into a heap buffer, then decodes MPEG-1 Layer 3 frame by frame
 * and writes PCM to the kernel mixer via audio_write().
 *
 * Only one stream plays at a time; navigating to a new page calls stop_audio()
 * which signals the worker to exit and joins it. */

/* Walk the parsed DOM for the first <audio src> and hand it to play_audio(). */
void Browser::scan_and_play_audio() {
    if (!html_buf_ || html_len_ <= 0) return;
    dom_doc dom; dom_init(&dom);
    dom_node *root = dom_parse(&dom, html_buf_, html_len_);
    if (!root) { dom_free(&dom); return; }

    char found[512] = {};
    struct Walk {
        static bool go(dom_node *node, char *out, int cap) {
            for (dom_node *c = node->first_child; c; c = c->next_sibling) {
                if (c->type == DOM_ELEMENT && __builtin_strcmp(c->tag, "audio") == 0) {
                    const char *s = dom_attr(c, "src");
                    /* <audio><source src=...></audio> form: check child <source>. */
                    if (!s || !s[0]) {
                        for (dom_node *sc = c->first_child; sc; sc = sc->next_sibling)
                            if (sc->type == DOM_ELEMENT &&
                                __builtin_strcmp(sc->tag, "source") == 0) {
                                s = dom_attr(sc, "src");
                                if (s && s[0]) break;
                            }
                    }
                    if (s && s[0]) { __builtin_strncpy(out, s, cap-1); out[cap-1]='\0'; return true; }
                }
                if (go(c, out, cap)) return true;
            }
            return false;
        }
    };
    bool have = Walk::go(root, found, (int)sizeof found);
    dom_free(&dom);

    if (have) play_audio(found);
}

/* Stop any currently-playing audio: signal the worker and join it. */
void Browser::stop_audio() {
    if (!audio_playing_) return;
    audio_stop_.store(true, std::memory_order_release);
    if (audio_thread_.joinable()) audio_thread_.join();
    audio_playing_ = false;
    audio_src_[0] = '\0';
}

/* Begin playing the audio at 'src' (resolved relative to the current page). */
void Browser::play_audio(const char *src) {
    stop_audio();   /* never run two streams at once */

    char resolved[512];
    /* A leading '/' that maps to a local disk path (e.g. /music/song.mp3) is
     * tried as a local file first by the worker; otherwise resolve to a URL. */
    if (src[0] == '/' && src[1] != '/') {
        __builtin_strncpy(resolved, src, sizeof resolved - 1);
        resolved[sizeof resolved - 1] = '\0';
    } else {
        resolve_href(src, resolved, (int)sizeof resolved);
    }
    __builtin_strncpy(audio_src_, resolved, sizeof audio_src_ - 1);
    audio_src_[sizeof audio_src_ - 1] = '\0';

    audio_stop_.store(false, std::memory_order_release);
    audio_playing_ = true;
    if (audio_thread_.joinable()) audio_thread_.detach();
    /* audio_src_ stays alive for the thread's lifetime (member, and stop_audio
     * joins before it is overwritten). */
    audio_thread_ = std::thread([this] { audio_worker(this, audio_src_, &audio_stop_); });
}

/* Blocking PCM write that respects the stop flag — yields when the ring is
 * full so audio_tick() can drain it.  Returns false if asked to stop. */
static bool browser_audio_write_all(const int16_t *buf, int samples,
                                    std::atomic<bool> *stop) {
    const unsigned char *p = (const unsigned char *)buf;
    unsigned long remaining = (unsigned long)samples * sizeof(int16_t);
    while (remaining > 0) {
        if (stop->load(std::memory_order_acquire)) return false;
        int written = audio_write(p, (unsigned int)remaining);
        if (written > 0) { p += written; remaining -= (unsigned long)written; }
        else __asm__ volatile("int $0x80" : : "a"(3) : "memory"); /* SYS_YIELD */
    }
    return true;
}

void Browser::audio_worker(Browser *b, char src[512], std::atomic<bool> *stop) {
    blogf("browser: audio_worker start src=%s", src);

    /* Decoder + scratch buffers are large; allocate on the heap so the thread
     * stack stays small. */
    mp3dec_t *dec = (mp3dec_t *)__builtin_malloc(sizeof(mp3dec_t));
    int16_t  *pcm = (int16_t *)__builtin_malloc(MP3DEC_MAX_SAMPLES * 2 * sizeof(int16_t));
    if (!dec || !pcm) { if (dec) __builtin_free(dec); if (pcm) __builtin_free(pcm); return; }
    mp3dec_init(dec);

    int sr = 0, ch = 0, br = 0, first = 1;

    /* Path 1: try opening as a local disk file (streams in 4 KB chunks). */
    int fd = open(src, 0 /*O_RDONLY*/);
    if (fd >= 0) {
        unsigned char in[4096];
        ssize_t n;
        while (!stop->load(std::memory_order_acquire) &&
               (n = read(fd, in, sizeof in)) > 0) {
            mp3dec_feed(dec, in, (int)n);
            int s;
            while ((s = mp3dec_decode(dec, pcm, &sr, &ch, &br)) > 0) {
                if (first) {
                    if (sr > 0) audio_ioctl(AUDIO_IOCTL_SET_RATE, (unsigned)sr);
                    first = 0;
                }
                if (!browser_audio_write_all(pcm, s, stop)) break;
            }
        }
        close(fd);
    } else {
        /* Path 2: HTTP fetch the whole file into a heap buffer, then decode.
         * Audio files can be several MB, so use a generous 8 MB buffer. */
        const int CAP = 8 * 1024 * 1024;
        char *raw = (char *)__builtin_malloc(CAP);
        if (raw) {
            int bodyoff = 0;
            int total = fetch_raw(src, raw, CAP, &bodyoff);
            if (total > bodyoff && !stop->load(std::memory_order_acquire)) {
                int pos = bodyoff;
                while (pos < total && !stop->load(std::memory_order_acquire)) {
                    int chunk = total - pos; if (chunk > 4096) chunk = 4096;
                    int used = mp3dec_feed(dec, (const uint8_t *)raw + pos, chunk);
                    pos += (used > 0) ? used : chunk;
                    int s;
                    while ((s = mp3dec_decode(dec, pcm, &sr, &ch, &br)) > 0) {
                        if (first) {
                            if (sr > 0) audio_ioctl(AUDIO_IOCTL_SET_RATE, (unsigned)sr);
                            first = 0;
                        }
                        if (!browser_audio_write_all(pcm, s, stop)) { pos = total; break; }
                    }
                }
            }
            __builtin_free(raw);
        }
    }

    /* Restore the system default sample rate so other apps play at 48 kHz. */
    if (sr > 0 && sr != 48000) audio_ioctl(AUDIO_IOCTL_SET_RATE, 48000);

    __builtin_free(dec);
    __builtin_free(pcm);
    blog("browser: audio_worker done");
}

void Browser::load_images(uint32_t gen) {
    if (!html_buf_ || html_len_ <= 0) return;
    dom_doc dom; dom_init(&dom);
    dom_node *root = dom_parse(&dom, html_buf_, html_len_);
    if (!root) { dom_free(&dom); return; }
    static char srcs[MAX_IMGS][256]; int n = 0;
    struct Walk {
        static void go(dom_node *node, char (*out)[256], int &count, int max) {
            for (dom_node *c = node->first_child; c && count < max; c = c->next_sibling) {
                if (c->type == DOM_ELEMENT && __builtin_strcmp(c->tag, "img") == 0) {
                    const char *s = dom_attr(c, "src");
                    if (!s || !s[0] || __builtin_strncmp(s,"data:",5)==0) s = dom_attr(c,"data-src");
                    if (s && s[0]) {
                        bool dup = false;
                        for (int k=0;k<count;++k) if (__builtin_strcmp(out[k],s)==0){dup=true;break;}
                        if (!dup) { __builtin_strncpy(out[count], s, 255); out[count][255]='\0'; ++count; }
                    }
                }
                go(c, out, count, max);
            }
        }
    };
    Walk::go(root, srcs, n, MAX_IMGS);
    dom_free(&dom);
    for (int i = 0; i < n; ++i) {
        if (load_gen_.load(std::memory_order_acquire) != gen) return;
        if (image_load(srcs[i]) < 0) continue;
        if (load_gen_.load(std::memory_order_acquire) != gen) return;
        { std::lock_guard<std::mutex> lk(layout_mutex_); layout_current(); }
        images_dirty_.store(true, std::memory_order_release);
    }
}

void Browser::poll_fetch() {
    int s = fetch_state_.load(std::memory_order_acquire);
    if (s == (int)FetchState::Ready || s == (int)FetchState::Failed) {
        if (worker_.joinable()) worker_.join();
        fetch_state_.store((int)FetchState::Idle, std::memory_order_release);
    }
}

void Browser::load_url(const char *start_url, uint32_t gen, bool push_to_history) {
    static char cur[1024], host[256], path[512], loc[1024];
    if (!start_url || !start_url[0]) { return; }
    __builtin_strncpy(cur, start_url, sizeof cur); cur[sizeof cur-1]='\0';
    for (int redir = 0; redir < 6; ++redir) {
        if (load_gen_.load(std::memory_order_acquire) != gen) return;
        const char *u = cur; uint32_t ip; int h=0,p=0,i=0,n,bodyoff,secure=0,code;
        char *raw; vos_http_req req;
        if (__builtin_strncmp(u,"https://",8)==0){secure=1;u+=8;}
        else if (__builtin_strncmp(u,"http://",7)==0) u+=7;
        h=0; while(u[i]&&u[i]!='/'&&h<(int)sizeof(host)-1) host[h++]=u[i++]; host[h]='\0';
        p=0; path[p++]='/'; if(u[i]=='/'){ ++i; while(u[i]&&p<(int)sizeof(path)-1) path[p++]=u[i++]; } path[p]='\0';
        blogf("browser: load_url host=%s path=%s secure=%d", host, path, secure);
        if (!parse_ipv4(host,&ip)) {
            if (!dns_lookup(host, &ip)) {
                if (vos_resolve(host,&ip)<0) { blogf("browser: resolve FAILED host=%s", host); return; }
                dns_insert(host, ip);
            }
        }
        if (load_gen_.load(std::memory_order_acquire) != gen) return;
        blogf("browser: resolved ip=%u.%u.%u.%u", (ip>>24)&255,(ip>>16)&255,(ip>>8)&255,ip&255);
        static constexpr int RAW_CAP = 512 * 1024;
        raw = static_cast<char *>(__builtin_malloc(RAW_CAP + 1));
        if (!raw) { blog("browser: malloc RAW_CAP FAILED"); return; }
        req.ip=ip; req.port=secure?443:80; req.host=host; req.path=path;
        req.out=raw; req.cap=RAW_CAP; req.user_agent=BROWSER_USER_AGENT; req.timeout_ms=0;
        n = secure ? vos_https_get(&req) : vos_http_get(&req);
        if (load_gen_.load(std::memory_order_acquire) != gen) { __builtin_free(raw); return; }
        blogf("browser: %s_get returned n=%d", secure?"https":"http", n);
        if (n <= 0) { __builtin_free(raw); return; }
        raw[n] = '\0';
        code = http_status(raw, n);
        blogf("browser: http_status=%d", code);
        if (code >= 300 && code < 400 && find_header(raw,n,"location:",loc,sizeof loc)) {
            if (__builtin_strncmp(loc,"http://",7)==0 || __builtin_strncmp(loc,"https://",8)==0)
                __builtin_strncpy(cur,loc,sizeof cur);
            else if (loc[0]=='/') __builtin_snprintf(cur,sizeof cur,"%s://%s%s",secure?"https":"http",host,loc);
            else __builtin_snprintf(cur,sizeof cur,"%s://%s/%s",secure?"https":"http",host,loc);
            __builtin_strncpy(url_,cur,sizeof url_);
            __builtin_free(raw); continue;
        }
        bodyoff = 0;
        for (int k=0; k+1<n; ++k) {
            if (k+3<n&&raw[k]=='\r'&&raw[k+1]=='\n'&&raw[k+2]=='\r'&&raw[k+3]=='\n'){bodyoff=k+4;break;}
            if (raw[k]=='\n'&&raw[k+1]=='\n'){bodyoff=k+2;break;}
        }
        cur_secure_=secure; __builtin_strncpy(cur_host_,host,sizeof cur_host_); __builtin_strncpy(cur_path_,path,sizeof cur_path_);
        int blen = n - bodyoff; if (is_chunked(raw,n)) blen = dechunk(raw+bodyoff, blen);
        if (html_buf_) { __builtin_free(html_buf_); html_buf_=nullptr; html_len_=0; }
        html_buf_ = static_cast<char *>(__builtin_malloc((unsigned)blen + 1));
        if (!html_buf_) { __builtin_free(raw); return; }
        __builtin_memcpy(html_buf_, raw+bodyoff, (unsigned)blen);
        html_buf_[blen] = '\0'; html_len_ = blen;
        images_clear(); layout_current(); seed_fields();
        __builtin_free(raw); if (push_to_history) push_history(cur);
        return;
    }
}

void Browser::images_clear() {
    for (int i = 0; i < n_imgs_; ++i) {
        if (images_[i].px)  { image_free(images_[i].px);  images_[i].px  = nullptr; }
        if (images_[i].spx) { __builtin_free(images_[i].spx); images_[i].spx = nullptr; }
        images_[i].sw = images_[i].sh = 0;
    }
    n_imgs_ = 0;
}

int Browser::img_sizer_cb(const char *src, int maxw, int *w, int *h) {
    if (!s_instance_) return 0;
    for (int i=0;i<s_instance_->n_imgs_;++i) {
        ImgEntry &e = s_instance_->images_[i];
        if (e.px && __builtin_strcmp(e.src,src)==0) {
            int iw=e.w, ih=e.h;
            if (iw>maxw&&maxw>0){ ih=(int)((long)ih*maxw/iw); iw=maxw; }
            if (ih<1)ih=1; *w=iw; *h=ih; return 1;
        }
    }
    return 0;
}

Browser::ImgEntry *Browser::img_find(const char *src) {
    if (!s_instance_) return nullptr;
    for (int i=0;i<s_instance_->n_imgs_;++i)
        if (__builtin_strcmp(s_instance_->images_[i].src,src)==0) return &s_instance_->images_[i];
    return nullptr;
}

int Browser::image_load(const char *rawsrc) {
    if (n_imgs_ >= MAX_IMGS) return -1;
    static char abs_url[1024], cur[1024], loc[1024], next_url[1024];
    resolve_href(rawsrc, abs_url, sizeof abs_url);
    if (!abs_url[0] || __builtin_strncmp(abs_url,"data:",5)==0) return -1;
    static char raw[512*1024]; __builtin_strncpy(cur, abs_url, sizeof cur);
    for (int redir = 0; redir < 5; ++redir) {
        int bodyoff = 0; int n = fetch_raw(cur, raw, (int)sizeof raw - 1, &bodyoff);
        if (n <= 0) return -1;
        int code = http_status(raw, n);
        if (code>=300&&code<400&&find_header(raw,n,"location:",loc,sizeof loc)){
            if (__builtin_strncmp(loc,"http://",7)==0||__builtin_strncmp(loc,"https://",8)==0) __builtin_strncpy(next_url,loc,sizeof next_url);
            else if (loc[0]=='/') __builtin_snprintf(next_url,sizeof next_url,"http://%.*s%s", (int)__builtin_strlen(cur_host_),cur_host_,loc);
            else __builtin_strncpy(next_url,loc,sizeof next_url);
            __builtin_strncpy(cur,next_url,sizeof cur); continue;
        }
        int blen = n - bodyoff; if (is_chunked(raw,n)) blen = dechunk(raw+bodyoff,blen);
        int w=0,h=0; uint32_t *px = image_decode(reinterpret_cast<const unsigned char *>(raw+bodyoff), blen, &w, &h);
        if (!px || w<=0 || h<=0) return -1;
        { std::lock_guard<std::mutex> lk(layout_mutex_); ImgEntry &e = images_[n_imgs_]; __builtin_strncpy(e.src, rawsrc, sizeof e.src); e.w=w; e.h=h; e.px=px; e.spx=nullptr; e.sw=0; e.sh=0; return n_imgs_++; }
    }
    return -1;
}

int Browser::link_at(int x, int y) const {
    int content_x = PAGE_PAD;
    int docx = x - content_x, docy = y + scroll_;
    for (int i=0; i<layout_.run_count; ++i) {
        const wl_run &r = layout_.runs[i];
        if (r.kind!=WL_TEXT || r.link<0) continue;
        if (docx>=r.x&&docx<r.x+r.w&&docy>=r.y&&docy<r.y+r.h) return r.link;
    }
    return -1;
}

bool Browser::hit_link(int x, int y, char *out, int cap) const {
    int li = link_at(x, y); if (li < 0) return false;
    resolve_href(layout_.hrefs[li], out, cap); return out[0] != '\0';
}

void Browser::seed_fields() {
    int n = layout_.field_count; if (n > MAX_FIELDS) n = MAX_FIELDS;
    for (int i = 0; i < n; ++i) { __builtin_strncpy(field_values_[i], layout_.fields[i].value, 127); field_values_[i][127] = '\0'; }
}

int Browser::field_at(int x, int y) const {
    int content_x = PAGE_PAD;
    int docx = x - content_x, docy = y + scroll_;
    for (int i = 0; i < layout_.run_count; ++i) {
        const wl_run &r = layout_.runs[i];
        if (r.kind != WL_FIELD) continue;
        if (docx>=r.x && docx<r.x+r.w && docy>=r.y && docy<r.y+r.h) return r.off;
    }
    return -1;
}

void Browser::submit_form(int field_idx) {
    if (field_idx < 0 || field_idx >= layout_.field_count) return;
    char query[1024]; int q = 0;
    auto enc = [&](const char *s) {
        const char *hex = "0123456789ABCDEF";
        for (; s && *s && q + 3 < (int)sizeof query; ++s) {
            char c = *s;
            if ((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.') query[q++] = c;
            else if (c == ' ') query[q++] = '+';
            else { query[q++]='%'; query[q++]=hex[(c>>4)&15]; query[q++]=hex[c&15]; }
        }
    };
    bool first = true;
    for (int i = 0; i < layout_.field_count && i < MAX_FIELDS; ++i) {
        const wl_field &f = layout_.fields[i];
        if (!f.name[0]) continue;
        if (f.kind == WLF_SUBMIT || f.kind == WLF_BUTTON) continue;
        if (!first && q + 1 < (int)sizeof query) query[q++] = '&';
        first = false; enc(f.name); if (q + 1 < (int)sizeof query) query[q++] = '='; enc(field_values_[i]);
    }
    query[q] = '\0';
    const wl_field &btn = layout_.fields[field_idx];
    char rel[1300], abs_url[1400];
    const char *act = btn.action[0] ? btn.action : cur_path_;
    __builtin_snprintf(rel, sizeof rel, "%s?%s", act, query);
    resolve_href(rel, abs_url, sizeof abs_url);
    if (abs_url[0]) { __builtin_strncpy(url_, abs_url, sizeof url_); url_[sizeof url_ - 1] = '\0'; url_len_ = (int)__builtin_strlen(url_); focused_field_ = -1; navigate(url_); }
}

void Browser::render() {
    int content_x = PAGE_PAD;
    fill(0, 0, win_w_, win_h_, COL_PAGE);
    bool fetching = fetch_state_.load(std::memory_order_acquire) == (int)FetchState::Fetching;
    bool text_ready = text_ready_.load(std::memory_order_acquire);
    if (fetching) {
        int bar_w = win_w_; int knob = bar_w / 4; int span = bar_w + knob;
        int pos  = (progress_phase_ * 12) % span - knob;
        fill(pos < 0 ? 0 : pos, 0, pos < 0 ? knob + pos : (pos + knob > bar_w ? bar_w - pos : knob), 2, COL_ACCENT);
    }
    if (!text_ready) { draw_text(PAGE_PAD, 20, "Loading...", COL_DIM); return; }
    std::lock_guard<std::mutex> render_lock(layout_mutex_);
    clamp_scroll();
    for (int i = 0; i < layout_.run_count; ++i) {
        const wl_run &r = layout_.runs[i];
        int sy = r.y - scroll_; int rx = content_x + r.x;
        /* Early exit: non-background runs are laid out top-to-bottom,
         * so once r.y is past the viewport bottom nothing below is visible. */
        if (r.kind != WL_RECT && sy >= win_h_) break;
        if (sy + r.h < 0 || sy >= win_h_) continue;
        if (r.kind == WL_RECT) { int top = sy, h = r.h; if (top < 0) { h -= -top; top = 0; } if (h > 0) fill(rx, top, r.w, h, r.color); continue; }
        if (r.kind == WL_IMAGE) {
            ImgEntry *im = img_find(r.off>=0&&r.off<layout_.href_count ? layout_.hrefs[r.off] : "");
            if (im && im->px && r.w > 0 && r.h > 0) {
                /* Build or reuse bilinear-scaled cache */
                if (!im->spx || im->sw != r.w || im->sh != r.h) {
                    if (im->spx) { __builtin_free(im->spx); im->spx = nullptr; }
                    im->spx = (uint32_t *)__builtin_malloc((unsigned)(r.w * r.h) * 4u);
                    if (im->spx) {
                        im->sw = r.w; im->sh = r.h;
                        int iw = im->w, ih = im->h;
                        int fsw = r.w > 1 ? r.w - 1 : 1, fsh = r.h > 1 ? r.h - 1 : 1;
                        for (int yy = 0; yy < r.h; ++yy) {
                            int fy = (int)((long)yy * (ih-1) * 256 / fsh);
                            int y0 = fy>>8, y1 = y0<ih-1?y0+1:y0, ty = fy&255;
                            for (int xx = 0; xx < r.w; ++xx) {
                                int fx = (int)((long)xx * (iw-1) * 256 / fsw);
                                int x0 = fx>>8, x1 = x0<iw-1?x0+1:x0, tx = fx&255;
                                uint32_t c00=im->px[y0*iw+x0],c10=im->px[y0*iw+x1];
                                uint32_t c01=im->px[y1*iw+x0],c11=im->px[y1*iw+x1];
                                int r0=((c00>>16)&255)*(256-tx)+((c10>>16)&255)*tx;
                                int r1=((c01>>16)&255)*(256-tx)+((c11>>16)&255)*tx;
                                int g0=((c00>>8)&255)*(256-tx)+((c10>>8)&255)*tx;
                                int g1=((c01>>8)&255)*(256-tx)+((c11>>8)&255)*tx;
                                int b0=(c00&255)*(256-tx)+(c10&255)*tx;
                                int b1=(c01&255)*(256-tx)+(c11&255)*tx;
                                im->spx[yy*r.w+xx] = (((uint32_t)((r0*(256-ty)+r1*ty)>>16))<<16)
                                                    | (((uint32_t)((g0*(256-ty)+g1*ty)>>16))<<8)
                                                    |  ((uint32_t)((b0*(256-ty)+b1*ty)>>16));
                            }
                        }
                    }
                }
                /* Blit cached scaled image — fast path */
                if (im->spx) {
                    for (int yy = 0; yy < r.h; ++yy) {
                        int py = sy + yy; if (py < 0 || py >= win_h_) continue;
                        const uint32_t *src = &im->spx[yy * r.w];
                        uint32_t *dst = &canvas_[py * BROW_MAX_W + rx];
                        int x0 = rx < 0 ? -rx : 0, x1 = rx+r.w > win_w_ ? win_w_-rx : r.w;
                        if (x1 > x0) __builtin_memcpy(dst+x0, src+x0, (unsigned)(x1-x0)*4u);
                    }
                }
            } else if (sy >= 0) fill(rx, sy, r.w, r.h, 0x00e0e4ecu);
            continue;
        }
        if (r.kind == WL_RULE) { if (sy >= 0) fill(rx, sy, r.w, r.h, r.color); continue; }
        if (r.kind == WL_BULLET) {
            int rad = r.px/7 + 2, cx = rx + rad + 2, cy = sy + r.h/2;
            for (int yy=-rad; yy<=rad; ++yy)
                for (int xx=-rad; xx<=rad; ++xx)
                    if (xx*xx+yy*yy<=rad*rad && cy+yy>=0)
                        if (cx+xx>=0&&cx+xx<win_w_&&cy+yy<win_h_) canvas_[(cy+yy)*BROW_MAX_W+(cx+xx)] = r.color;
            continue;
        }
        if (r.kind == WL_FIELD) {
            if (sy < 0) continue;
            int kind = r.len; bool button = (kind == WLF_SUBMIT || kind == WLF_BUTTON); bool focused = (r.off == focused_field_);
            uint32_t face = button ? (r.bg ? r.bg : 0x002a354du) : (r.bg ? r.bg : 0x00ffffffu);
            uint32_t border = focused ? COL_ACCENT : 0x00808a99u;
            fill(rx, sy, r.w, r.h, face); fill(rx, sy, r.w, 1, border); fill(rx, sy + r.h - 1, r.w, 1, border); fill(rx, sy, 1, r.h, border); fill(rx + r.w - 1, sy, 1, r.h, border);
            const char *text = button ? ((r.off>=0 && r.off<layout_.field_count && layout_.fields[r.off].value[0]) ? layout_.fields[r.off].value : "Submit") : ((r.off>=0 && r.off<MAX_FIELDS) ? field_values_[r.off] : "");
            uint32_t tcol = button ? 0x00ffffffu : (r.color ? r.color : 0x00202632u);
            int ty = sy + (r.h - r.px) / 2;
            int text_w = text_width_proportional(text, r.px);
            int tx = rx + (button ? (r.w - text_w)/2 : 6); if (tx < rx + 4) tx = rx + 4;
            draw_text_proportional(tx, ty, text, r.w - 10, tcol, r.px, button);
            if (focused && !button) {
                int cx = tx + text_width_proportional(text, r.px);
                if (cx > rx + r.w - 4) cx = rx + r.w - 4;
                fill(cx, sy+3, 2, r.h-6, COL_ACCENT);
            }
            continue;
        }
        if (r.link >= 0 && r.link == hover_link_ && sy >= 0) fill(rx-1, sy, r.w+2, r.h, COL_HOVER);
        else if (r.bg && sy >= 0) fill(rx-1, sy, r.w+2, r.h, r.bg);
        draw_run_text(rx, sy, &r);
        if (r.underline && sy + r.h - 2 >= 0) fill(rx, sy + r.h - 2, r.w, 1, r.color);
        if (r.strikethrough) { int mid = sy + r.h * 2 / 5; if (mid >= 0 && mid < win_h_) fill(rx, mid, r.w, 1, r.color); }
    }
    if (layout_.height > content_h()) {
        int track_h = content_h();
        int knob_h  = track_h * content_h() / layout_.height; if (knob_h < 10) knob_h = 10;
        int knob_y  = (track_h - knob_h) * scroll_ / (layout_.height - content_h());
        
        // Track: subtle, dark semi-transparent panel background
        fill(win_w_ - SCROLLBAR_W, 0, SCROLLBAR_W, track_h, 0x000e1622u);
        
        // Knob: rounded pill inset by 2 pixels
        uint32_t knob_col = scroll_dragging_ ? COL_ACCENT : (scroll_hovered_ ? 0x004da3ffu : 0x00334155u);
        fill(win_w_ - SCROLLBAR_W + 2, knob_y + 2, SCROLLBAR_W - 4, knob_h - 4, knob_col);
    }
}

void Browser::on_key(uint32_t k) {
    int old_focused = focused_field_;
    if (focused_field_ >= 0 && focused_field_ < MAX_FIELDS) {
        char *v = field_values_[focused_field_]; int n = (int)__builtin_strlen(v);
        if (k == '\n' || k == '\r') submit_form(focused_field_);
        else if (k == 0x08) { if (n > 0) v[n-1] = '\0'; }
        else if (k == 0x1b) focused_field_ = -1;
        else if (k >= 0x20 && k < 0x7f && n < 126) { v[n]=(char)k; v[n+1]='\0'; }
        if (focused_field_ != old_focused || k != '\n') {
            render();
            vui_request_repaint(win_);
        }
        return;
    }
    int old_scroll = scroll_;
    switch (k) {
    case 'j':            scroll_ += 20; clamp_scroll(); break;
    case 'k':            scroll_ -= 20; clamp_scroll(); break;
    case ' ': case 'f':  scroll_ += content_h() - 20; clamp_scroll(); break;
    case 'b':            scroll_ -= content_h() - 20; clamp_scroll(); break;
    case 'g':            scroll_ = 0; break;
    case 'G':            scroll_ = layout_.height; clamp_scroll(); break;
    case 'r':            if (html_buf_) { layout_current(); } break;
    case '[':            on_back(); break;
    case ']':            on_forward(); break;
    default: break;
    }
    if (scroll_ != old_scroll || k == 'r') {
        render();
        vui_request_repaint(win_);
    }
}

void Browser::on_mouse_move(int x, int y) {
    if (scroll_dragging_) {
        int track_h = content_h();
        int knob_h = track_h * content_h() / layout_.height;
        if (knob_h < 10) knob_h = 10;
        int denom = track_h - knob_h;
        if (denom > 0) {
            int dy = y - scroll_drag_start_y_;
            int delta_scroll = dy * (layout_.height - content_h()) / denom;
            scroll_ = scroll_drag_start_scroll_ + delta_scroll;
            clamp_scroll();
            render();
            vui_request_repaint(win_);
        }
        return;
    }
    
    bool old_hovered = scroll_hovered_;
    scroll_hovered_ = (x >= win_w_ - SCROLLBAR_W);
    if (scroll_hovered_ != old_hovered) {
        render();
        vui_request_repaint(win_);
    }
    
    int hl = link_at(x, y);
    if (hl != hover_link_) {
        hover_link_ = hl;
        render();
        vui_request_repaint(win_);
    }
}

void Browser::on_click(int x, int y) {
    if (x >= win_w_ - SCROLLBAR_W) {
        focused_field_ = -1;
        int track_h = content_h();
        int knob_h = track_h * content_h() / layout_.height;
        if (knob_h < 10) knob_h = 10;
        int knob_y = (track_h - knob_h) * scroll_ / (layout_.height - content_h());
        
        if (y >= knob_y && y < knob_y + knob_h) {
            scroll_dragging_ = true;
            scroll_drag_start_y_ = y;
            scroll_drag_start_scroll_ = scroll_;
        } else {
            scroll_ = (y - knob_h / 2) * (layout_.height - content_h()) / (track_h - knob_h);
            clamp_scroll();
            render();
            vui_request_repaint(win_);
        }
        return;
    }
    
    int old_focused = focused_field_;
    int fi = field_at(x, y);
    if (fi >= 0 && fi < layout_.field_count) {
        int kind = layout_.fields[fi].kind;
        if (kind == WLF_SUBMIT || kind == WLF_BUTTON) { focused_field_ = -1; submit_form(fi); }
        else focused_field_ = fi;
        if (focused_field_ != old_focused) {
            render();
            vui_request_repaint(win_);
        }
        return;
    }
    focused_field_ = -1;
    if (focused_field_ != old_focused) {
        render();
        vui_request_repaint(win_);
    }
    {
        char href[1024];
        if (hit_link(x, y, href, sizeof href)) {
            __builtin_strncpy(url_, href, sizeof url_);
            url_len_ = (int)__builtin_strlen(url_);
            vui_set_text(w_url_bar_, url_);
            navigate(url_);
        }
    }
}

void Browser::on_mouse_release(int x, int y) {
    (void)x; (void)y;
    if (scroll_dragging_) {
        scroll_dragging_ = false;
        render();
        vui_request_repaint(win_);
    }
}

void Browser::on_scroll(int dy) {
    int old_scroll = scroll_;
    scroll_ += dy * 3 * 20;
    clamp_scroll();
    if (scroll_ != old_scroll) {
        render();
        vui_request_repaint(win_);
    }
}

void Browser::on_resize(int w, int h) {
    if (w < 80) w = 80; if (h < 60) h = 60; if (w > BROW_MAX_W) w = BROW_MAX_W; if (h > BROW_MAX_H) h = BROW_MAX_H;
    win_w_ = w; win_h_ = h; if (w_canvas_) vui_set_size(w_canvas_, win_w_, win_h_ - BAR_H); if (html_buf_) layout_current();
}

int main() { static Browser browser; browser.run(); }
