/* layout_engine.cpp — improved HTML flow-layout engine.
 *
 * All internal state lives in the file-local `State` struct that is passed
 * through every function.  The public LayoutEngine class is a thin wrapper
 * that holds the two font-metric callbacks and the image sizer.
 *
 * Block-margin collapsing
 * -----------------------
 * State::pending_margin tracks the largest margin emitted since the last
 * visible run.  block_break() advances cy by max(requested, pending) so that
 * two adjacent block elements (e.g. </p><p>) produce only one gap instead of
 * doubling up.  Any visible run resets pending_margin to zero.
 *
 * Skipped elements
 * ----------------
 * <script>, <style>, <head>, <title>, <meta>, <link>, <noscript>: metadata.
 * CSS display:none is honoured.  Everything else renders. */

#include "layout_engine.h"
#include "../umalloc.h"
#include <cstring>

/* ---- colour palette (light paper page theme) --------------------------- */
static const unsigned COL_TEXT    = 0x202632u;
static const unsigned COL_LINK    = 0x1a56dbu;
static const unsigned COL_HEAD    = 0x0d1b2au;
static const unsigned COL_QUOTE   = 0x566072u;
static const unsigned COL_RULE    = 0xc8cedau;
static const unsigned COL_CODEBG  = 0xe7eaf2u;
static const unsigned COL_ACCENT  = 0x4a90d9u;
static const int      BODY_PX     = 17;
static const unsigned COL_PREBG   = 0xe2e5f0u;

/* ---- small helpers ----------------------------------------------------- */

static bool ieq(const char *a, const char *b) {
    for (; *a && *b; ++a, ++b) if ((*a | 32) != (*b | 32)) return false;
    return *a == '\0' && *b == '\0';
}
static bool ieq_pfx(const char *s, const char *pfx) {
    for (; *pfx; ++s, ++pfx) if ((*s | 32) != (*pfx | 32)) return false;
    return true;
}
static bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

/* ---- wl_doc growable-storage helpers (mirrors weblayout.c internals) --- */

static int pool_append(wl_doc *d, const char *s, int n) {
    int off = d->pool_len;
    if (d->pool_len + n + 1 > d->pool_cap) {
        int nc = d->pool_cap ? d->pool_cap * 2 : 4096;
        while (nc < d->pool_len + n + 1) nc *= 2;
        char *np = static_cast<char *>(urealloc(d->pool, (umsize_t)nc));
        if (!np) return -1;
        d->pool = np; d->pool_cap = nc;
    }
    __builtin_memcpy(d->pool + off, s, (unsigned)n);
    d->pool_len += n;
    return off;
}

static wl_run *add_run(wl_doc *d) {
    if (d->run_count >= d->run_cap) {
        int nc = d->run_cap ? d->run_cap * 2 : 128;
        wl_run *nr = static_cast<wl_run *>(urealloc(d->runs, (umsize_t)nc * (int)sizeof(wl_run)));
        if (!nr) return nullptr;
        d->runs = nr; d->run_cap = nc;
    }
    return &d->runs[d->run_count++];
}

static int add_href(wl_doc *d, const char *href) {
    if (d->href_count >= d->href_cap) {
        int nc = d->href_cap ? d->href_cap * 2 : 32;
        char (*nh)[WL_HREF_MAX] = static_cast<char(*)[WL_HREF_MAX]>(
            urealloc(d->hrefs, (umsize_t)nc * WL_HREF_MAX));
        if (!nh) return -1;
        d->hrefs = nh; d->href_cap = nc;
    }
    int i = 0;
    for (; href[i] && i < WL_HREF_MAX - 1; ++i) d->hrefs[d->href_count][i] = href[i];
    d->hrefs[d->href_count][i] = '\0';
    return d->href_count++;
}

/* Append a form-control descriptor; returns its index (or -1). */
static int add_field(wl_doc *d) {
    if (d->field_count >= d->field_cap) {
        int nc = d->field_cap ? d->field_cap * 2 : 16;
        wl_field *nf = static_cast<wl_field *>(
            urealloc(d->fields, (umsize_t)nc * (int)sizeof(wl_field)));
        if (!nf) return -1;
        d->fields = nf; d->field_cap = nc;
    }
    wl_field *f = &d->fields[d->field_count];
    f->kind = WLF_TEXT; f->name[0] = '\0'; f->value[0] = '\0';
    f->action[0] = '\0'; f->method_post = 0;
    return d->field_count++;
}

static void str_copy(char *dst, int cap, const char *src) {
    int i = 0;
    for (; src && src[i] && i < cap - 1; ++i) dst[i] = src[i];
    dst[i] = '\0';
}

/* ---- inline style parsing ---------------------------------------------- */

