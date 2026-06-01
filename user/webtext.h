/* webtext — turn an HTML response body into wrapped, readable lines.
 *
 * This is the engine half of the browser, kept free of syscalls and rendering
 * so it can be unit-tested on the host and reused by the later real layout
 * engine. It depends only on the umalloc heap.
 *
 * Pipeline:  raw HTML body  --webtext_from_html-->  plain text
 *            plain text     --webtext_wrap-------->  display lines (off/len) */
#ifndef VIBEOS_WEBTEXT_H
#define VIBEOS_WEBTEXT_H

struct webline { int off; int len; };

struct webtext {
    char *text;            /* umalloc'd readable text (NUL-terminated) */
    int   text_len;
    struct webline *lines; /* umalloc'd, grown on demand */
    int   line_count;
    int   line_cap;
};

/* Reset to empty (does not free — call webtext_free first if needed). */
void webtext_init(struct webtext *wt);

/* Free all owned buffers and reset. */
void webtext_free(struct webtext *wt);

/* Strip `body`[0..n) (HTML or plain text) to readable text stored in wt->text.
 * Removes script/style/head, drops tags, decodes common entities, collapses
 * whitespace, and inserts line breaks for block-level tags. Returns the text
 * length, or -1 on allocation failure. */
int webtext_from_html(struct webtext *wt, const char *body, int n);

/* Word-wrap wt->text to `cols` columns into wt->lines. Safe to call repeatedly
 * (e.g. on window resize). */
void webtext_wrap(struct webtext *wt, int cols);

#endif
