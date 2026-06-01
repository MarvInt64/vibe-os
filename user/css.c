/* css — stylesheet parsing + selector matching + cascade. See css.h. */
#include "css.h"
#include "dom.h"
#include "umalloc.h"

static char lc(char c){ return (c>='A'&&c<='Z')?(char)(c+32):c; }
static int is_space(char c){ return c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\f'||c=='\v'; }
static int ieq(const char *a,const char *b){ int i=0; for(;a[i]&&b[i];++i) if(lc(a[i])!=lc(b[i])) return 0; return a[i]==0&&b[i]==0; }

/* One rule = one simple/compound selector + its declaration block. A comma
 * list in the source expands to several rules sharing the same block. */
struct css_rule {
    char tag[24];      /* "" = any element */
    char cls[48];      /* "" = no class constraint */
    char id[48];       /* "" = no id constraint */
    int  spec;         /* specificity: id*100 + class*10 + tag */
    int  order;        /* source order (tiebreak) */
    int  unsupported;  /* key has [attr]/:pseudo we can't evaluate -> never match */
    char *decls;       /* arena offset during build, pointer after finalize */
};

struct css_sheet {
    struct css_rule *rules; int count, cap;
    char *arena; unsigned long alen, acap;
};

static long arena_put(struct css_sheet *s, const char *p, int len){
    long off;
    if(s->alen + (unsigned long)len + 1 > s->acap){
        unsigned long nc = s->acap ? s->acap*2 : 4096;
        char *na; while(nc < s->alen+(unsigned long)len+1) nc*=2;
        na = (char*)urealloc(s->arena, nc); if(!na) return -1;
        s->arena=na; s->acap=nc;
    }
    off=(long)s->alen;
    { int i; for(i=0;i<len;++i) s->arena[off+i]=p[i]; }
    s->arena[off+len]=0; s->alen += (unsigned long)len+1;
    return off;
}

static struct css_rule *rule_new(struct css_sheet *s){
    if(s->count>=s->cap){
        int nc=s->cap?s->cap*2:64;
        struct css_rule *nr=(struct css_rule*)urealloc(s->rules,(umsize_t)nc*sizeof(struct css_rule));
        if(!nr) return 0;
        s->rules=nr; s->cap=nc;
    }
    { struct css_rule *r=&s->rules[s->count++]; r->tag[0]=r->cls[0]=r->id[0]=0; r->spec=0; r->order=s->count; r->unsupported=0; r->decls=0; return r; }
}

/* stop char for a tag/class/id token */
static int sel_stop(char c){ return c=='.'||c=='#'||c=='['||c==':'||c=='('||is_space(c); }

/* Parse a selector into a rule. For a descendant selector ("A B C") we use the
 * RIGHTMOST compound (the key element that actually matches) — taking the first
 * was the bug that made "html ... {display:none}" hide the whole page. We don't
 * evaluate the ancestor parts, so if the key carries an [attr] or :pseudo we
 * can't check, the rule is marked unsupported and never matches (conservative:
 * better to miss a style than to wrongly hide content). */
static void parse_selector(const char *sel, int n, struct css_rule *r){
    int i, keystart=0, depth=0; char q=0;

    /* find the start of the rightmost top-level compound */
    for(i=0;i<n;++i){ char c=sel[i];
        if(q){ if(c==q)q=0; }
        else if(c=='"'||c=='\'') q=c;
        else if(c=='('||c=='[') depth++;
        else if(c==')'||c==']'){ if(depth>0)depth--; }
        else if(is_space(c) && depth==0) keystart=i+1;
    }
    i=keystart;
    /* tag */
    { int k=0; while(i<n && !sel_stop(sel[i]) && k<23){ r->tag[k++]=lc(sel[i]); i++; } r->tag[k]=0;
      if(r->tag[0]=='*'&&r->tag[1]==0) r->tag[0]=0; }
    /* class / id segments; anything else (attr/pseudo) => unsupported */
    while(i<n && !is_space(sel[i])){
        if(sel[i]=='.'){ int k=0; i++; while(i<n && !sel_stop(sel[i]) && k<47){ r->cls[k++]=sel[i]; i++; } r->cls[k]=0; }
        else if(sel[i]=='#'){ int k=0; i++; while(i<n && !sel_stop(sel[i]) && k<47){ r->id[k++]=sel[i]; i++; } r->id[k]=0; }
        else if(sel[i]=='['||sel[i]==':'){ r->unsupported=1; i++; }
        else i++;
    }
    r->spec = (r->id[0]?100:0) + (r->cls[0]?10:0) + (r->tag[0]?1:0);
}

struct css_sheet *css_parse(const char *text, int n){
    struct css_sheet *s = (struct css_sheet*)umalloc(sizeof(struct css_sheet));
    int i=0;
    if(!s) return 0;
    s->rules=0; s->count=0; s->cap=0; s->arena=0; s->alen=0; s->acap=0;

    while(i<n){
        int sel_start, sel_end, blk_start, blk_end;
        long doff;
        /* skip whitespace + comments + @-rules (skip @media{...} bodies too) */
        while(i<n && is_space(text[i])) i++;
        if(i+1<n && text[i]=='/' && text[i+1]=='*'){ i+=2; while(i+1<n && !(text[i]=='*'&&text[i+1]=='/')) i++; i+=2; continue; }
        if(i<n && text[i]=='@'){ /* skip at-rule: to ';' or balanced '{...}' */
            int depth=0; while(i<n){ char c=text[i++]; if(c=='{')depth++; else if(c=='}'){ if(--depth<=0) break; } else if(c==';'&&depth==0) break; } continue; }
        if(i>=n) break;

        sel_start=i;
        while(i<n && text[i]!='{') i++;
        sel_end=i;
        if(i>=n) break;
        i++; /* past '{' */
        blk_start=i;
        while(i<n && text[i]!='}') i++;
        blk_end=i;
        if(i<n) i++; /* past '}' */

        doff = arena_put(s, text+blk_start, blk_end-blk_start);
        if(doff<0) break;

        /* split selector list on commas; each becomes a rule sharing the block */
        { int a=sel_start;
          while(a<sel_end){
              int b=a; while(b<sel_end && text[b]!=',') b++;
              { struct css_rule *r=rule_new(s); if(!r) break; parse_selector(text+a, b-a, r); r->decls=(char*)(long)doff; }
              a=b+1;
          }
        }
    }

    /* finalize: arena offsets -> pointers */
    { int z; for(z=0;z<s->count;++z) s->rules[z].decls = s->arena + (long)s->rules[z].decls; }
    return s;
}

void css_free(struct css_sheet *s){
    if(!s) return;
    if(s->rules) ufree(s->rules);
    if(s->arena) ufree(s->arena);
    ufree(s);
}

/* Does node have class token `cls` (class attr is space-separated)? */
static int has_class(const struct dom_node *node, const char *cls){
    const char *cv = dom_attr(node, "class");
    int i=0, cl=0;
    if(!cv || !cls[0]) return 0;
    while(cls[cl]) cl++;
    while(cv[i]){
        int s; while(cv[i]&&is_space(cv[i])) i++; s=i;
        while(cv[i]&&!is_space(cv[i])) i++;
        if(i-s==cl){ int k=0; for(;k<cl;++k) if(cv[s+k]!=cls[k]) break; if(k==cl) return 1; }
    }
    return 0;
}

static int rule_matches(const struct css_rule *r, const struct dom_node *node){
    if(r->unsupported) return 0;   /* key has [attr]/:pseudo we can't evaluate */
    if(r->tag[0] && !ieq(node->tag, r->tag)) return 0;
    if(r->cls[0] && !has_class(node, r->cls)) return 0;
    if(r->id[0]){ const char *idv=dom_attr(node,"id"); if(!idv || !ieq(idv, r->id)) return 0; }
    if(!r->tag[0] && !r->cls[0] && !r->id[0]) return 0;   /* empty selector */
    return 1;
}

int css_match(struct css_sheet *s, const struct dom_node *node, char *out, int cap){
    int matched[64]; int mn=0;
    int i, o=0;
    if(!s || !node) { if(cap)out[0]=0; return 0; }

    for(i=0;i<s->count && mn<64;++i) if(rule_matches(&s->rules[i], node)) matched[mn++]=i;

    /* insertion sort by (spec, order) ascending so higher specificity is
     * emitted LAST and wins in the left-to-right property parser */
    { int a,b; for(a=1;a<mn;++a){ int v=matched[a]; struct css_rule *rv=&s->rules[v];
        b=a-1; while(b>=0){ struct css_rule *rb=&s->rules[matched[b]];
            if(rb->spec>rv->spec || (rb->spec==rv->spec && rb->order>rv->order)){ matched[b+1]=matched[b]; b--; } else break; }
        matched[b+1]=v; } }

    for(i=0;i<mn;++i){ const char *d=s->rules[matched[i]].decls;
        while(*d && o+1<cap) out[o++]=*d++;
        if(o+1<cap) out[o++]=';';
    }
    if(o<cap) out[o]=0; else if(cap) out[cap-1]=0;
    return mn;
}