static bool parse_color(const char *s, unsigned &out) {
    while (*s == ' ') ++s;
    if (ieq_pfx(s, "rgb") || ieq_pfx(s, "rgba")) {
        const char *p = s;
        while (*p && *p != '(') ++p;
        if (*p == '(') {
            ++p;
            auto next_num = [&p]() {
                while (*p && (*p == ' ' || *p == ',')) ++p;
                int v = 0;
                while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); ++p; }
                return v;
            };
            int r = next_num(), g = next_num(), b = next_num();
            /* parse optional alpha: "0", "0.0", "0.5", "1", "1.0" */
            while (*p == ' ' || *p == ',') ++p;
            bool has_alpha = (*p >= '0' && *p <= '9') || *p == '.';
            if (has_alpha) {
                int whole = 0;
                while (*p >= '0' && *p <= '9') { whole = whole * 10 + (*p - '0'); ++p; }
                int frac100 = 0;
                if (*p == '.') { ++p; int div=10; while (*p>='0'&&*p<='9'&&div<=100){frac100+=(*p-'0')*100/div;div*=10;++p;} }
                int alpha100 = whole * 100 + frac100;
                if (alpha100 == 0) return false; /* fully transparent — don't apply color */
            }
            if (r >= 0 && r <= 255 && g >= 0 && g <= 255 && b >= 0 && b <= 255) {
                out = ((unsigned)r << 16) | ((unsigned)g << 8) | (unsigned)b;
                return true;
            }
        }
    }
    if (*s == '#') {
        ++s;
        auto hd = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            c |= 32; if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            return -1;
        };
        int d[6], k = 0;
        while (k < 6 && hd(s[k]) >= 0) { d[k] = hd(s[k]); ++k; }
        if (k >= 6) { out = ((unsigned)(d[0]*16+d[1])<<16)|((unsigned)(d[2]*16+d[3])<<8)|(unsigned)(d[4]*16+d[5]); return true; }
        if (k >= 3) { out = ((unsigned)(d[0]*17)<<16)|((unsigned)(d[1]*17)<<8)|(unsigned)(d[2]*17); return true; }
        return false;
    }
    struct { const char *n; unsigned c; } tbl[] = {
        {"black",0x000000},{"white",0xffffff},{"red",0xcc0000},{"green",0x008000},
        {"blue",0x1a56db},{"navy",0x001f5f},{"gray",0x808080},{"grey",0x808080},
        {"silver",0xc0c0c0},{"orange",0xff6600},{"purple",0x800080},
        {"teal",0x008080},{"maroon",0x800000},{"olive",0x808000},{"yellow",0xd4b000},
        /* extended names */
        {"darkblue",0x00008b},{"darkgreen",0x006400},{"darkred",0x8b0000},
        {"darkgray",0xa9a9a9},{"darkgrey",0xa9a9a9},{"lightgray",0xd3d3d3},
        {"lightgrey",0xd3d3d3},{"lightblue",0xadd8e6},{"lightgreen",0x90ee90},
        {"lightyellow",0xffffe0},{"lightcoral",0xf08080},{"lightsalmon",0xffa07a},
        {"lightpink",0xffb6c1},{"lightskyblue",0x87cefa},{"lightsteelblue",0xb0c4de},
        {"pink",0xffb6c1},{"coral",0xff7f50},{"tomato",0xff6347},{"salmon",0xfa8072},
        {"crimson",0xdc143c},{"firebrick",0xb22222},{"indianred",0xcd5c5c},
        {"gold",0xffd700},{"goldenrod",0xdaa520},{"khaki",0xf0e68c},
        {"cyan",0x00ced1},{"aqua",0x00ced1},{"turquoise",0x40e0d0},
        {"magenta",0xff00ff},{"fuchsia",0xff00ff},{"violet",0xee82ee},
        {"orchid",0xda70d6},{"plum",0xdda0dd},{"indigo",0x4b0082},
        {"steelblue",0x4682b4},{"cornflowerblue",0x6495ed},{"royalblue",0x4169e1},
        {"dodgerblue",0x1e90ff},{"deepskyblue",0x00bfff},{"skyblue",0x87ceeb},
        {"cadetblue",0x5f9ea0},{"mediumblue",0x0000cd},{"slateblue",0x6a5acd},
        {"chocolate",0xd2691e},{"saddlebrown",0x8b4513},{"sienna",0xa0522d},
        {"peru",0xcd853f},{"tan",0xd2b48c},{"brown",0xa52a2a},{"wheat",0xf5deb3},
        {"beige",0xf5f5dc},{"ivory",0xfffff0},{"linen",0xfaf0e6},
        {"lavender",0xe6e6fa},{"lavenderblush",0xfff0f5},{"mistyrose",0xffe4e1},
        {"seashell",0xfff5ee},{"ghostwhite",0xf8f8ff},{"whitesmoke",0xf5f5f5},
        {"aliceblue",0xf0f8ff},{"azure",0xf0ffff},{"honeydew",0xf0fff0},
        {"mintcream",0xf5fffa},{"snow",0xfffafa},{"floralwhite",0xfffaf0},
        {"oldlace",0xfdf5e6},{"antiquewhite",0xfaebd7},{"bisque",0xffe4c4},
        {"moccasin",0xffe4b5},{"navajowhite",0xffdead},{"peachpuff",0xffdab9},
        {"papayawhip",0xffefd5},{"blanchedalmond",0xffebcd},{"lemonchiffon",0xfffacd},
        {"chartreuse",0x7fff00},{"greenyellow",0xadff2f},{"lawngreen",0x7cfc00},
        {"lime",0x00ff00},{"limegreen",0x32cd32},{"springgreen",0x00ff7f},
        {"mediumseagreen",0x3cb371},{"seagreen",0x2e8b57},{"forestgreen",0x228b22},
        {"darkseagreen",0x8fbc8f},{"mediumaquamarine",0x66cdaa},{"aquamarine",0x7fffd4},
        {"dimgray",0x696969},{"dimgrey",0x696969},{"slategray",0x708090},
        {"slategrey",0x708090},{"darkslategray",0x2f4f4f},
        {nullptr,0}};
    for (int i = 0; tbl[i].n; ++i) if (ieq(s, tbl[i].n)) { out = tbl[i].c; return true; }
    return false;
}

/* Minimal CSS declaration parser: feeds into a style accumulator. */
struct StyleDecls {
    int      px;
    bool     bold;
    bool     underline;
    bool     italic;
    bool     strikethrough;
    unsigned color;
    unsigned bg;
    bool     hidden;
    int      width;
    int      margin_top;
    int      margin_bottom;
    int      padding_top;
    int      padding_bottom;
    int      padding_left;
    int      padding_right;
    int      text_align;      /* 0=left, 1=center, 2=right (−1=unset) */
    int      line_height_pct; /* e.g. 160 = 1.6× (0=unset) */
    int      max_width;
    unsigned border_color;
    int      border_width;
    bool     text_transform;
    bool     list_style_none;  /* suppress list bullet */
    bool     pos_fixed;        /* position: fixed → skip subtree */
};

