/* dom — a real HTML document tree for the VibeOS browser.
 *
 * Replaces the old stream-to-runs parser with a proper node tree (elements +
 * text + attributes), the foundation every later stage builds on: CSS cascade,
 * box layout, painting, and eventually scripting. Arena-allocated over the
 * umalloc heap; the whole document is freed in one shot with dom_free.
 *
 * The parser is an HTML5-ish tokenizer + tree builder: it lowercases tag names,
 * parses quoted/unquoted attributes, treats void elements (br, img, hr, meta,
 * link, input) as self-closing, captures raw <script>/<style> text, skips
 * comments and doctype, and auto-closes mismatched tags so malformed real-world
 * markup still yields a sane tree. */
#ifndef VIBEOS_DOM_H
#define VIBEOS_DOM_H

#ifdef __cplusplus
extern "C" {
#endif

enum dom_type { DOM_ELEMENT = 0, DOM_TEXT = 1 };

struct dom_attr {
    char *name;     /* lowercased, arena string */
    char *value;    /* arena string (entity-decoded for text; raw for attrs) */
};

struct dom_node {
    int type;
    char tag[24];               /* element: lowercase tag name */
    char *text;                 /* text node: decoded UTF-8/Latin-1 content */
    struct dom_attr *attrs;
    int nattrs;
    struct dom_node *parent;
    struct dom_node *first_child;
    struct dom_node *last_child;
    struct dom_node *next_sibling;
};

struct dom_doc {
    struct dom_node *root;      /* synthetic document root (tag "#document") */
    /* arena */
    char *arena; unsigned long arena_len, arena_cap;
    /* Nodes are allocated individually (stable pointers); only this index array
     * grows. A growing node *array* would move and invalidate every parent/
     * child/sibling pointer once it reallocs — that crashed on large pages. */
    struct dom_node **nodes; int node_count, node_cap;
};

void dom_init(struct dom_doc *d);
void dom_free(struct dom_doc *d);

/* Parse html[0..n) into the tree. Returns the document root (never NULL after
 * a successful init), or 0 on allocation failure. */
struct dom_node *dom_parse(struct dom_doc *d, const char *html, int n);

/* Attribute lookup (case-insensitive). Returns value or 0. */
const char *dom_attr(const struct dom_node *node, const char *name);
struct dom_node *dom_add_text_node(struct dom_doc *d, struct dom_node *parent, const char *text);
struct dom_node *dom_add_element_node(struct dom_doc *d, struct dom_node *parent, const char *tag);
void dom_append_child(struct dom_node *parent, struct dom_node *child);


#ifdef __cplusplus
}
#endif

#endif
