#pragma once

/* layout_engine.h — improved HTML flow-layout engine for the VibeOS browser.
 *
 * Consumes a parsed DOM tree (dom.h) with CSS cascade (css.h) and produces a
 * flat list of positioned, styled runs in a wl_doc (weblayout.h).
 *
 * Improvements over the original weblayout.c:
 *   - Real CSS block-margin collapsing (no more stacked blank lines).
 *   - Proper per-element margins: <p> 12 px, <h1> 18 px, etc.
 *   - <nav> subtrees skipped entirely (navigation menus are noise in text).
 *   - <pre> rendered as a full-width background box with whitespace preserved.
 *   - <blockquote> with a left accent bar.
 *   - <footer> shown with dimmed text (not skipped — may have useful content).
 *   - <table> cells laid out inline with a gap between columns.
 *   - Page <title> can be extracted from the DOM separately. */

#include "weblayout.h"
#include "dom.h"
#include "css.h"

class LayoutEngine {
public:
    LayoutEngine() = default;

    /* Proportional font metrics from the glyph cache.  If not set a fixed
     * fallback is used (advance = px/2). */
    void set_metrics(int (*advance_fn)(int codepoint, int px),
                     int (*line_height_fn)(int px));

    /* Image sizer: given a src + max_width, fills w/h with the aspect-fitted
     * display size.  Returns 1 when the image is ready, 0 when loading. */
    void set_image_sizer(int (*fn)(const char *src, int maxw,
                                   int *w, int *h));

    /* Walk the DOM and populate doc with positioned runs.
     * Returns the total document height in pixels. */
    int layout(wl_doc *doc, dom_node *root, int viewport_w);

    /* Copy the text of the first <title> element into out (NUL-terminated). */
    static void extract_title(dom_node *root, char *out, int cap);

private:
    int  (*advance_fn_)(int codepoint, int px) = nullptr;
    int  (*line_height_fn_)(int px)            = nullptr;
    int  (*image_sizer_fn_)(const char *src, int maxw, int *w, int *h) = nullptr;
};
