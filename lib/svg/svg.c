/* libsvg implementation — see svg.h.
 *
 * Geometry is flattened into polylines and rendered into an offscreen
 * straight-alpha RGBA layer, so fills, linear gradients, per-element and
 * group opacity, and group filters (e.g. a Gaussian glow) all composite
 * correctly. The caller's output buffer is used directly as the base layer.
 *
 * Filled paths use a scanline rasterizer (non-zero winding) with vertical
 * supersampling plus analytic horizontal coverage. Strokes use a signed
 * distance field over the same polylines, giving crisp round caps/joins. */

#include "svg.h"

typedef unsigned int  uint32_t;
typedef unsigned char uint8_t;

/* ---- tiny XML/number scanning helpers ---- */
static int svg_is_space(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == ',';
}

static int svg_match_name(const char *p, const char *name) {
    int i = 0;
    while (name[i]) {
        if (p[i] != name[i]) return 0;
        ++i;
    }
    return 1;
}

static const char *svg_tag_end(const char *tag) {
    while (*tag && *tag != '>') ++tag;
    return tag;
}

/* Parse a (possibly signed, fractional, exponent) decimal number. */
static int svg_parse_f(const char **pp, float *out) {
    const char *p = *pp;
    float sign = 1.0f, val = 0.0f, frac = 0.0f, div = 1.0f;
    int seen = 0;
    while (svg_is_space(*p)) ++p;
    if (*p == '-') { sign = -1.0f; ++p; }
    else if (*p == '+') { ++p; }
    while (*p >= '0' && *p <= '9') { val = val * 10.0f + (*p - '0'); ++p; seen = 1; }
    if (*p == '.') {
        ++p;
        while (*p >= '0' && *p <= '9') { frac = frac * 10.0f + (*p - '0'); div *= 10.0f; ++p; seen = 1; }
        val += frac / div;
    }
    if (*p == 'e' || *p == 'E') {                 /* scientific notation */
        const char *q = p + 1;
        int es = 1, e = 0;
        if (*q == '-') { es = -1; ++q; } else if (*q == '+') ++q;
        if (*q >= '0' && *q <= '9') {
            float m = 1.0f;
            while (*q >= '0' && *q <= '9') { e = e * 10 + (*q - '0'); ++q; }
            p = q;
            while (e--) m *= 10.0f;
            if (es < 0) val /= m; else val *= m;
        }
    }
    if (!seen) return 0;
    *out = sign * val;
    *pp = p;
    return 1;
}

/* Return a pointer just past `name="` for an attribute on `tag`, matching only
 * on whole-word attribute boundaries (so "x" never matches inside "rx"). The
 * value's closing-quote position is reported in *vend. */
static const char *svg_attr_val(const char *tag, const char *name, const char **vend) {
    const char *end = svg_tag_end(tag);
    const char *p = tag;
    int nl = 0;
    while (name[nl]) ++nl;
    while (p < end) {
        if ((p == tag || svg_is_space(p[-1])) && svg_match_name(p, name)) {
            const char *q = p + nl;
            while (q < end && svg_is_space(*q)) ++q;
            if (q < end && *q == '=') {
                ++q;
                while (q < end && svg_is_space(*q)) ++q;
                if (q < end && *q == '"') {
                    ++q;
                    if (vend) { const char *e = q; while (e < end && *e != '"') ++e; *vend = e; }
                    return q;
                }
            }
        }
        ++p;
    }
    if (vend) *vend = 0;
    return 0;
}

static float svg_attr_f(const char *tag, const char *name, float fallback) {
    const char *v = svg_attr_val(tag, name, 0);
    float out;
    if (v && svg_parse_f(&v, &out)) return out;
    return fallback;
}

static const char *svg_attr_text(const char *tag, const char *name, const char **end_out) {
    return svg_attr_val(tag, name, end_out);
}

/* Copy an attribute string value into a fixed buffer (truncating safely). */
static void svg_copy_attr(const char *tag, const char *name, char *dst, int cap) {
    const char *vend = 0;
    const char *v = svg_attr_val(tag, name, &vend);
    int i = 0;
    if (v && vend) for (; v < vend && i < cap - 1; ++v) dst[i++] = *v;
    dst[i] = 0;
}

/* ======================================================================
 *  Renderer
 * ====================================================================== */

#define SVG_LAYER_PX    (SVG_MAX_DIM * SVG_MAX_DIM)
#define SVG_MAX_PTS     2048                       /* flattened vertices      */
#define SVG_MAX_SUBPATH 64
#define SVG_MAX_GRAD    8
#define SVG_MAX_STOPS   8
#define SVG_MAX_FILTER  4
#define SVG_MAX_FE      8
#define SVG_FILL_SS     4                          /* vertical supersamples   */
#define SVG_CURVE_STEPS 24