static void apply_decls(const char *css, StyleDecls &st) {
    int i = 0;
    while (css[i]) {
        char prop[28] = {}, val[80] = {};
        int p = 0, v = 0;
        while (css[i] && css[i] != ':' && css[i] != ';')
            { if (css[i] != ' ' && p < 27) prop[p++] = css[i]; ++i; }
        if (css[i] == ':') {
            ++i; while (css[i] == ' ') ++i;
            while (css[i] && css[i] != ';') { if (v < 79) val[v++] = css[i]; ++i; }
        }
        val[v] = '\0'; prop[p] = '\0';
        if (css[i] == ';') ++i;

        if (ieq(prop,"display"))         { const char *w=val; while(*w==' ')++w; st.hidden=ieq(w,"none"); }
        else if (ieq(prop,"color"))      { unsigned c; if(parse_color(val,c)) st.color=c; }
        else if (ieq(prop,"background-color")||ieq(prop,"background"))
                                         { unsigned c; if(parse_color(val,c)) st.bg=c; }
        else if (ieq(prop,"font-weight")){ const char *w=val; while(*w==' ')++w;
                                           st.bold=ieq(w,"bold")||ieq(w,"bolder")||ieq(w,"700")||ieq(w,"800")||ieq(w,"900"); }
        else if (ieq(prop,"font-style")) { const char *w=val; while(*w==' ')++w; st.italic=ieq(w,"italic")||ieq(w,"oblique"); }
        else if (ieq(prop,"text-decoration")){
            for(int j=0;val[j];++j){
                if(ieq(val+j,"underline")){st.underline=true;}
                if(ieq(val+j,"line-through")){st.strikethrough=true;}
            }
        }
        else if (ieq(prop,"font-size"))  { const char *w=val; int px=0; while(*w==' ')++w;
                                           while(*w>='0'&&*w<='9'){px=px*10+(*w-'0');++w;} if(px>=10&&px<=96) st.px=px; }
        else if (ieq(prop,"visibility")) { const char *w=val; while(*w==' ')++w; if(ieq(w,"hidden")) st.hidden=true; }
        else if (ieq(prop,"width"))      { const char *w=val; int px=0; while(*w==' ')++w;
                                           while(*w>='0'&&*w<='9'){px=px*10+(*w-'0');++w;} if(px>0&&px<=2000) st.width=px; }
        else if (ieq(prop,"margin-top")) { const char *w=val; int px=0; while(*w==' ')++w;
                                           while(*w>='0'&&*w<='9'){px=px*10+(*w-'0');++w;} st.margin_top=px; }
        else if (ieq(prop,"margin-bottom")) { const char *w=val; int px=0; while(*w==' ')++w;
                                           while(*w>='0'&&*w<='9'){px=px*10+(*w-'0');++w;} st.margin_bottom=px; }
        else if (ieq(prop,"margin"))     { const char *w=val; int px=0; while(*w==' ')++w;
                                           while(*w>='0'&&*w<='9'){px=px*10+(*w-'0');++w;} st.margin_top=px; st.margin_bottom=px; }
        else if (ieq(prop,"padding-top"))    { const char *w=val; int px=0; while(*w==' ')++w;
                                           while(*w>='0'&&*w<='9'){px=px*10+(*w-'0');++w;} st.padding_top=px; }
        else if (ieq(prop,"padding-bottom")) { const char *w=val; int px=0; while(*w==' ')++w;
                                           while(*w>='0'&&*w<='9'){px=px*10+(*w-'0');++w;} st.padding_bottom=px; }
        else if (ieq(prop,"padding-left"))   { const char *w=val; int px=0; while(*w==' ')++w;
                                           while(*w>='0'&&*w<='9'){px=px*10+(*w-'0');++w;} st.padding_left=px; }
        else if (ieq(prop,"padding-right"))  { const char *w=val; int px=0; while(*w==' ')++w;
                                           while(*w>='0'&&*w<='9'){px=px*10+(*w-'0');++w;} st.padding_right=px; }
        else if (ieq(prop,"padding"))    {
            const char *w=val; int px=0; while(*w==' ')++w;
            while(*w>='0'&&*w<='9'){px=px*10+(*w-'0');++w;}
            if(px>0&&px<=200){ st.padding_top=px; st.padding_bottom=px; st.padding_left=px; st.padding_right=px; }
        }
        else if (ieq(prop,"max-width"))  { const char *w=val; int px=0; while(*w==' ')++w;
                                           while(*w>='0'&&*w<='9'){px=px*10+(*w-'0');++w;} if(px>0&&px<=4000) st.max_width=px; }
        else if (ieq(prop,"border")||ieq(prop,"border-top")||ieq(prop,"border-bottom")||
                 ieq(prop,"border-left")||ieq(prop,"border-right")||ieq(prop,"outline")) {
            /* parse "1px solid #ccc" or "none" */
            const char *w=val; while(*w==' ')++w;
            if(ieq(w,"none")||ieq(w,"0")){ st.border_width=0; st.border_color=0; }
            else {
                int bw=0; while(*w>='0'&&*w<='9'){bw=bw*10+(*w-'0');++w;}
                while(*w&&*w!=' ')++w; while(*w==' ')++w; /* skip 'px solid ...' */
                while(*w&&*w!=' ')++w; while(*w==' ')++w;
                unsigned bc=0xc8cedau; parse_color(w,bc);
                if(bw>0&&bw<=8){ st.border_width=bw; st.border_color=bc; }
            }
        }
        else if (ieq(prop,"border-color")) { unsigned c=0; if(parse_color(val,c)) st.border_color=c; }
        else if (ieq(prop,"border-width")) { const char *w=val; int px=0; while(*w==' ')++w;
                                           while(*w>='0'&&*w<='9'){px=px*10+(*w-'0');++w;} if(px>0&&px<=8) st.border_width=px; }
        else if (ieq(prop,"text-transform")){ const char *w=val; while(*w==' ')++w; st.text_transform=ieq(w,"uppercase")||ieq(w,"capitalize"); }
        else if (ieq(prop,"list-style")||ieq(prop,"list-style-type")) {
            const char *w=val; while(*w==' ')++w; st.list_style_none=ieq(w,"none"); }
        else if (ieq(prop,"position")) {
            const char *w=val; while(*w==' ')++w;
            st.pos_fixed=(ieq(w,"fixed")||ieq(w,"sticky")); }
        else if (ieq(prop,"text-align")) {
            const char *w=val; while(*w==' ')++w;
            if(ieq(w,"center")) st.text_align=1;
            else if(ieq(w,"right")) st.text_align=2;
            else if(ieq(w,"left")) st.text_align=0;
        }
        else if (ieq(prop,"line-height")) {
            const char *w=val; while(*w==' ')++w;
            /* Parse: "1.5" → 150, "160%" → 160, "24px" → skip (complex) */
            int whole=0, frac=0, frac_div=1;
            while(*w>='0'&&*w<='9'){whole=whole*10+(*w-'0');++w;}
            if(*w=='.'){++w; while(*w>='0'&&*w<='9'&&frac_div<100){frac=frac*10+(*w-'0');frac_div*=10;++w;}}
            int pct = whole*100 + frac*100/frac_div;
            if(*w=='%') pct = whole*100 + frac*100/frac_div; /* same */
            else if(*w!='p'/* px */) pct = pct; /* unitless: treat as percent×100 */
            if(pct>=80&&pct<=300) st.line_height_pct=pct;
        }
    }
}

/* ---- style stack ------------------------------------------------------- */

struct Style {
    int      px        = BODY_PX;
    bool     bold      = false;
    bool     underline = false;
    bool     italic    = false;
    bool     strikethrough = false;
    unsigned color     = COL_TEXT;
    unsigned bg        = 0;
    int      link      = -1;
    int      left      = 0;
    bool     hidden    = false;
    bool     pre       = false;
    int      width     = 0;
    int      margin_top    = -1;
    int      margin_bottom = -1;
    int      padding_top    = -1;
    int      padding_bottom = -1;
    int      padding_left   = -1;
    int      padding_right  = -1;
    int      text_align      = 0;
    int      line_height_pct = 0;
    int      max_width       = 0;
    unsigned border_color    = 0;
    int      border_width    = 0;
    bool     text_transform  = false;
    bool     list_style_none = false;
    bool     pos_fixed       = false;
};

/* ---- layout state ------------------------------------------------------ */

