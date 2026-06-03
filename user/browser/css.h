/* css — stylesheet parsing + selector matching + cascade.
 *
 * Supports:
 *   - tag / .class / #id compounds and comma lists
 *   - Descendant ( ) and child (>) combinators, up to 3 levels
 *   - :first-child, :last-child, :nth-child(an+b/odd/even)
 *   - CSS custom properties (--name / var(--name))
 *   - @media blocks with min-width / max-width evaluation */
#ifndef VIBEOS_CSS_H
#define VIBEOS_CSS_H

#ifdef __cplusplus
extern "C" {
#endif

struct dom_node;
struct css_sheet;

/* Parse stylesheet text. viewport_w is used to evaluate @media width conditions. */
struct css_sheet *css_parse(const char *text, int n, int viewport_w);
void css_free(struct css_sheet *s);

/* Append the cascaded declarations matching `node` into out[], NUL-terminated.
 * ancestors[0..anc_depth-1] = ancestor chain from root to direct parent.
 * Returns the number of matching rules. */
int css_match(struct css_sheet *s, const struct dom_node *node,
              const struct dom_node * const *ancestors, int anc_depth,
              char *out, int cap);

#ifdef __cplusplus
}
#endif

#endif