/* --- small float helpers (SSE; no libc dependency) --- */
static inline float svg_sqrtf(float x){ float r; __asm__("sqrtss %1,%0":"=x"(r):"x"(x)); return r; }
static inline float svg_clampf(float v,float lo,float hi){ return v<lo?lo:(v>hi?hi:v); }

typedef struct { float r, g, b, a; } svg_rgba;     /* straight alpha, 0..1 */

typedef struct {
    int   used; char id[32];
    float x1, y1, x2, y2;                          /* axis, user space */
    int   nstops; float off[SVG_MAX_STOPS]; svg_rgba col[SVG_MAX_STOPS];
} svg_gradient;

enum { SVG_PAINT_NONE = 0, SVG_PAINT_SOLID, SVG_PAINT_GRAD };
typedef struct { int kind; svg_rgba color; int grad; } svg_paint;

enum { FE_BLUR = 1, FE_COLORMATRIX, FE_BLEND };
typedef struct { int type; float stddev; float m[20]; int in_source; } svg_fe;
typedef struct { int used; char id[32]; int n; svg_fe fe[SVG_MAX_FE]; } svg_filter;

/* Offscreen layers, straight-alpha 0xAARRGGBB. The base layer is the caller's
 * output buffer; group/tmp layers are library-owned scratch. */
static uint32_t *g_svg_base;
static uint32_t g_svg_group[SVG_LAYER_PX];
static uint32_t g_svg_tmpA[SVG_LAYER_PX];
static uint32_t g_svg_tmpB[SVG_LAYER_PX];
static uint32_t *g_svg_target;                     /* current draw layer */

static int   g_svg_size;
static float g_svg_scale, g_svg_ox, g_svg_oy;      /* user->pixel: p = u*s + o */

static svg_gradient g_grads[SVG_MAX_GRAD];
static int g_grad_n;
static svg_filter   g_filters[SVG_MAX_FILTER];
static int g_filter_n;

/* Flattened geometry of the current element, in pixel space. */
static float g_pts_x[SVG_MAX_PTS];
static float g_pts_y[SVG_MAX_PTS];
static int   g_pt_n;
static struct { int start, count, closed; } g_sub[SVG_MAX_SUBPATH];
static int   g_sub_n;

/* Scratch for the scanline fill. */
static float g_svg_cov[SVG_MAX_DIM];
static float g_svg_cx[SVG_MAX_PTS];
static int   g_svg_cd[SVG_MAX_PTS];

