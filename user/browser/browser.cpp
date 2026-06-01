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
 * The canvas is a plain RGBA pixel buffer drawn by render() and handed to
 * the kernel window server via vos_window_present.  VexUI is not used here
 * because the browser needs pixel-level control of the page area.
 *
 * Navigation history
 * ------------------
 * A fixed ring of up to HIST_CAP URLs.  Press '[' or click the ← button to
 * go back, ']' or → to go forward.  A new load from the URL bar truncates
 * the forward history.
 *
 * Keyboard shortcuts
 * ------------------
 *   Enter        load URL             u / /       focus URL bar
 *   [ / ]        back / forward       Esc         cancel URL editing
 *   j / k        scroll down/up       Space / b   page down/up
 *   g / G        top / bottom         r           reload current page */

#include "browser.h"
#include "appfont.h"
#include "appimage.h"
#include "dom.h"
#include "../vexui_font.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>

/* ---- theme colours ----------------------------------------------------- */
static const uint32_t COL_BG       = 0x00101620u;  /* window background    */
static const uint32_t COL_BAR      = 0x001d2740u;  /* chrome bar           */
static const uint32_t COL_BAR_HI   = 0x00273f6bu;  /* URL bar active       */
static const uint32_t COL_BTN      = 0x00243350u;  /* nav button bg        */
/* COL_BTN_HI reserved for future hover state on back/forward buttons. */
/* static const uint32_t COL_BTN_HI = 0x00304472u; */
static const uint32_t COL_PAGE     = 0x00f7f8fcu;  /* page paper           */
static const uint32_t COL_HOVER    = 0x00d8e4ffu;  /* link hover           */
static const uint32_t COL_TEXT_CHR = 0x00d6e2f2u;  /* chrome text          */
static const uint32_t COL_DIM      = 0x008396adu;  /* dimmed chrome text   */
static const uint32_t COL_ACCENT   = 0x0064f2ccu;  /* cursor / accent      */
static const uint32_t COL_SCROLLBG = 0x001a2438u;  /* scrollbar track      */

Browser *Browser::s_instance_ = nullptr;

Browser::Browser() {
    wl_init(&layout_);
    appfont_init();
    layout_engine_.set_metrics(appfont_advance, appfont_line_height);
    layout_engine_.set_image_sizer(img_sizer_cb);
    s_instance_ = this;
}

/* ---- entry point ------------------------------------------------------- */

void __attribute__((noreturn)) Browser::run() {
    win_id_ = vos_window_create("Browser", win_w_, win_h_);
    if (win_id_ < 0) {
        vos_log(VOS_LOG_APP, "browser: no window server — start the desktop with 'gui'");
        exit(1);
    }

    /* Pre-fill URL bar with a hint. */
    const char *hint = "example.com";
    __builtin_strcpy(url_, hint);
    url_len_ = (int)__builtin_strlen(hint);

    __builtin_strcpy(status_,
        "type a URL + Enter  |  [/] back/forward  |  j/k scroll  |  u=edit");

    render();

    for (;;) {
        vos_event ev;
        bool dirty = false;

        while (vos_event_poll(win_id_, &ev) == 1) {
            switch (ev.type) {
            case VOS_EV_CLOSE:      exit(0);
            case VOS_EV_KEY:        on_key(ev.key);              dirty = true; break;
            case VOS_EV_SCROLL:     on_scroll(ev.y);             dirty = true; break;
            case VOS_EV_MOUSE_MOVE: on_mouse_move(ev.x, ev.y);  dirty = true; break;
            case VOS_EV_MOUSE_DOWN: on_click(ev.x, ev.y);       dirty = true; break;
            case VOS_EV_RESIZE:     on_resize(ev.x, ev.y);      dirty = true; break;
            default: break;
            }
        }

        if (dirty) render();
        vos_yield();
    }
}

/* ---- canvas drawing helpers -------------------------------------------- */

