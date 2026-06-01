/* css — a small CSS cascade for the VibeOS browser (stage 2).
 *
 * Parses <style> stylesheet text into rules, matches simple selectors against
 * DOM nodes (tag / .class / #id and compounds like a.foo, div#x, plus comma
 * lists), and produces the cascaded declaration block for a node in
 * specificity + source order. The browser then feeds that block (followed by
 * the element's inline style="") to the existing apply_decls() so the same
 * property parser handles everything.
 *
 * Not a full CSS engine: no descendant/child combinators, no pseudo-classes,
 * no media queries — just enough that class/id/tag styling and display:none
 * work, which is the bulk of what shapes a page. */
#ifndef VIBEOS_CSS_H
#define VIBEOS_CSS_H

struct dom_node;
struct css_sheet;

/* Parse stylesheet text (concatenated <style> contents). Returns an umalloc'd
 * sheet (free with css_free), or 0 on failure / empty. */
struct css_sheet *css_parse(const char *text, int n);
void css_free(struct css_sheet *s);

/* Append the cascaded declarations matching `node` (ascending specificity, so a
 * left-to-right property parser lets higher-specificity rules win) into out[],
 * NUL-terminated. Returns the number of matching rules. */
int css_match(struct css_sheet *s, const struct dom_node *node, char *out, int cap);

#endif