struct State {
    wl_doc   *doc            = nullptr;
    int       vw             = 0;
    int       cx             = 0;
    int       cy             = 0;
    int       line_h         = 0;
    bool      at_sol         = true;
    bool      pend_space     = false;
    int       pending_margin = 0;
    int       line_start_run = 0;  /* run index at start of current line (for text-align) */
    Style     stk[40];
    int       sp             = 0;
    css_sheet *sheet         = nullptr;

    /* Current <form> context (raw action; browser resolves + submits). */
    char      form_action[256] = {};
    int       form_method_post = 0;

    /* font callbacks */
    int  (*adv_fn)(int cp, int px)  = nullptr;
    int  (*lh_fn)(int px)           = nullptr;
    int  (*img_fn)(const char *src, int maxw, int *w, int *h) = nullptr;

    int  adv(int cp, int px) const  { return adv_fn ? adv_fn(cp, px) : px/2; }
    int  lh (int px)         const  { return lh_fn  ? lh_fn(px)      : px+3; }
};

static void st_push(State &S) { if(S.sp<39){S.stk[S.sp+1]=S.stk[S.sp];++S.sp;} }
static void st_pop (State &S) { if(S.sp>0) --S.sp; }

/* Apply CSS cascade to the top of the style stack. `width` is reset to 0 first
 * so it applies per-element (it must not inherit to children). */
static void apply_css(State &S, dom_node *node) {
    StyleDecls d{S.stk[S.sp].px, S.stk[S.sp].bold, S.stk[S.sp].underline,
                 S.stk[S.sp].italic, S.stk[S.sp].strikethrough,
                 S.stk[S.sp].color, S.stk[S.sp].bg, S.stk[S.sp].hidden,
                 0, -1, -1, -1, -1, -1, -1, -1, 0, 0, 0, 0, false, false, false};
    if (S.sheet) { char buf[640]; css_match(S.sheet, node, buf, sizeof buf); apply_decls(buf, d); }
    const char *inl = dom_attr(node,"style"); if (inl) apply_decls(inl, d);
    S.stk[S.sp].px            = d.px;
    S.stk[S.sp].bold          = d.bold;
    S.stk[S.sp].underline     = d.underline;
    S.stk[S.sp].italic        = d.italic;
    S.stk[S.sp].strikethrough = d.strikethrough;
    S.stk[S.sp].color         = d.color;
    S.stk[S.sp].bg            = d.bg;
    S.stk[S.sp].hidden        = d.hidden;
    S.stk[S.sp].width         = d.width;
    S.stk[S.sp].margin_top    = d.margin_top;
    S.stk[S.sp].margin_bottom = d.margin_bottom;
    S.stk[S.sp].padding_top    = d.padding_top;
    S.stk[S.sp].padding_bottom = d.padding_bottom;
    S.stk[S.sp].padding_left   = d.padding_left;
    S.stk[S.sp].padding_right  = d.padding_right;
    if (d.text_align >= 0)      S.stk[S.sp].text_align = d.text_align;
    if (d.line_height_pct > 0)  S.stk[S.sp].line_height_pct = d.line_height_pct;
    S.stk[S.sp].max_width      = d.max_width;
    if (d.border_width > 0)   { S.stk[S.sp].border_color = d.border_color; S.stk[S.sp].border_width = d.border_width; }
    if (d.text_transform)       S.stk[S.sp].text_transform = true;
    if (d.list_style_none)      S.stk[S.sp].list_style_none = true;
    if (d.pos_fixed)            S.stk[S.sp].pos_fixed = true;
}

static bool check_hidden(State &S, dom_node *node) {
    StyleDecls d{BODY_PX,false,false,false,false,COL_TEXT,0,false,0,-1,-1,-1,-1,-1,-1,-1,0,0,0,0,false,false,false};
    if (S.sheet) { char buf[640]; css_match(S.sheet, node, buf, sizeof buf); apply_decls(buf, d); }
    const char *inl = dom_attr(node,"style"); if (inl) apply_decls(inl, d);
    return d.hidden || d.pos_fixed;
}

/* ---- layout primitives ------------------------------------------------- */

/* Shift runs on the current line for text-align center/right. */
static void apply_line_align(State &S) {
    int align = S.stk[S.sp].text_align;
    if (align == 0 || S.at_sol) return;
    int line_w = S.cx - S.stk[S.sp].left;
    int avail  = S.vw - S.stk[S.sp].left;
    int shift  = (align == 1) ? (avail - line_w) / 2 : (avail - line_w);
    if (shift <= 0) return;
    for (int i = S.line_start_run; i < S.doc->run_count; ++i)
        S.doc->runs[i].x += shift;
}

static void newline(State &S) {
    apply_line_align(S);
    S.cy += S.line_h > 0 ? S.line_h : S.lh(BODY_PX);
    S.cx = S.stk[S.sp].left;
    S.line_h = 0; S.at_sol = true; S.pend_space = false;
    S.line_start_run = S.doc->run_count;
}

/* Block break with margin collapsing: advance cy only by the delta above
 * whatever space was already pending. */
static void block_break(State &S, int margin) {
    if (!S.at_sol) newline(S);
    int add = margin - S.pending_margin;
    if (add > 0) { S.cy += add; S.pending_margin = margin; }
    S.at_sol = true; S.pend_space = false;
}

/* Compute pixel width of a text run. */
static int word_w(State &S, const char *s, int n, int px) {
    int w = 0;
    for (int i = 0; i < n; ++i) w += S.adv((unsigned char)s[i], px);
    return w;
}

static void place_word(State &S, const char *s, int n) {
    const Style &st = S.stk[S.sp];
    /* text-transform: uppercase — copy to local buf and upper-case */
    char upper_buf[512];
    if (st.text_transform && n < 511) {
        for (int k = 0; k < n; ++k) {
            char c = s[k];
            upper_buf[k] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
        }
        upper_buf[n] = '\0';
        s = upper_buf;
    }
    int px = st.px;
    int h  = st.line_height_pct > 0 ? (S.lh(px) * st.line_height_pct / 100) : S.lh(px);
    if (h < S.lh(px)) h = S.lh(px); /* never shorter than natural height */
    int w = word_w(S, s, n, px);
    int gap = (S.pend_space && !S.at_sol) ? S.adv(' ', px) : 0;

    /* Word wrap */
    if (!S.at_sol && S.cx + gap + w > S.vw) { newline(S); gap = 0; }
    else S.cx += gap;
    S.pend_space = false;

    int off = pool_append(S.doc, s, n);
    if (off < 0) return;
    wl_run *r = add_run(S.doc);
    if (!r) return;
    r->kind=WL_TEXT; r->x=S.cx; r->y=S.cy; r->w=w; r->h=h;
    r->px=px; r->bold=st.bold?1:0; r->underline=st.underline?1:0;
    r->italic=st.italic?1:0; r->strikethrough=st.strikethrough?1:0;
    r->color=st.color; r->bg=st.bg; r->off=off; r->len=n; r->link=st.link;
    S.cx += w; S.at_sol = false;
    S.pending_margin = 0;  /* visible run resets the margin accumulator */
    if (h > S.line_h) S.line_h = h;
}