void Browser::fill(int x, int y, int w, int h, uint32_t c) {
    for (int iy = y; iy < y + h; ++iy) {
        if (iy < 0 || iy >= win_h_) continue;
        for (int ix = x; ix < x + w; ++ix) {
            if (ix >= 0 && ix < win_w_) canvas_[iy * win_w_ + ix] = c;
        }
    }
}

void Browser::draw_glyph(int x, int y, char ch, uint32_t c) {
    const uint8_t *g = glyph_for_char(ch);
    for (int r = 0; r < 16; ++r) {
        uint8_t b = g[r];
        for (int col = 0; col < 8; ++col) {
            if (!((b >> (7 - col)) & 1u)) continue;
            int px = x + col, py = y + r;
            if (px >= 0 && px < win_w_ && py >= 0 && py < win_h_)
                canvas_[py * win_w_ + px] = c;
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

/* Anti-aliased glyph blit from the appfont cache. */
void Browser::blit_glyph_aa(const af_glyph *g, int ox, int oy,
                              uint32_t color, int clip_top) {
    if (!g || !g->cov) return;
    int sr = (color >> 16) & 255, sg = (color >> 8) & 255, sb = color & 255;
    for (int yy = 0; yy < g->h; ++yy) {
        int py = oy + yy;
        if (py < clip_top || py >= win_h_) continue;
        for (int xx = 0; xx < g->w; ++xx) {
            int a = g->cov[yy * g->w + xx];
            if (!a) continue;
            int px = ox + xx;
            if (px < 0 || px >= win_w_) continue;
            uint32_t &d  = canvas_[py * win_w_ + px];
            int dr = (d >> 16) & 255, dg = (d >> 8) & 255, db = d & 255;
            int r = (sr * a + dr * (255 - a)) / 255;
            int gv= (sg * a + dg * (255 - a)) / 255;
            int bv= (sb * a + db * (255 - a)) / 255;
            d = ((uint32_t)r << 16) | ((uint32_t)gv << 8) | (uint32_t)bv;
        }
    }
}

/* Draw one wl_run's text using the proportional font cache. */
void Browser::draw_run_text(int rx, int sy, const wl_run *r) {
    int pen = rx;
    int base = sy + appfont_ascent(r->px);
    for (int k = 0; k < r->len; ++k) {
        unsigned char cp = (unsigned char)layout_.pool[r->off + k];
        const af_glyph *g = appfont_get(cp, r->px);
        if (g) {
            blit_glyph_aa(g, pen + g->xoff, base + g->yoff, r->color, CONTENT_TOP);
            if (r->bold) /* faux bold: draw shifted by 1 px */
                blit_glyph_aa(g, pen + g->xoff + 1, base + g->yoff, r->color, CONTENT_TOP);
            pen += g->advance;
        } else {
            pen += r->px / 2;
        }
    }
}

void Browser::present() {
    vos_window_present(win_id_, canvas_, win_w_, win_h_);
}

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

bool Browser::find_header(const char *raw, int n, const char *name,
                            char *out, int cap) {
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

/* One-shot fetch with redirect following; returns body length, sets *bodyoff. */
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
    if (!parse_ipv4(host, &ip)) { if (vos_resolve(host,&ip)<0) return -1; }
    req.ip=ip; req.port=secure?443:80; req.host=host; req.path=path; req.out=out; req.cap=cap;
    n = secure ? vos_https_get(&req) : vos_http_get(&req);
    if (n <= 0) return -1;
    /* Locate the header/body separator. */
    for (int k=0; k+1<n; ++k) {
        if (k+3<n && out[k]=='\r'&&out[k+1]=='\n'&&out[k+2]=='\r'&&out[k+3]=='\n'){*bodyoff=k+4;break;}
        if (out[k]=='\n'&&out[k+1]=='\n'){*bodyoff=k+2;break;}
    }
    return n;
}

/* ---- navigation -------------------------------------------------------- */

void Browser::push_history(const char *url) {
    /* Truncate any forward history. */
    if (hist_pos_ + 1 < hist_count_) hist_count_ = hist_pos_ + 1;
    if (hist_count_ < HIST_CAP) {
        __builtin_strncpy(history_[hist_count_], url, 1023);
        history_[hist_count_][1023] = '\0';
        hist_pos_ = hist_count_++;
    } else {
        /* Rotate ring: drop oldest. */
        for (int i = 0; i < HIST_CAP - 1; ++i)
            __builtin_memcpy(history_[i], history_[i+1], 1024);
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

    /* Extract <title> for the window title bar. */
    LayoutEngine::extract_title(root, page_title_, (int)sizeof page_title_);
    if (page_title_[0]) {
        char title_buf[160];
        __builtin_snprintf(title_buf, sizeof title_buf, "Browser — %s", page_title_);
        /* Update window title (not currently exposed via vibeos.h; use status). */
    }

    dom_free(&dom);
    clamp_scroll();
}

void Browser::navigate(const char *url) {
    load_url(url);
}

void Browser::load_url(const char *start_url) {
    static char cur[1024];
    static char host[256], path[512], loc[1024];

    if (!start_url || !start_url[0]) { __builtin_strcpy(status_,"enter a URL"); return; }
    __builtin_strncpy(cur, start_url, sizeof cur); cur[sizeof cur-1]='\0';

    for (int redir = 0; redir < 6; ++redir) {
        const char *u = cur;
        uint32_t ip; int h=0,p=0,i=0,n,bodyoff,secure=0,code;
        char *raw;
        vos_http_req req;

        if (__builtin_strncmp(u,"https://",8)==0){secure=1;u+=8;}
        else if (__builtin_strncmp(u,"http://",7)==0) u+=7;

        h=0; while(u[i]&&u[i]!='/'&&h<(int)sizeof(host)-1) host[h++]=u[i++]; host[h]='\0';
        p=0; path[p++]='/';
        if(u[i]=='/'){ ++i; while(u[i]&&p<(int)sizeof(path)-1) path[p++]=u[i++]; } path[p]='\0';

        if (!parse_ipv4(host,&ip)) {
            __builtin_snprintf(status_,sizeof status_,"Resolving %s …",host); present();
            if (vos_resolve(host,&ip)<0){__builtin_strcpy(status_,"DNS: cannot resolve host");return;}
        }

        static constexpr int RAW_CAP = 512 * 1024;
        raw = static_cast<char *>(__builtin_malloc(RAW_CAP + 1));
        if (!raw) { __builtin_strcpy(status_,"out of memory"); return; }

        req.ip=ip; req.port=secure?443:80; req.host=host; req.path=path;
        req.out=raw; req.cap=RAW_CAP;
        __builtin_snprintf(status_,sizeof status_,"%s %s …",secure?"TLS":"HTTP",host);
        present();
        vos_log(VOS_LOG_APP, cur);
        n = secure ? vos_https_get(&req) : vos_http_get(&req);
        if (n <= 0) {
            __builtin_free(raw);
            __builtin_strcpy(status_, secure ? "TLS request failed" : "request failed");
            return;
        }
        raw[n] = '\0';

        code = http_status(raw, n);

        /* Follow HTTP redirects. */
        if (code >= 300 && code < 400 && find_header(raw,n,"location:",loc,sizeof loc)) {
            if (__builtin_strncmp(loc,"http://",7)==0 || __builtin_strncmp(loc,"https://",8)==0)
                __builtin_strncpy(cur,loc,sizeof cur);
            else if (loc[0]=='/')
                __builtin_snprintf(cur,sizeof cur,"%s://%s%s",secure?"https":"http",host,loc);
            else
                __builtin_snprintf(cur,sizeof cur,"%s://%s/%s",secure?"https":"http",host,loc);
            __builtin_strncpy(url_,cur,sizeof url_); url_len_=(int)__builtin_strlen(url_);
            __builtin_free(raw); continue;
        }

        /* Locate body. */
        bodyoff = 0;
        for (int k=0; k+1<n; ++k) {
            if (k+3<n&&raw[k]=='\r'&&raw[k+1]=='\n'&&raw[k+2]=='\r'&&raw[k+3]=='\n'){bodyoff=k+4;break;}
            if (raw[k]=='\n'&&raw[k+1]=='\n'){bodyoff=k+2;break;}
        }

        /* Save page base for relative-URL resolution. */
        cur_secure_=secure;
        __builtin_strncpy(cur_host_,host,sizeof cur_host_);
        __builtin_strncpy(cur_path_,path,sizeof cur_path_);

        /* Decode chunked transfer encoding if needed. */
        int blen = n - bodyoff;
        if (is_chunked(raw,n)) blen = dechunk(raw+bodyoff, blen);

        /* Store HTML. */
        if (html_buf_) { __builtin_free(html_buf_); html_buf_=nullptr; html_len_=0; }
        html_buf_ = static_cast<char *>(__builtin_malloc((unsigned)blen + 1));
        if (!html_buf_) { __builtin_free(raw); __builtin_strcpy(status_,"out of memory"); return; }
        __builtin_memcpy(html_buf_, raw+bodyoff, (unsigned)blen);
        html_buf_[blen] = '\0'; html_len_ = blen;

        /* Load images before layout so the sizer has data. */
        {
            dom_doc dom2; dom_init(&dom2);
            dom_node *root2 = dom_parse(&dom2, html_buf_, html_len_);
            if (root2) { images_clear(); images_collect(root2); }
            dom_free(&dom2);
        }

        layout_current();
        __builtin_free(raw);
        scroll_ = 0;
        push_history(cur);
        __builtin_snprintf(status_,sizeof status_,
            "%d  %d runs  %luK  %s",
            code, layout_.run_count, (unsigned long)0,
            page_title_[0] ? page_title_ : "");
        vos_log(VOS_LOG_APP, status_);
        editing_ = false;
        return;
    }
    __builtin_strcpy(status_,"too many redirects");
}

/* ---- image cache ------------------------------------------------------- */

void Browser::images_clear() {
    for (int i = 0; i < n_imgs_; ++i)
        if (images_[i].px) { appimage_free(images_[i].px); images_[i].px=nullptr; }
    n_imgs_ = 0;
}

void Browser::images_collect(dom_node *node) {
    if (!node || n_imgs_ >= MAX_IMGS) return;
    for (dom_node *c=node->first_child; c && n_imgs_<MAX_IMGS; c=c->next_sibling) {
        if (c->type==DOM_ELEMENT && __builtin_strcmp(c->tag,"img")==0) {
            const char *src = dom_attr(c,"src");
            if (!src || !src[0] || __builtin_strncmp(src,"data:",5)==0)
                src = dom_attr(c,"data-src");
            if (src && src[0]) {
                bool dup = false;
                for (int k=0;k<n_imgs_;++k)
                    if (__builtin_strcmp(images_[k].src,src)==0){dup=true;break;}
                if (!dup) {
                    __builtin_snprintf(status_,sizeof status_,"Loading image %d …",n_imgs_+1);
                    present();
                    image_load(src);
                }
            }
        }
        images_collect(c);
    }
}

/* Static callback: routes back to the current Browser instance. */
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
        if (__builtin_strcmp(s_instance_->images_[i].src,src)==0)
            return &s_instance_->images_[i];
    return nullptr;
}

int Browser::image_load(const char *rawsrc) {
    if (n_imgs_ >= MAX_IMGS) return -1;
    static char abs_url[1024], cur[1024], loc[1024], next_url[1024];
    resolve_href(rawsrc, abs_url, sizeof abs_url);
    if (!abs_url[0] || __builtin_strncmp(abs_url,"data:",5)==0) return -1;

    static char raw[512*1024];
    __builtin_strncpy(cur, abs_url, sizeof cur);

    for (int redir = 0; redir < 5; ++redir) {
        int bodyoff = 0;
        int n = fetch_raw(cur, raw, (int)sizeof raw - 1, &bodyoff);
        if (n <= 0) return -1;
        int code = http_status(raw, n);
        if (code>=300&&code<400&&find_header(raw,n,"location:",loc,sizeof loc)){
            /* Resolve redirect location against the current base. */
            if (__builtin_strncmp(loc,"http://",7)==0||__builtin_strncmp(loc,"https://",8)==0)
                __builtin_strncpy(next_url,loc,sizeof next_url);
            else if (loc[0]=='/')
                __builtin_snprintf(next_url,sizeof next_url,"http://%.*s%s",
                    (int)__builtin_strlen(cur_host_),cur_host_,loc);
            else __builtin_strncpy(next_url,loc,sizeof next_url);
            __builtin_strncpy(cur,next_url,sizeof cur); continue;
        }
        int blen = n - bodyoff;
        if (is_chunked(raw,n)) blen = dechunk(raw+bodyoff,blen);
        int w=0,h=0;
        uint32_t *px = appimage_decode(
            reinterpret_cast<const unsigned char *>(raw+bodyoff), blen, &w, &h);
        if (!px || w<=0 || h<=0) return -1;
        ImgEntry &e = images_[n_imgs_];
        __builtin_strncpy(e.src, rawsrc, sizeof e.src);
        e.px=px; e.w=w; e.h=h;
        __builtin_snprintf(status_,sizeof status_,"Loaded %dx%d image",w,h);
        return n_imgs_++;
    }
    return -1;
}

/* ---- link hit-test ----------------------------------------------------- */

int Browser::link_at(int x, int y) const {
    if (y < CONTENT_TOP) return -1;
    int content_x = MARGIN + PAGE_PAD;
    int docx = x - content_x, docy = (y - CONTENT_TOP) + scroll_;
    for (int i=0; i<layout_.run_count; ++i) {
        const wl_run &r = layout_.runs[i];
        if (r.kind!=WL_TEXT || r.link<0) continue;
        if (docx>=r.x&&docx<r.x+r.w&&docy>=r.y&&docy<r.y+r.h) return r.link;
    }
    return -1;
}

bool Browser::hit_link(int x, int y, char *out, int cap) const {
    int li = link_at(x, y);
    if (li < 0) return false;
    resolve_href(layout_.hrefs[li], out, cap);
    return out[0] != '\0';
}

/* ---- rendering --------------------------------------------------------- */

void Browser::render() {
    int bar_cols = (win_w_ - 2*MARGIN - 2*BTN_W - 8) / 8;
    int content_x = MARGIN + PAGE_PAD;

    fill(0, 0, win_w_, win_h_, COL_BG);

    /* ---- Chrome: back / forward buttons -------------------------------- */
    bool can_back    = (hist_pos_ > 0);
    bool can_forward = (hist_pos_ + 1 < hist_count_);
    fill(0,        0, BTN_W, BAR_H, can_back    ? COL_BTN : COL_BAR);
    fill(BTN_W,    0, BTN_W, BAR_H, can_forward ? COL_BTN : COL_BAR);
    draw_text(8,    7, "<", can_back    ? COL_TEXT_CHR : COL_DIM);
    draw_text(8+BTN_W, 7, ">", can_forward ? COL_TEXT_CHR : COL_DIM);

    /* ---- Chrome: URL bar ---------------------------------------------- */
    int bar_x = 2 * BTN_W;
    fill(bar_x, 0, win_w_ - bar_x, BAR_H, editing_ ? COL_BAR_HI : COL_BAR);
    draw_text(bar_x + 4, 7, editing_ ? "URL:" : " ", COL_ACCENT);

    {
        int txt_x = bar_x + 40;
        int start = 0; if (url_len_ > bar_cols) start = url_len_ - bar_cols;
        draw_text_n(txt_x, 7, url_ + start, bar_cols, COL_TEXT_CHR);
        if (editing_) {
            int cx = txt_x + (url_len_ - start) * 8;
            fill(cx, 4, 2, BAR_H - 8, COL_ACCENT);  /* text cursor */
        }
    }

    /* ---- Chrome: status bar ------------------------------------------- */
    fill(0, BAR_H, win_w_, STATUS_H + 4, COL_BAR);
    draw_text(MARGIN, BAR_H + 2, status_, COL_DIM);

    /* ---- Page paper area ----------------------------------------------- */
    fill(MARGIN, CONTENT_TOP, page_w(), win_h_ - CONTENT_TOP, COL_PAGE);

    /* ---- Content: positioned runs offset by scroll --------------------- */
    clamp_scroll();
    for (int i = 0; i < layout_.run_count; ++i) {
        const wl_run &r = layout_.runs[i];
        int sy = CONTENT_TOP + r.y - scroll_;
        int rx = content_x + r.x;
        if (sy + r.h < CONTENT_TOP || sy >= win_h_) continue;

        if (r.kind == WL_RECT) {
            int top = sy, h = r.h;
            if (top < CONTENT_TOP) { h -= (CONTENT_TOP - top); top = CONTENT_TOP; }
            if (h > 0) fill(rx, top, r.w, h, r.color);
            continue;
        }
        if (r.kind == WL_IMAGE) {
            ImgEntry *im = img_find(r.off>=0&&r.off<layout_.href_count
                                    ? layout_.hrefs[r.off] : "");
            if (im && im->px) {
                /* Nearest-neighbour scale to fit the reserved box. */
                for (int yy=0; yy<r.h; ++yy) {
                    int py = sy + yy;
                    if (py < CONTENT_TOP || py >= win_h_) continue;
                    int syi = (int)((long)yy * im->h / r.h);
                    for (int xx=0; xx<r.w; ++xx) {
                        int pxx = rx + xx; if (pxx<0||pxx>=win_w_) continue;
                        int sxi = (int)((long)xx * im->w / r.w);
                        canvas_[py * win_w_ + pxx] = im->px[syi * im->w + sxi];
                    }
                }
            } else if (sy >= CONTENT_TOP) {
                fill(rx, sy, r.w, r.h, 0x00e0e4ecu);  /* grey placeholder */
            }
            continue;
        }
        if (r.kind == WL_RULE) {
            if (sy >= CONTENT_TOP) fill(rx, sy, r.w, r.h, r.color);
            continue;
        }
        if (r.kind == WL_BULLET) {
            int rad = r.px/7 + 2, cx = rx + rad + 2, cy = sy + r.h/2;
            for (int yy=-rad; yy<=rad; ++yy)
                for (int xx=-rad; xx<=rad; ++xx)
                    if (xx*xx+yy*yy<=rad*rad && cy+yy>=CONTENT_TOP)
                        if (cx+xx>=0&&cx+xx<win_w_&&cy+yy<win_h_)
                            canvas_[(cy+yy)*win_w_+(cx+xx)] = r.color;
            continue;
        }
        /* WL_TEXT */
        if (r.link >= 0 && r.link == hover_link_ && sy >= CONTENT_TOP)
            fill(rx-1, sy, r.w+2, r.h, COL_HOVER);
        else if (r.bg && sy >= CONTENT_TOP)
            fill(rx-1, sy, r.w+2, r.h, r.bg);
        draw_run_text(rx, sy, &r);
        if (r.underline && sy + r.h - 2 >= CONTENT_TOP)
            fill(rx, sy + r.h - 2, r.w, 1, r.color);
    }

    /* ---- Scrollbar ----------------------------------------------------- */
    if (layout_.height > content_h()) {
        int track_h = win_h_ - CONTENT_TOP;
        int knob_h  = track_h * content_h() / layout_.height; if (knob_h < 10) knob_h = 10;
        int knob_y  = CONTENT_TOP + (track_h - knob_h) * scroll_
                      / (layout_.height - content_h());
        fill(win_w_ - SCROLLBAR_W, CONTENT_TOP, SCROLLBAR_W, track_h, COL_SCROLLBG);
        fill(win_w_ - SCROLLBAR_W, knob_y, SCROLLBAR_W, knob_h, COL_ACCENT);
    }

    present();
}

/* ---- event handlers ---------------------------------------------------- */

void Browser::on_key(uint32_t k) {
    if (editing_) {
        if (k == '\n' || k == '\r') {
            editing_ = false;
            navigate(url_);
        } else if (k == 0x08) {  /* Backspace */
            url_backspace();
        } else if (k == 0x1b) {  /* Escape */
            editing_ = false;
        } else if (k >= 0x20 && k < 0x7f) {
            url_putc(static_cast<char>(k));
        }
        return;
    }
    switch (k) {
    case 'u': case '/':  editing_ = true; break;
    case 'j':            scroll_ += 20; clamp_scroll(); break;
    case 'k':            scroll_ -= 20; clamp_scroll(); break;
    case ' ': case 'f':  scroll_ += content_h() - 20; clamp_scroll(); break;
    case 'b':            scroll_ -= content_h() - 20; clamp_scroll(); break;
    case 'g':            scroll_ = 0; break;
    case 'G':            scroll_ = layout_.height; clamp_scroll(); break;
    case 'r':            if (html_buf_) { layout_current(); } break;
    case '[':
        if (hist_pos_ > 0) {
            --hist_pos_;
            __builtin_strncpy(url_,history_[hist_pos_],sizeof url_);
            url_len_ = (int)__builtin_strlen(url_);
            load_url(url_);  /* load without pushing history */
        }
        break;
    case ']':
        if (hist_pos_ + 1 < hist_count_) {
            ++hist_pos_;
            __builtin_strncpy(url_,history_[hist_pos_],sizeof url_);
            url_len_ = (int)__builtin_strlen(url_);
            load_url(url_);
        }
        break;
    default: break;
    }
}

void Browser::on_mouse_move(int x, int y) {
    int hl = editing_ ? -1 : link_at(x, y);
    if (hl != hover_link_) hover_link_ = hl;
}

void Browser::on_click(int x, int y) {
    /* Back button */
    if (y >= 0 && y < BAR_H && x < BTN_W && hist_pos_ > 0) {
        --hist_pos_;
        __builtin_strncpy(url_,history_[hist_pos_],sizeof url_);
        url_len_ = (int)__builtin_strlen(url_);
        load_url(url_); return;
    }
    /* Forward button */
    if (y >= 0 && y < BAR_H && x >= BTN_W && x < 2*BTN_W && hist_pos_+1 < hist_count_) {
        ++hist_pos_;
        __builtin_strncpy(url_,history_[hist_pos_],sizeof url_);
        url_len_ = (int)__builtin_strlen(url_);
        load_url(url_); return;
    }
    /* Click on URL bar */
    if (y >= 0 && y < BAR_H && x >= 2*BTN_W) { editing_ = true; return; }
    /* Click on page */
    if (!editing_) {
        char href[1024];
        if (hit_link(x, y, href, sizeof href)) {
            __builtin_strncpy(url_, href, sizeof url_);
            url_len_ = (int)__builtin_strlen(url_);
            navigate(url_);
        }
    }
}

void Browser::on_scroll(int dy) {
    scroll_ += dy * 3 * 20;
    clamp_scroll();
}

void Browser::on_resize(int w, int h) {
    if (w < 80) w = 80; if (h < 60) h = 60;
    if (w > BROW_MAX_W) w = BROW_MAX_W;
    if (h > BROW_MAX_H) h = BROW_MAX_H;
    win_w_ = w; win_h_ = h;
    if (html_buf_) layout_current();
}

/* ---- URL bar ----------------------------------------------------------- */

void Browser::url_putc(char c) {
    if (url_len_ < (int)sizeof(url_) - 1) { url_[url_len_++] = c; url_[url_len_] = '\0'; }
}
void Browser::url_backspace() {
    if (url_len_ > 0) url_[--url_len_] = '\0';
}
void Browser::url_clear() {
    url_len_ = 0; url_[0] = '\0';
}

/* ---- entry point ------------------------------------------------------- */

int main() {
    static Browser browser;
    browser.run();
}
