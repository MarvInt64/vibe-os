/* weblayout — a minimal HTML flow-layout engine for VibeOS's browser.
 *
 * A real step up from the plain-text webtext: it parses HTML into positioned,
 * styled runs (headings, bold, links, list bullets, rules) using inline/block
 * flow with word wrapping, and records clickable link rectangles. No CSS yet —
 * styling is driven by the tag names directly. Syscall-free (depends only on
 * umalloc) so it can be unit-tested on the host.
 *
 *   wl_layout(doc, html, n, viewport_w)  ->  doc->runs[] positioned in document
 *   coordinates (y grows downward), doc->height total, doc->hrefs[] for links. */
#ifndef VIBEOS_WEBLAYOUT_H
#define VIBEOS_WEBLAYOUT_H

typedef unsigned int wl_u32;

enum { WL_TEXT = 0, WL_RULE = 1, WL_BULLET = 2, WL_RECT = 3, WL_IMAGE = 4 };

struct wl_run {
    int kind;            /* WL_TEXT, WL_RULE, WL_BULLET, WL_RECT, WL_IMAGE */
    int x, y, w, h;      /* document coordinates */
    int px;              /* font pixel size for this run */
    int bold;            /* faux-bold (double draw) */
    int underline;
    wl_u32 color;
    wl_u32 bg;           /* run background (0 = transparent), e.g. inline code */
    int off, len;        /* text slice into doc->pool (WL_TEXT); WL_IMAGE: src href idx */
    int link;            /* index into doc->hrefs, or -1 */
};

/* The app supplies proportional font metrics; if unset, weblayout falls back to
 * a fixed-width approximation (used by host tests). */
void wl_set_metrics(int (*advance)(int codepoint, int px), int (*line_height)(int px));

/* Optional image sizer: src + max width -> reserved box w/h (aspect-fit).
 * Returns 1 if the image is known/decoded, 0 otherwise (placeholder used). */
void wl_set_image_sizer(int (*fn)(const char *src, int maxw, int *w, int *h));

#define WL_HREF_MAX 256

struct wl_doc {
    char *pool; int pool_len, pool_cap;
    struct wl_run *runs; int run_count, run_cap;
    char (*hrefs)[WL_HREF_MAX]; int href_count, href_cap;
    int height;
};

void wl_init(struct wl_doc *d);
void wl_free(struct wl_doc *d);

/* Lay out html[0..n) into `viewport_w` pixels wide. Returns total height. */
int wl_layout(struct wl_doc *d, const char *html, int n, int viewport_w);

/* Lay out a parsed DOM tree (the proper path). `root` is a struct dom_node*. */
struct dom_node;
int wl_layout_dom(struct wl_doc *d, struct dom_node *root, int viewport_w);

#endif