static void place_text(State &S, const char *text) {
    if (!text) return;
    if (is_space(*text)) S.pend_space = true;
    int i = 0;
    while (text[i]) {
        if (is_space(text[i])) { S.pend_space = true; ++i; continue; }
        int s = i;
        while (text[i] && !is_space(text[i])) ++i;
        place_word(S, text + s, i - s);
    }
}

/* Whitespace-preserving text for <pre>. */
static void place_pre_text(State &S, const char *text) {
    if (!text) return;
    while (*text) {
        const char *end = text;
        while (*end && *end != '\n') ++end;
        if (end > text) place_word(S, text, (int)(end - text));
        if (*end == '\n') { newline(S); S.pend_space = false; }
        text = (*end == '\n') ? end + 1 : end;
    }
}

/* Emit a WL_RECT background for a block; returns run index for later patching. */
static int begin_bg(State &S, unsigned color) {
    if (!S.at_sol) newline(S);
    wl_run *r = add_run(S.doc); if (!r) return -1;
    int idx = S.doc->run_count - 1;
    r->kind=WL_RECT; r->x=S.stk[S.sp].left; r->y=S.cy-2;
    r->w=S.vw-S.stk[S.sp].left; r->h=0; r->color=color;
    r->px=r->bold=r->underline=r->off=r->len=0; r->bg=0; r->link=-1;
    return idx;
}
static void end_bg(State &S, int idx) {
    if (idx < 0 || idx >= S.doc->run_count) return;
    if (!S.at_sol) { /* include trailing partial line */ }
    wl_run *r = &S.doc->runs[idx];
    r->h = (S.cy + 4) - r->y; if (r->h < 0) r->h = 0;
}

/* Left accent bar for <blockquote>. */
static void accent_bar(State &S, int y_start, unsigned color) {
    wl_run *r = add_run(S.doc); if (!r) return;
    r->kind=WL_RECT; r->x=S.stk[S.sp].left-7; r->y=y_start;
    r->w=3; r->h=S.cy-y_start; r->color=color;
    r->px=r->bold=r->underline=r->off=r->len=0; r->bg=0; r->link=-1;
}

static void add_rule(State &S, unsigned color, int h) {
    block_break(S, 6);
    wl_run *r = add_run(S.doc);
    if (r) {
        r->kind=WL_RULE; r->x=S.stk[S.sp].left; r->y=S.cy;
        r->w=S.vw-S.stk[S.sp].left; r->h=h; r->color=color;
        r->px=r->bold=r->underline=r->off=r->len=0; r->bg=0; r->link=-1;
    }
    S.cy += h + 8; S.at_sol = true;
}

/* ---- DOM walker -------------------------------------------------------- */

static void walk(State &S, dom_node *node);

static void walk_children(State &S, dom_node *node) {
    for (dom_node *c = node->first_child; c; c = c->next_sibling)
        walk(S, c);
}