/* ---- pixel pack / unpack ---- */
static svg_rgba svg_unpack(uint32_t p) {
    svg_rgba c;
    c.a = ((p >> 24) & 255) / 255.0f;
    c.r = ((p >> 16) & 255) / 255.0f;
    c.g = ((p >> 8) & 255) / 255.0f;
    c.b = (p & 255) / 255.0f;
    return c;
}
static uint32_t svg_pack(svg_rgba c) {
    int a = (int)(svg_clampf(c.a, 0, 1) * 255.0f + 0.5f);
    int r = (int)(svg_clampf(c.r, 0, 1) * 255.0f + 0.5f);
    int g = (int)(svg_clampf(c.g, 0, 1) * 255.0f + 0.5f);
    int b = (int)(svg_clampf(c.b, 0, 1) * 255.0f + 0.5f);
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/* Composite straight-alpha `s` over straight-alpha `d` ("source over"). */
static svg_rgba svg_over(svg_rgba s, svg_rgba d) {
    svg_rgba o;
    float ia = 1.0f - s.a;
    o.a = s.a + d.a * ia;
    if (o.a <= 0.0001f) { o.r = o.g = o.b = o.a = 0; return o; }
    o.r = (s.r * s.a + d.r * d.a * ia) / o.a;
    o.g = (s.g * s.a + d.g * d.a * ia) / o.a;
    o.b = (s.b * s.a + d.b * d.a * ia) / o.a;
    return o;
}

/* Blend a straight-alpha color over one pixel of a layer. */
static void svg_blend(uint32_t *L, int x, int y, svg_rgba c) {
    int i;
    if (c.a <= 0.0f || x < 0 || y < 0 || x >= g_svg_size || y >= g_svg_size) return;
    i = y * g_svg_size + x;
    L[i] = svg_pack(svg_over(c, svg_unpack(L[i])));
}

static void svg_layer_clear(uint32_t *L) {
    int i, n = g_svg_size * g_svg_size;
    for (i = 0; i < n; ++i) L[i] = 0;
}
static void svg_layer_copy(uint32_t *d, const uint32_t *s) {
    int i, n = g_svg_size * g_svg_size;
    for (i = 0; i < n; ++i) d[i] = s[i];
}

/* ---- color parsing ---- */
static int svg_hex_nib(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}
static svg_rgba svg_hex_color(const char *p) {       /* p points at '#' */
    svg_rgba c = {0, 0, 0, 1.0f};
    int len = 0;
    const char *q = p + 1;
    while ((q[len] >= '0' && q[len] <= '9') ||
           (q[len] >= 'a' && q[len] <= 'f') ||
           (q[len] >= 'A' && q[len] <= 'F')) ++len;
    if (len >= 6) {
        c.r = (svg_hex_nib(q[0]) * 16 + svg_hex_nib(q[1])) / 255.0f;
        c.g = (svg_hex_nib(q[2]) * 16 + svg_hex_nib(q[3])) / 255.0f;
        c.b = (svg_hex_nib(q[4]) * 16 + svg_hex_nib(q[5])) / 255.0f;
    } else if (len >= 3) {                            /* #rgb shorthand */
        c.r = (svg_hex_nib(q[0]) * 17) / 255.0f;
        c.g = (svg_hex_nib(q[1]) * 17) / 255.0f;
        c.b = (svg_hex_nib(q[2]) * 17) / 255.0f;
    }
    return c;
}

static int svg_id_eq(const char *s, const char *id) {  /* s ends at ')' or '"' */
    int i = 0;
    while (id[i]) {
        if (s[i] != id[i]) return 0;
        ++i;
    }
    return s[i] == ')' || s[i] == '"' || s[i] == 0;
}
static int svg_find_gradient(const char *idref) {       /* idref after "url(#" */
    int i;
    for (i = 0; i < g_grad_n; ++i)
        if (g_grads[i].used && svg_id_eq(idref, g_grads[i].id)) return i;
    return -1;
}

/* Resolve a fill/stroke attribute into a paint. */
static void svg_parse_paint(const char *tag, const char *name,
                            svg_paint *out, svg_rgba current) {
    const char *v = svg_attr_val(tag, name, 0);
    out->kind = SVG_PAINT_NONE;
    if (!v) return;
    if (svg_match_name(v, "none")) return;
    if (svg_match_name(v, "url(#")) {
        int g = svg_find_gradient(v + 5);
        if (g >= 0) { out->kind = SVG_PAINT_GRAD; out->grad = g; }
        return;
    }
    if (svg_match_name(v, "currentColor")) { out->kind = SVG_PAINT_SOLID; out->color = current; return; }
    if (*v == '#') { out->kind = SVG_PAINT_SOLID; out->color = svg_hex_color(v); }
}

/* Evaluate a paint's color at pixel (px,py). */
static svg_rgba svg_paint_at(const svg_paint *pt, float px, float py) {
    svg_rgba z = {0, 0, 0, 0};
    if (pt->kind == SVG_PAINT_SOLID) return pt->color;
    if (pt->kind == SVG_PAINT_GRAD) {
        const svg_gradient *g = &g_grads[pt->grad];
        float gx1 = g->x1 * g_svg_scale + g_svg_ox, gy1 = g->y1 * g_svg_scale + g_svg_oy;
        float gx2 = g->x2 * g_svg_scale + g_svg_ox, gy2 = g->y2 * g_svg_scale + g_svg_oy;
        float dx = gx2 - gx1, dy = gy2 - gy1, len2 = dx * dx + dy * dy;
        float t = len2 > 0 ? ((px - gx1) * dx + (py - gy1) * dy) / len2 : 0.0f;
        int i;
        t = svg_clampf(t, 0, 1);
        if (g->nstops == 0) return z;
        if (t <= g->off[0]) return g->col[0];
        if (t >= g->off[g->nstops - 1]) return g->col[g->nstops - 1];
        for (i = 1; i < g->nstops; ++i) {
            if (t <= g->off[i]) {
                float span = g->off[i] - g->off[i - 1];
                float f = span > 0 ? (t - g->off[i - 1]) / span : 0.0f;
                svg_rgba a = g->col[i - 1], b = g->col[i], o;
                o.r = a.r + (b.r - a.r) * f;
                o.g = a.g + (b.g - a.g) * f;
                o.b = a.b + (b.b - a.b) * f;
                o.a = a.a + (b.a - a.a) * f;
                return o;
            }
        }
    }
    return z;
}

/* ---- geometry flattening (pixel space) ---- */
static void svg_geo_reset(void) { g_pt_n = 0; g_sub_n = 0; }

static void svg_geo_moveto(float ux, float uy) {
    if (g_sub_n >= SVG_MAX_SUBPATH || g_pt_n >= SVG_MAX_PTS) return;
    g_sub[g_sub_n].start = g_pt_n;
    g_sub[g_sub_n].count = 1;
    g_sub[g_sub_n].closed = 0;
    g_pts_x[g_pt_n] = ux * g_svg_scale + g_svg_ox;
    g_pts_y[g_pt_n] = uy * g_svg_scale + g_svg_oy;
    ++g_pt_n; ++g_sub_n;
}
static void svg_geo_lineto(float ux, float uy) {
    if (g_sub_n == 0) { svg_geo_moveto(ux, uy); return; }
    if (g_pt_n >= SVG_MAX_PTS) return;
    g_pts_x[g_pt_n] = ux * g_svg_scale + g_svg_ox;
    g_pts_y[g_pt_n] = uy * g_svg_scale + g_svg_oy;
    ++g_pt_n; g_sub[g_sub_n - 1].count++;
}
static void svg_geo_close(void) { if (g_sub_n) g_sub[g_sub_n - 1].closed = 1; }

/* Flatten a cubic Bezier; (x0,y0) is the current point (already emitted). */
static void svg_geo_curve(float x0, float y0, float x1, float y1,
                          float x2, float y2, float x3, float y3) {
    int i;
    for (i = 1; i <= SVG_CURVE_STEPS; ++i) {
        float t = (float)i / SVG_CURVE_STEPS, u = 1.0f - t;
        float a = u*u*u, b = 3*u*u*t, c = 3*u*t*t, d = t*t*t;
        svg_geo_lineto(a*x0 + b*x1 + c*x2 + d*x3, a*y0 + b*y1 + c*y2 + d*y3);
    }
}

static void svg_geo_rect(float x, float y, float w, float h) {
    svg_geo_moveto(x, y);
    svg_geo_lineto(x + w, y);
    svg_geo_lineto(x + w, y + h);
    svg_geo_lineto(x, y + h);
    svg_geo_close();
}

/* A circle as four cubic arcs (kappa), reusing the Bezier flattener. */
static void svg_geo_circle(float cx, float cy, float r) {
    float k = 0.5522847498f * r;
    svg_geo_moveto(cx + r, cy);
    svg_geo_curve(cx + r, cy,      cx + r, cy + k,  cx + k, cy + r,  cx,     cy + r);
    svg_geo_curve(cx,     cy + r,  cx - k, cy + r,  cx - r, cy + k,  cx - r, cy);
    svg_geo_curve(cx - r, cy,      cx - r, cy - k,  cx - k, cy - r,  cx,     cy - r);
    svg_geo_curve(cx,     cy - r,  cx + k, cy - r,  cx + r, cy - k,  cx + r, cy);
    svg_geo_close();
}

static int svg_path_cmd(char ch) {
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
}

/* Parse a path "d" string (user coords) into flattened subpaths. */
static void svg_parse_path(const char *p, const char *end) {
    char cmd = 0;
    float cx = 0, cy = 0, sx = 0, sy = 0;
    while (p && p < end && *p) {
        float x, y, x1, y1, x2, y2;
        while (p < end && svg_is_space(*p)) ++p;
        if (p >= end || *p == '"') break;
        if (svg_path_cmd(*p)) {
            cmd = *p++;
            if (cmd == 'Z' || cmd == 'z') { svg_geo_close(); cx = sx; cy = sy; continue; }
        }
        if (cmd == 'M' || cmd == 'm') {
            if (!svg_parse_f(&p, &x) || !svg_parse_f(&p, &y)) break;
            if (cmd == 'm') { x += cx; y += cy; }
            svg_geo_moveto(x, y);
            cx = sx = x; cy = sy = y;
            cmd = (cmd == 'm') ? 'l' : 'L';
        } else if (cmd == 'L' || cmd == 'l') {
            if (!svg_parse_f(&p, &x) || !svg_parse_f(&p, &y)) break;
            if (cmd == 'l') { x += cx; y += cy; }
            svg_geo_lineto(x, y); cx = x; cy = y;
        } else if (cmd == 'H' || cmd == 'h') {
            if (!svg_parse_f(&p, &x)) break;
            if (cmd == 'h') x += cx;
            svg_geo_lineto(x, cy); cx = x;
        } else if (cmd == 'V' || cmd == 'v') {
            if (!svg_parse_f(&p, &y)) break;
            if (cmd == 'v') y += cy;
            svg_geo_lineto(cx, y); cy = y;
        } else if (cmd == 'C' || cmd == 'c') {
            if (!svg_parse_f(&p, &x1) || !svg_parse_f(&p, &y1) ||
                !svg_parse_f(&p, &x2) || !svg_parse_f(&p, &y2) ||
                !svg_parse_f(&p, &x)  || !svg_parse_f(&p, &y)) break;
            if (cmd == 'c') { x1 += cx; y1 += cy; x2 += cx; y2 += cy; x += cx; y += cy; }
            svg_geo_curve(cx, cy, x1, y1, x2, y2, x, y); cx = x; cy = y;
        } else break;
    }
}

/* ---- fill (scanline, non-zero winding) ---- */
static void svg_add_span(float xa, float xb, float w) {
    int x, ix0, ix1;
    if (xb < xa) { float t = xa; xa = xb; xb = t; }
    if (xb <= 0 || xa >= g_svg_size) return;
    if (xa < 0) xa = 0;
    if (xb > g_svg_size) xb = g_svg_size;
    ix0 = (int)xa; ix1 = (int)xb;
    for (x = ix0; x <= ix1 && x < g_svg_size; ++x) {
        float l = xa > x ? xa : (float)x;
        float r = xb < (x + 1) ? xb : (float)(x + 1);
        if (r > l) g_svg_cov[x] += (r - l) * w;
    }
}

static void svg_fill(const svg_paint *pt, float opacity) {
    int py, px, ss, s, k, i;
    float w = 1.0f / SVG_FILL_SS;
    for (py = 0; py < g_svg_size; ++py) {
        for (px = 0; px < g_svg_size; ++px) g_svg_cov[px] = 0.0f;
        for (ss = 0; ss < SVG_FILL_SS; ++ss) {
            float yc = py + (ss + 0.5f) / SVG_FILL_SS;
            int nx = 0, wind = 0;
            for (s = 0; s < g_sub_n; ++s) {
                int st = g_sub[s].start, cnt = g_sub[s].count;
                if (cnt < 2) continue;
                for (k = 0; k < cnt; ++k) {           /* implicitly closed */
                    int a = st + k, b = st + ((k + 1) % cnt);
                    float ya = g_pts_y[a], yb = g_pts_y[b], t, xc;
                    int dir;
                    if (ya == yb) continue;
                    if (ya < yb) { if (yc < ya || yc >= yb) continue; dir = 1; }
                    else         { if (yc < yb || yc >= ya) continue; dir = -1; }
                    t = (yc - ya) / (yb - ya);
                    xc = g_pts_x[a] + (g_pts_x[b] - g_pts_x[a]) * t;
                    if (nx < SVG_MAX_PTS) { g_svg_cx[nx] = xc; g_svg_cd[nx] = dir; ++nx; }
                }
            }
            for (i = 1; i < nx; ++i) {                /* insertion sort by x */
                float kx = g_svg_cx[i]; int kd = g_svg_cd[i]; int j = i - 1;
                while (j >= 0 && g_svg_cx[j] > kx) {
                    g_svg_cx[j + 1] = g_svg_cx[j]; g_svg_cd[j + 1] = g_svg_cd[j]; --j;
                }
                g_svg_cx[j + 1] = kx; g_svg_cd[j + 1] = kd;
            }
            for (i = 0; i < nx; ++i) {
                wind += g_svg_cd[i];
                if (wind != 0 && i + 1 < nx) svg_add_span(g_svg_cx[i], g_svg_cx[i + 1], w);
            }
        }
        for (px = 0; px < g_svg_size; ++px) {
            float a = g_svg_cov[px];
            svg_rgba c;
            if (a <= 0.0f) continue;
            if (a > 1.0f) a = 1.0f;
            c = svg_paint_at(pt, px + 0.5f, py + 0.5f);
            c.a *= opacity * a;
            svg_blend(g_svg_target, px, py, c);
        }
    }
}

/* ---- stroke (signed distance field) ---- */
static float svg_seg_dist(float px, float py, float ax, float ay, float bx, float by) {
    float dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy, t, ex, ey;
    if (len2 <= 0) { ex = px - ax; ey = py - ay; return svg_sqrtf(ex*ex + ey*ey); }
    t = ((px - ax) * dx + (py - ay) * dy) / len2;
    t = svg_clampf(t, 0, 1);
    ex = px - (ax + dx * t); ey = py - (ay + dy * t);
    return svg_sqrtf(ex*ex + ey*ey);
}

static void svg_stroke(const svg_paint *pt, float halfw, float opacity) {
    int px, py, s, k;
    for (py = 0; py < g_svg_size; ++py) {
        for (px = 0; px < g_svg_size; ++px) {
            float pxc = px + 0.5f, pyc = py + 0.5f, best = 1e30f, a;
            svg_rgba c;
            for (s = 0; s < g_sub_n; ++s) {
                int st = g_sub[s].start, cnt = g_sub[s].count;
                int edges = g_sub[s].closed ? cnt : cnt - 1;
                for (k = 0; k < edges; ++k) {
                    int ia = st + k, ib = st + ((k + 1) % cnt);
                    float d = svg_seg_dist(pxc, pyc, g_pts_x[ia], g_pts_y[ia],
                                           g_pts_x[ib], g_pts_y[ib]);
                    if (d < best) best = d;
                }
            }
            a = svg_clampf(halfw + 0.5f - best, 0, 1);   /* 1px analytic AA */
            if (a <= 0.0f) continue;
            c = svg_paint_at(pt, pxc, pyc);
            c.a *= opacity * a;
            svg_blend(g_svg_target, px, py, c);
        }
    }
}

/* ---- filters ---- */
static void svg_blur_axis(const uint32_t *src, uint32_t *dst, int r, int horizontal) {
    int x, y, k, i, n = g_svg_size;
    for (y = 0; y < n; ++y) {
        for (x = 0; x < n; ++x) {
            float ar = 0, ag = 0, ab = 0, aa = 0; int cnt = 0;
            for (k = -r; k <= r; ++k) {
                int sx = horizontal ? x + k : x;
                int sy = horizontal ? y : y + k;
                svg_rgba c;
                if (sx < 0 || sy < 0 || sx >= n || sy >= n) continue;
                c = svg_unpack(src[sy * n + sx]);
                ar += c.r; ag += c.g; ab += c.b; aa += c.a; ++cnt;
            }
            i = y * n + x;
            if (cnt > 0) {
                svg_rgba o; o.r = ar/cnt; o.g = ag/cnt; o.b = ab/cnt; o.a = aa/cnt;
                dst[i] = svg_pack(o);
            } else dst[i] = 0;
        }
    }
}
/* Three box passes approximate a Gaussian; result ends up in `inout`. */
static void svg_gaussian(uint32_t *inout, float sigma_px) {
    int r = (int)(sigma_px + 0.5f), pass;
    if (r < 1) return;
    for (pass = 0; pass < 3; ++pass) {
        svg_blur_axis(inout, g_svg_tmpA, r, 1);       /* horizontal */
        svg_blur_axis(g_svg_tmpA, inout, r, 0);       /* vertical   */
    }
}
static void svg_colormatrix(uint32_t *L, const float *m) {
    int i, n = g_svg_size * g_svg_size;
    for (i = 0; i < n; ++i) {
        svg_rgba c = svg_unpack(L[i]), o;
        o.r = m[0]*c.r  + m[1]*c.g  + m[2]*c.b  + m[3]*c.a  + m[4];
        o.g = m[5]*c.r  + m[6]*c.g  + m[7]*c.b  + m[8]*c.a  + m[9];
        o.b = m[10]*c.r + m[11]*c.g + m[12]*c.b + m[13]*c.a + m[14];
        o.a = m[15]*c.r + m[16]*c.g + m[17]*c.b + m[18]*c.a + m[19];
        L[i] = svg_pack(o);
    }
}

/* Run a filter on the group layer (SourceGraphic); result lands in tmpB. */
static void svg_apply_filter(const svg_filter *f) {
    int i, j, n = g_svg_size * g_svg_size;
    svg_layer_copy(g_svg_tmpB, g_svg_group);          /* cur := SourceGraphic */
    for (i = 0; i < f->n; ++i) {
        const svg_fe *fe = &f->fe[i];
        if (fe->type == FE_BLUR) {
            svg_gaussian(g_svg_tmpB, fe->stddev * g_svg_scale);
        } else if (fe->type == FE_COLORMATRIX) {
            svg_colormatrix(g_svg_tmpB, fe->m);
        } else if (fe->type == FE_BLEND) {
            /* in = SourceGraphic over the previous result (normal mode) */
            for (j = 0; j < n; ++j)
                g_svg_tmpB[j] = svg_pack(svg_over(svg_unpack(g_svg_group[j]),
                                                  svg_unpack(g_svg_tmpB[j])));
        }
    }
}

/* Composite a finished layer onto the base, scaled by group opacity. */
static void svg_composite_onto_base(const uint32_t *L, float opacity) {
    int x, y;
    for (y = 0; y < g_svg_size; ++y)
        for (x = 0; x < g_svg_size; ++x) {
            svg_rgba c = svg_unpack(L[y * g_svg_size + x]);
            c.a *= opacity;
            svg_blend(g_svg_base, x, y, c);
        }
}

/* ---- defs ---- */
static void svg_parse_defs(const char *svg) {
    const char *p;
    g_grad_n = 0; g_filter_n = 0;
    for (p = svg; *p; ++p) {
        if (p[0] == '<' && svg_match_name(p + 1, "linearGradient") && g_grad_n < SVG_MAX_GRAD) {
            svg_gradient *g = &g_grads[g_grad_n++];
            const char *q;
            g->used = 1;
            svg_copy_attr(p, "id", g->id, sizeof(g->id));
            g->x1 = svg_attr_f(p, "x1", 0); g->y1 = svg_attr_f(p, "y1", 0);
            g->x2 = svg_attr_f(p, "x2", 1); g->y2 = svg_attr_f(p, "y2", 0);
            g->nstops = 0;
            for (q = svg_tag_end(p); *q; ++q) {
                if (q[0] == '<' && q[1] == '/' && svg_match_name(q + 2, "linearGradient")) break;
                if (q[0] == '<' && svg_match_name(q + 1, "stop") && g->nstops < SVG_MAX_STOPS) {
                    const char *cv = svg_attr_val(q, "stop-color", 0);
                    svg_rgba col = {0, 0, 0, 1.0f};
                    if (cv && *cv == '#') col = svg_hex_color(cv);
                    col.a *= svg_attr_f(q, "stop-opacity", 1.0f);
                    g->off[g->nstops] = svg_clampf(svg_attr_f(q, "offset", 0), 0, 1);
                    g->col[g->nstops] = col;
                    ++g->nstops;
                }
            }
        } else if (p[0] == '<' && svg_match_name(p + 1, "filter") && g_filter_n < SVG_MAX_FILTER) {
            svg_filter *f = &g_filters[g_filter_n++];
            const char *q;
            f->used = 1; f->n = 0;
            svg_copy_attr(p, "id", f->id, sizeof(f->id));
            for (q = svg_tag_end(p); *q; ++q) {
                if (q[0] == '<' && q[1] == '/' && svg_match_name(q + 2, "filter")) break;
                if (q[0] != '<' || f->n >= SVG_MAX_FE) continue;
                if (svg_match_name(q + 1, "feGaussianBlur")) {
                    f->fe[f->n].type = FE_BLUR;
                    f->fe[f->n].stddev = svg_attr_f(q, "stdDeviation", 0);
                    ++f->n;
                } else if (svg_match_name(q + 1, "feColorMatrix")) {
                    const char *v = svg_attr_val(q, "values", 0);
                    int i;
                    f->fe[f->n].type = FE_COLORMATRIX;
                    for (i = 0; i < 20; ++i) {
                        float val = 0;
                        if (v) svg_parse_f(&v, &val);
                        f->fe[f->n].m[i] = val;
                    }
                    ++f->n;
                } else if (svg_match_name(q + 1, "feBlend")) {
                    const char *in = svg_attr_val(q, "in", 0);
                    f->fe[f->n].type = FE_BLEND;
                    f->fe[f->n].in_source = in && svg_match_name(in, "SourceGraphic");
                    ++f->n;
                }
            }
        }
    }
}
static int svg_find_filter(const char *tag) {
    const char *v = svg_attr_val(tag, "filter", 0);
    int i;
    if (!v || !svg_match_name(v, "url(#")) return -1;
    for (i = 0; i < g_filter_n; ++i)
        if (g_filters[i].used && svg_id_eq(v + 5, g_filters[i].id)) return i;
    return -1;
}

/* ---- element rendering ---- */
static void svg_draw_element(const char *tag, char kind, svg_rgba current) {
    svg_paint fill, stroke;
    float opacity = svg_attr_f(tag, "opacity", 1.0f);
    float fop = svg_attr_f(tag, "fill-opacity", 1.0f);
    float sop = svg_attr_f(tag, "stroke-opacity", 1.0f);
    float sw = svg_attr_f(tag, "stroke-width", 1.0f);

    svg_parse_paint(tag, "fill", &fill, current);
    svg_parse_paint(tag, "stroke", &stroke, current);

    svg_geo_reset();
    if (kind == 'p') {
        const char *d_end = 0;
        const char *d = svg_attr_text(tag, "d", &d_end);
        if (d && d_end) svg_parse_path(d, d_end);
    } else if (kind == 'r') {
        svg_geo_rect(svg_attr_f(tag, "x", 0), svg_attr_f(tag, "y", 0),
                     svg_attr_f(tag, "width", 0), svg_attr_f(tag, "height", 0));
    } else if (kind == 'c') {
        svg_geo_circle(svg_attr_f(tag, "cx", 0), svg_attr_f(tag, "cy", 0),
                       svg_attr_f(tag, "r", 0));
    }

    if (fill.kind != SVG_PAINT_NONE) svg_fill(&fill, opacity * fop);
    if (stroke.kind != SVG_PAINT_NONE)
        svg_stroke(&stroke, sw * g_svg_scale * 0.5f, opacity * sop);
}

/* ---- main walk ---- */
static void svg_draw_pass(const char *svg, svg_rgba current) {
    const char *p = svg;
    int depth = 0, gactive = 0, gdepth = 0, gfilter = -1;
    float gop = 1.0f;
    g_svg_target = g_svg_base;
    while (*p) {
        if (p[0] == '<') {
            if (p[1] == '/' && svg_match_name(p + 2, "g") &&
                (p[3] == '>' || svg_is_space(p[3]))) {
                if (gactive && depth == gdepth) {
                    if (gfilter >= 0) {
                        svg_apply_filter(&g_filters[gfilter]);
                        svg_composite_onto_base(g_svg_tmpB, gop);
                    } else {
                        svg_composite_onto_base(g_svg_group, gop);
                    }
                    gactive = 0; g_svg_target = g_svg_base;
                }
                --depth;
            } else if (svg_match_name(p + 1, "g") &&
                       (p[2] == '>' || svg_is_space(p[2]))) {
                ++depth;
                if (!gactive) {
                    float op = svg_attr_f(p, "opacity", 1.0f);
                    int fi = svg_find_filter(p);
                    if (fi >= 0 || op < 1.0f) {
                        gactive = 1; gdepth = depth; gfilter = fi; gop = op;
                        svg_layer_clear(g_svg_group);
                        g_svg_target = g_svg_group;
                    }
                }
            } else if (svg_match_name(p + 1, "path")) {
                svg_draw_element(p, 'p', current);
            } else if (svg_match_name(p + 1, "rect")) {
                svg_draw_element(p, 'r', current);
            } else if (svg_match_name(p + 1, "circle")) {
                svg_draw_element(p, 'c', current);
            }
        }
        ++p;
    }
}

/* Set up the user->pixel transform from the root viewBox (uniform fit, with
 * a small inset so strokes/glow never clip at the icon edge). */
static void svg_setup_transform(const char *svg, int size) {
    const char *vb = 0;
    float minx = 0, miny = 0, vw = (float)size, vh = (float)size;
    const char *s = svg;
    while (*s) {                                       /* find the <svg> tag */
        if (s[0] == '<' && svg_match_name(s + 1, "svg")) { vb = s; break; }
        ++s;
    }
    if (vb) {
        const char *v = svg_attr_val(vb, "viewBox", 0);
        if (v) { svg_parse_f(&v, &minx); svg_parse_f(&v, &miny);
                 svg_parse_f(&v, &vw);   svg_parse_f(&v, &vh); }
    }
    if (vw <= 0) vw = (float)size;
    if (vh <= 0) vh = (float)size;
    {
        float pad = size * 0.0625f;                    /* ~6% edge inset */
        float avail = size - 2 * pad;
        float sx = avail / vw, sy = avail / vh;
        g_svg_scale = sx < sy ? sx : sy;
        g_svg_ox = pad + (avail - vw * g_svg_scale) * 0.5f - minx * g_svg_scale;
        g_svg_oy = pad + (avail - vh * g_svg_scale) * 0.5f - miny * g_svg_scale;
    }
}

void svg_render_rgba(const char *svg, unsigned int *out, int size,
                     unsigned int current_color) {
    svg_rgba current;
    if (!svg || !out || size <= 0) return;
    if (size > SVG_MAX_DIM) size = SVG_MAX_DIM;
    g_svg_size = size;
    g_svg_base = (uint32_t *)out;
    current.r = ((current_color >> 16) & 255) / 255.0f;
    current.g = ((current_color >> 8) & 255) / 255.0f;
    current.b = (current_color & 255) / 255.0f;
    current.a = 1.0f;

    svg_setup_transform(svg, size);
    svg_parse_defs(svg);
    svg_layer_clear(g_svg_base);
    svg_draw_pass(svg, current);
}