static void walk(State &S, dom_node *node) {
    if (!node) return;

    if (node->type == DOM_TEXT) {
        if (S.stk[S.sp].pre) place_pre_text(S, node->text);
        else                  place_text(S, node->text);
        return;
    }

    const char *t = node->tag;

    /* ---- Fully-skipped subtrees --------------------------------------- */
    if (ieq(t,"script") || ieq(t,"style")   || ieq(t,"head")     ||
        ieq(t,"noscript") || ieq(t,"template") ||
        ieq(t,"title")  || ieq(t,"meta")     || ieq(t,"link"))    return;

    if (check_hidden(S, node)) return;

    /* ---- Void / replaced elements ------------------------------------- */
    if (ieq(t,"br")) { newline(S); return; }
    if (ieq(t,"hr")) { add_rule(S, COL_RULE, 2); return; }

    /* ---- Images ------------------------------------------------------- */
    if (ieq(t,"img")) {
        const char *src = dom_attr(node,"src");
        if (!src || !src[0] || ieq_pfx(src,"data:")) src = dom_attr(node,"data-src");
        int iw=0, ih=0, maxw=S.vw-S.stk[S.sp].left;
        if (src && src[0] && S.img_fn && S.img_fn(src, maxw, &iw, &ih) && iw>0) {
            int hi = add_href(S.doc, src);
            if (!S.at_sol) newline(S);
            wl_run *r = add_run(S.doc);
            if (r) { r->kind=WL_IMAGE; r->x=S.stk[S.sp].left; r->y=S.cy;
                     r->w=iw; r->h=ih; r->off=hi; r->len=0; r->link=-1;
                     r->px=r->bold=r->underline=r->color=r->bg=0; }
            S.cy += ih + 4; S.at_sol=true; S.line_h=0; S.pending_margin=0;
        } else {
            const char *alt = dom_attr(node,"alt");
            if (alt && alt[0]) {
                st_push(S); S.stk[S.sp].color=COL_QUOTE;
                place_word(S,"[",1); place_text(S,alt); place_word(S,"]",1);
                st_pop(S);
            }
        }
        return;
    }

    /* ---- Headings ----------------------------------------------------- */
    if (ieq(t,"h1")) {
        st_push(S); S.stk[S.sp].px=30; S.stk[S.sp].bold=true; S.stk[S.sp].color=COL_HEAD;
        apply_css(S,node);
        int mt = S.stk[S.sp].margin_top >= 0 ? S.stk[S.sp].margin_top : 18;
        int mb = S.stk[S.sp].margin_bottom >= 0 ? S.stk[S.sp].margin_bottom : 10;
        block_break(S,mt); walk_children(S,node);
        add_rule(S,COL_RULE,1); block_break(S,mb); st_pop(S); return;
    }
    if (ieq(t,"h2")) {
        st_push(S); S.stk[S.sp].px=24; S.stk[S.sp].bold=true; S.stk[S.sp].color=COL_HEAD;
        apply_css(S,node);
        int mt = S.stk[S.sp].margin_top >= 0 ? S.stk[S.sp].margin_top : 16;
        int mb = S.stk[S.sp].margin_bottom >= 0 ? S.stk[S.sp].margin_bottom : 10;
        block_break(S,mt); walk_children(S,node); block_break(S,mb); st_pop(S); return;
    }
    if (ieq(t,"h3")) {
        st_push(S); S.stk[S.sp].px=20; S.stk[S.sp].bold=true; S.stk[S.sp].color=COL_HEAD;
        apply_css(S,node);
        int mt = S.stk[S.sp].margin_top >= 0 ? S.stk[S.sp].margin_top : 14;
        int mb = S.stk[S.sp].margin_bottom >= 0 ? S.stk[S.sp].margin_bottom : 8;
        block_break(S,mt); walk_children(S,node); block_break(S,mb); st_pop(S); return;
    }
    if (ieq(t,"h4")) {
        st_push(S); S.stk[S.sp].px=17; S.stk[S.sp].bold=true; S.stk[S.sp].color=COL_HEAD;
        apply_css(S,node);
        int mt = S.stk[S.sp].margin_top >= 0 ? S.stk[S.sp].margin_top : 12;
        int mb = S.stk[S.sp].margin_bottom >= 0 ? S.stk[S.sp].margin_bottom : 6;
        block_break(S,mt); walk_children(S,node); block_break(S,mb); st_pop(S); return;
    }
    if (ieq(t,"h5")||ieq(t,"h6")) {
        st_push(S); S.stk[S.sp].px=15; S.stk[S.sp].bold=true; S.stk[S.sp].color=COL_HEAD;
        apply_css(S,node);
        int mt = S.stk[S.sp].margin_top >= 0 ? S.stk[S.sp].margin_top : 10;
        int mb = S.stk[S.sp].margin_bottom >= 0 ? S.stk[S.sp].margin_bottom : 4;
        block_break(S,mt); walk_children(S,node); block_break(S,mb); st_pop(S); return;
    }

    /* ---- Inline formatting -------------------------------------------- */
    if (ieq(t,"b")||ieq(t,"strong"))
        { st_push(S); S.stk[S.sp].bold=true;      apply_css(S,node); walk_children(S,node); st_pop(S); return; }
    if (ieq(t,"i")||ieq(t,"em"))
        { st_push(S); S.stk[S.sp].italic=true; apply_css(S,node); walk_children(S,node); st_pop(S); return; }
    if (ieq(t,"u"))
        { st_push(S); S.stk[S.sp].underline=true; apply_css(S,node); walk_children(S,node); st_pop(S); return; }
    if (ieq(t,"small")||ieq(t,"sub")||ieq(t,"sup"))
        { st_push(S); if(S.stk[S.sp].px>11)S.stk[S.sp].px-=3; apply_css(S,node); walk_children(S,node); st_pop(S); return; }
    if (ieq(t,"span")||ieq(t,"label")||ieq(t,"abbr")||ieq(t,"cite")||
        ieq(t,"dfn") ||ieq(t,"q")   ||ieq(t,"time") ||ieq(t,"mark"))
        { st_push(S); apply_css(S,node); walk_children(S,node); st_pop(S); return; }
    if (ieq(t,"font")) {
        st_push(S);
        const char *cv = dom_attr(node,"color");
        if (cv) { unsigned c; if(parse_color(cv,c)) S.stk[S.sp].color=c; }
        apply_css(S,node); walk_children(S,node); st_pop(S); return;
    }
    if (ieq(t,"code")||ieq(t,"tt")||ieq(t,"kbd")||ieq(t,"samp")||ieq(t,"var"))
        { st_push(S); S.stk[S.sp].bg=COL_CODEBG; if(S.stk[S.sp].px>12)S.stk[S.sp].px--; apply_css(S,node); walk_children(S,node); st_pop(S); return; }
    if (ieq(t,"a")) {
        st_push(S); S.stk[S.sp].color=COL_LINK; S.stk[S.sp].underline=true;
        const char *href=dom_attr(node,"href");
        if (href&&href[0]) S.stk[S.sp].link=add_href(S.doc,href);
        apply_css(S,node); walk_children(S,node); st_pop(S); return;
    }

    /* ---- Pre-formatted code block ------------------------------------- */
    if (ieq(t,"pre")) {
        st_push(S); S.stk[S.sp].pre=true; S.stk[S.sp].bg=0;
        S.stk[S.sp].color=0x2a3a4au;
        if (S.stk[S.sp].px>13) S.stk[S.sp].px-=2;
        apply_css(S,node);
        int mt = S.stk[S.sp].margin_top >= 0 ? S.stk[S.sp].margin_top : 10;
        int mb = S.stk[S.sp].margin_bottom >= 0 ? S.stk[S.sp].margin_bottom : 10;
        block_break(S,mt);
        int bg_idx=begin_bg(S,COL_PREBG);
        S.cy += 8; S.cx=S.stk[S.sp].left+12;
        walk_children(S,node);
        if (!S.at_sol) newline(S);
        S.cy += 6;
        end_bg(S,bg_idx);
        block_break(S,mb);
        st_pop(S); return;
    }

    /* ---- Block quote -------------------------------------------------- */
    if (ieq(t,"blockquote")) {
        st_push(S); S.stk[S.sp].left+=22; S.stk[S.sp].color=COL_QUOTE;
        apply_css(S,node);
        int mt = S.stk[S.sp].margin_top >= 0 ? S.stk[S.sp].margin_top : 10;
        int mb = S.stk[S.sp].margin_bottom >= 0 ? S.stk[S.sp].margin_bottom : 10;
        block_break(S,mt);
        int y0=S.cy;
        walk_children(S,node);
        if (!S.at_sol) newline(S);
        accent_bar(S,y0,COL_ACCENT);
        block_break(S,mb);
        st_pop(S); return;
    }

    /* ---- Lists -------------------------------------------------------- */
    if (ieq(t,"ul")||ieq(t,"ol")||ieq(t,"dl"))
        { block_break(S,8); st_push(S); S.stk[S.sp].left+=20; apply_css(S,node); walk_children(S,node); st_pop(S); block_break(S,8); return; }
    if (ieq(t,"li")) {
        block_break(S,3);
        st_push(S); apply_css(S,node);
        if (!S.stk[S.sp].list_style_none) {
            wl_run *r=add_run(S.doc);
            if (r) { int px=S.stk[S.sp].px,h=S.lh(px);
                     r->kind=WL_BULLET; r->x=S.stk[S.sp].left; r->y=S.cy;
                     r->w=px; r->h=h; r->px=px; r->bold=0; r->underline=0;
                     r->italic=0; r->strikethrough=0;
                     r->color=S.stk[S.sp].color; r->bg=0; r->off=0; r->len=0; r->link=-1;
                     S.cx=S.stk[S.sp].left+px+4; S.at_sol=false; S.line_h=h; }
        }
        walk_children(S,node); st_pop(S);
        block_break(S,3); return;
    }
    if (ieq(t,"dt")) { block_break(S,4); st_push(S); S.stk[S.sp].bold=true; apply_css(S,node); walk_children(S,node); st_pop(S); block_break(S,2); return; }
    if (ieq(t,"dd")) { block_break(S,2); st_push(S); S.stk[S.sp].left+=16; apply_css(S,node); walk_children(S,node); st_pop(S); block_break(S,4); return; }

    /* ---- Tables ------------------------------------------------------- */
    if (ieq(t,"table")) { block_break(S,10); st_push(S); apply_css(S,node); walk_children(S,node); st_pop(S); block_break(S,10); return; }
    if (ieq(t,"tr"))    { block_break(S,4); st_push(S); apply_css(S,node); walk_children(S,node); st_pop(S); if(!S.at_sol)newline(S); return; }
    if (ieq(t,"td")||ieq(t,"th")) {
        st_push(S); if(ieq(t,"th"))S.stk[S.sp].bold=true;
        apply_css(S,node); walk_children(S,node); st_pop(S);
        S.pend_space=true; S.cx+=8; return;
    }

    /* ---- Footer (rendered but dimmed) --------------------------------- */
    if (ieq(t,"footer")) {
        block_break(S,8); st_push(S);
        S.stk[S.sp].color=COL_QUOTE;
        if (S.stk[S.sp].px>13) S.stk[S.sp].px--;
        apply_css(S,node); walk_children(S,node); st_pop(S);
        block_break(S,8); return;
    }

    /* ---- Paragraphs --------------------------------------------------- */
    if (ieq(t,"p")) {
        st_push(S); apply_css(S,node);
        int mt = S.stk[S.sp].margin_top >= 0 ? S.stk[S.sp].margin_top : 12;
        int mb = S.stk[S.sp].margin_bottom >= 0 ? S.stk[S.sp].margin_bottom : 12;
        block_break(S,mt); walk_children(S,node); block_break(S,mb); st_pop(S); return;
    }

    /* ---- Form: establish the action/method context for child controls -- */
    if (ieq(t,"form")) {
        char saved_action[256]; int saved_post = S.form_method_post;
        for (int i=0;i<256;++i) saved_action[i]=S.form_action[i];
        const char *act = dom_attr(node,"action");
        const char *mth = dom_attr(node,"method");
        str_copy(S.form_action, sizeof S.form_action, act ? act : "");
        S.form_method_post = (mth && (mth[0]=='p'||mth[0]=='P'));
        st_push(S); apply_css(S,node);
        int mt = S.stk[S.sp].margin_top >= 0 ? S.stk[S.sp].margin_top : 8;
        int mb = S.stk[S.sp].margin_bottom >= 0 ? S.stk[S.sp].margin_bottom : 8;
        block_break(S,mt);
        if (!S.stk[S.sp].hidden) walk_children(S,node);
        block_break(S,mb);
        st_pop(S);
        for (int i=0;i<256;++i) S.form_action[i]=saved_action[i];
        S.form_method_post = saved_post;
        return;
    }

    /* ---- Form controls: input / textarea / button / select ------------- *
     * Each emits a WL_FIELD run (a box carrying the cascaded style: color,
     * background, font-size and an optional CSS width) plus a wl_field entry
     * with the metadata needed to submit the owning form. */
    if (ieq(t,"input") || ieq(t,"textarea") || ieq(t,"button") || ieq(t,"select")) {
        st_push(S); apply_css(S,node);
        if (S.stk[S.sp].hidden) { st_pop(S); return; }

        const char *type = dom_attr(node,"type");
        const char *name = dom_attr(node,"name");
        const char *val  = dom_attr(node,"value");
        int px = S.stk[S.sp].px;

        /* Classify. */
        int kind = WLF_TEXT;
        int hidden_input = 0;
        if (ieq(t,"button")) kind = WLF_BUTTON;
        else if (ieq(t,"textarea")) kind = WLF_TEXTAREA;
        else if (ieq(t,"input") && type) {
            if (ieq(type,"submit")) kind = WLF_SUBMIT;
            else if (ieq(type,"button")) kind = WLF_BUTTON;
            else if (ieq(type,"hidden")) hidden_input = 1;
            else if (ieq(type,"checkbox")||ieq(type,"radio")) kind = WLF_BUTTON; /* drawn as small box */
        }

        int idx = add_field(S.doc);
        if (idx >= 0) {
            wl_field *f = &S.doc->fields[idx];
            f->kind = kind;
            str_copy(f->name,  sizeof f->name,  name ? name : "");
            str_copy(f->value, sizeof f->value, val ? val : "");
            str_copy(f->action,sizeof f->action,S.form_action);
            f->method_post = S.form_method_post;
        }

        /* Hidden inputs are submittable but invisible: no run. */
        if (hidden_input) { st_pop(S); return; }

        /* Box geometry: CSS width wins; else `size` chars; else a default. */
        int w_css = S.stk[S.sp].width;
        int chars = 20;
        { const char *sz = dom_attr(node,"size"); if (sz) { int v=0; for(const char*p=sz;*p>='0'&&*p<='9';++p)v=v*10+(*p-'0'); if(v>0)chars=v; } }
        int box_w, box_h;
        if (kind == WLF_SUBMIT || kind == WLF_BUTTON) {
            const char *label = (val && val[0]) ? val : "Submit";
            box_w = w_css > 0 ? w_css : word_w(S, label, (int)__builtin_strlen(label), px) + 24;
            box_h = px + 12;
        } else if (kind == WLF_TEXTAREA) {
            box_w = w_css > 0 ? w_css : 280;
            box_h = S.lh(px) * 3 + 8;
        } else { /* text-like input */
            box_w = w_css > 0 ? w_css : chars * S.adv('m', px);
            box_h = px + 10;
        }
        if (box_w > S.vw - S.stk[S.sp].left) box_w = S.vw - S.stk[S.sp].left;

        /* Place the box on the current line (wrap if needed). */
        int gap = (S.pend_space && !S.at_sol) ? S.adv(' ', px) : 0;
        if (!S.at_sol && S.cx + gap + box_w > S.vw) { newline(S); gap = 0; }
        else S.cx += gap;
        S.pend_space = false;

        wl_run *r = add_run(S.doc);
        if (r) {
            r->kind = WL_FIELD;
            r->x = S.cx; r->y = S.cy; r->w = box_w; r->h = box_h;
            r->px = px;
            r->bold = (kind==WLF_SUBMIT||kind==WLF_BUTTON) ? 1 : 0;
            r->underline = 0;
            r->color = S.stk[S.sp].color;
            r->bg = S.stk[S.sp].bg;
            r->off = idx; r->len = kind; r->link = -1;
            S.cx += box_w; S.at_sol = false;
            if (box_h > S.line_h) S.line_h = box_h;
            S.pending_margin = 0;
        }
        st_pop(S);
        return;
    }

    /* ---- Generic block containers ------------------------------------- */
    if (ieq(t,"div")    ||ieq(t,"section")||ieq(t,"article")||ieq(t,"header")||
        ieq(t,"main")   ||ieq(t,"aside")  ||ieq(t,"figure") ||ieq(t,"figcaption")||
        ieq(t,"fieldset")||ieq(t,"details")||ieq(t,"summary")||
        ieq(t,"nav")    ||ieq(t,"address") ||ieq(t,"center") ||
        ieq(t,"body")   ||ieq(t,"html")) {
        st_push(S); apply_css(S,node);
        if (!S.stk[S.sp].hidden) {
            int mt = S.stk[S.sp].margin_top >= 0 ? S.stk[S.sp].margin_top : 0;
            int mb = S.stk[S.sp].margin_bottom >= 0 ? S.stk[S.sp].margin_bottom : 0;
            int pt = S.stk[S.sp].padding_top >= 0 ? S.stk[S.sp].padding_top : 0;
            int pb = S.stk[S.sp].padding_bottom >= 0 ? S.stk[S.sp].padding_bottom : 0;
            int pl = S.stk[S.sp].padding_left >= 0 ? S.stk[S.sp].padding_left : 0;
            if (pl > 80) pl = 80;
            /* max-width: shrink effective viewport and optionally center */
            int mw = S.stk[S.sp].max_width;
            int saved_vw = S.vw;
            int orig_left = S.stk[S.sp].left;
            if (mw > 0) {
                int avail = S.vw - orig_left;
                if (mw < avail) {
                    /* center the constrained block if margin appears auto-like */
                    int indent = (avail - mw) / 2;
                    if (indent > 0) { S.stk[S.sp].left += indent; }
                    S.vw = S.stk[S.sp].left + mw;
                }
            }
            int bg_idx = -1;
            block_break(S, mt);
            if (S.stk[S.sp].bg) {
                bg_idx = begin_bg(S, S.stk[S.sp].bg);
                S.stk[S.sp].bg = 0;
            }
            if (pt > 0) S.cy += pt;
            if (pl > 0) { S.stk[S.sp].left += pl; if (S.at_sol) S.cx = S.stk[S.sp].left; }
            walk_children(S, node);
            if (!S.at_sol) newline(S);
            if (pb > 0) S.cy += pb;
            if (bg_idx >= 0) {
                end_bg(S, bg_idx);
                /* draw border around the background block */
                int bw = S.stk[S.sp].border_width;
                if (bw > 0 && bw <= 4) {
                    const wl_run &bg = S.doc->runs[bg_idx];
                    unsigned bc = S.stk[S.sp].border_color ? S.stk[S.sp].border_color : COL_RULE;
                    auto bline = [&](int x,int y,int w,int h){
                        wl_run *r=add_run(S.doc); if(!r)return;
                        r->kind=WL_RECT;r->x=x;r->y=y;r->w=w;r->h=h;r->color=bc;
                        r->px=r->bold=r->underline=r->italic=r->strikethrough=r->off=r->len=0;r->bg=0;r->link=-1;
                    };
                    bline(bg.x, bg.y, bg.w, bw);                    /* top */
                    bline(bg.x, bg.y+bg.h-bw, bg.w, bw);            /* bottom */
                    bline(bg.x, bg.y, bw, bg.h);                    /* left */
                    bline(bg.x+bg.w-bw, bg.y, bw, bg.h);            /* right */
                }
            }
            S.vw = saved_vw;
            block_break(S, mb);
        }
        st_pop(S); return;
    }

    /* ---- Fallback: unknown tags just pass through their children ------- */
    st_push(S); apply_css(S,node); walk_children(S,node); st_pop(S);
}

/* ---- Style-sheet collector -------------------------------------------- */

static int collect_styles(dom_node *node, char *out, int cap, int pos) {
    if (!node) return pos;
    for (dom_node *c = node->first_child; c; c = c->next_sibling) {
        if (c->type==DOM_ELEMENT && ieq(c->tag,"style")) {
            for (dom_node *tx=c->first_child; tx; tx=tx->next_sibling)
                if (tx->type==DOM_TEXT && tx->text) {
                    const char *s=tx->text;
                    while (*s && pos+1<cap) out[pos++]=*s++;
                    if (pos+1<cap) out[pos++]='\n';
                }
        }
        pos = collect_styles(c, out, cap, pos);
    }
    return pos;
}

/* ---- LayoutEngine public methods --------------------------------------- */

void LayoutEngine::set_metrics(int (*advance)(int, int), int (*line_height)(int)) {
    advance_fn_     = advance;
    line_height_fn_ = line_height;
}
void LayoutEngine::set_image_sizer(int (*fn)(const char *, int, int *, int *)) {
    image_sizer_fn_ = fn;
}

int LayoutEngine::layout(wl_doc *doc, dom_node *root, int viewport_w) {
    State S;
    S.doc    = doc;
    S.vw     = viewport_w > 64 ? viewport_w : 64;
    S.adv_fn = advance_fn_;
    S.lh_fn  = line_height_fn_;
    S.img_fn = image_sizer_fn_;

    doc->pool_len = doc->run_count = doc->href_count = doc->height = 0;

    /* Collect and parse <style> sheets. */
    char *css_buf = static_cast<char *>(umalloc(64 * 1024));
    if (css_buf) {
        int css_len = collect_styles(root, css_buf, 64*1024, 0);
        css_buf[css_len < 64*1024 ? css_len : 64*1024-1] = '\0';
        if (css_len > 0) S.sheet = css_parse(css_buf, css_len);
        ufree(css_buf);
    }

    if (root) walk_children(S, root);
    if (!S.at_sol) S.cy += S.line_h > 0 ? S.line_h : S.lh(BODY_PX);
    doc->height = S.cy;

    if (S.sheet) css_free(S.sheet);
    return S.cy;
}

void LayoutEngine::extract_title(dom_node *root, char *out, int cap) {
    if (!out || cap <= 0 || !root) { if (out && cap > 0) out[0]='\0'; return; }
    out[0] = '\0';
    /* BFS through the DOM. */
    struct F { dom_node *n; };
    static F stack[64]; int top = 0;
    stack[top++] = {root};
    while (top > 0) {
        dom_node *n = stack[--top].n;
        while (n) {
            if (n->type == DOM_ELEMENT) {
                if (ieq(n->tag,"title")) {
                    int pos = 0;
                    for (dom_node *tx=n->first_child; tx; tx=tx->next_sibling)
                        if (tx->type==DOM_TEXT && tx->text)
                            { const char *s=tx->text; while(*s&&pos+1<cap) out[pos++]=*s++; }
                    out[pos]='\0'; return;
                }
                if (n->first_child && top < 63) stack[top++] = {n->first_child};
            }
            n = n->next_sibling;
        }
    }
}
