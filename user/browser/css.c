/* css — stylesheet parsing + selector matching + cascade. See css.h.
 *
 * Improvements over the original:
 *   - @media blocks: entered (not skipped) unless condition contains "print",
 *     "speech", or "prefers-color-scheme" with "dark"
 *   - CSS custom properties: --name: value collected from all rules;
 *     var(--name) references resolved in css_match output
 *   - Same selector engine (rightmost compound) but now works inside media */

#include "css.h"
#include "dom.h"
#include "umalloc.h"

static char lc(char c){ return (c>='A'&&c<='Z')?(char)(c+32):c; }
static int is_space(char c){ return c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\f'||c=='\v'; }
static int ieq(const char *a,const char *b){
    int i=0; for(;a[i]&&b[i];++i) if(lc(a[i])!=lc(b[i])) return 0; return a[i]==0&&b[i]==0;
}
static int ihas(const char *haystack, const char *needle){
    int nl=0; const char *p=needle; while(*p) { nl++; p++; }
    for(;*haystack;++haystack){
        int k=0; const char *h=haystack;
        while(k<nl && lc(*h)==lc(needle[k])) { h++; k++; }
        if(k==nl) return 1;
    }
    return 0;
}

/* ---- CSS custom property store ----------------------------------------- */

#define CSS_VAR_MAX 256
struct css_var { char name[64]; char value[128]; };

/* ---- One rule = one compound selector + its declaration block ----------- */

struct css_rule {
    char tag[24]; char cls[48]; char id[48];
    int  spec, order, unsupported;
    char *decls;
};

struct css_sheet {
    struct css_rule *rules; int count, cap;
    char *arena; unsigned long alen, acap;
    struct css_var vars[CSS_VAR_MAX]; int var_count;
};

/* ---- Arena (string pool) helpers --------------------------------------- */

static long arena_put(struct css_sheet *s, const char *p, int len){
    long off; int i;
    if(s->alen+(unsigned long)len+1 > s->acap){
        unsigned long nc=s->acap?s->acap*2:4096;
        char *na; while(nc<s->alen+(unsigned long)len+1) nc*=2;
        na=(char*)urealloc(s->arena,nc); if(!na) return -1;
        s->arena=na; s->acap=nc;
    }
    off=(long)s->alen;
    for(i=0;i<len;++i) s->arena[off+i]=p[i];
    s->arena[off+len]=0; s->alen+=(unsigned long)len+1;
    return off;
}

static struct css_rule *rule_new(struct css_sheet *s){
    if(s->count>=s->cap){
        int nc=s->cap?s->cap*2:64;
        struct css_rule *nr=(struct css_rule*)urealloc(s->rules,(umsize_t)nc*sizeof(struct css_rule));
        if(!nr) return 0;
        s->rules=nr; s->cap=nc;
    }
    { struct css_rule *r=&s->rules[s->count++];
      r->tag[0]=r->cls[0]=r->id[0]=0; r->spec=0; r->order=s->count; r->unsupported=0; r->decls=0;
      return r; }
}

/* ---- Selector parsing -------------------------------------------------- */

static int sel_stop(char c){ return c=='.'||c=='#'||c=='['||c==':'||c=='('||is_space(c); }

static void parse_selector(const char *sel, int n, struct css_rule *r){
    int i,keystart=0,depth=0; char q=0;
    for(i=0;i<n;++i){ char c=sel[i];
        if(q){ if(c==q)q=0; }
        else if(c=='"'||c=='\'') q=c;
        else if(c=='('||c=='[') depth++;
        else if(c==')'||c==']'){ if(depth>0)depth--; }
        else if(is_space(c)&&depth==0) keystart=i+1;
    }
    i=keystart;
    { int k=0; while(i<n&&!sel_stop(sel[i])&&k<23){ r->tag[k++]=lc(sel[i]); i++; } r->tag[k]=0;
      if(r->tag[0]=='*'&&r->tag[1]==0) r->tag[0]=0; }
    while(i<n&&!is_space(sel[i])){
        if(sel[i]=='.'){ int k=0; i++; while(i<n&&!sel_stop(sel[i])&&k<47){ r->cls[k++]=sel[i]; i++; } r->cls[k]=0; }
        else if(sel[i]=='#'){ int k=0; i++; while(i<n&&!sel_stop(sel[i])&&k<47){ r->id[k++]=sel[i]; i++; } r->id[k]=0; }
        else if(sel[i]=='['||sel[i]==':'){ r->unsupported=1; i++; }
        else i++;
    }
    r->spec=(r->id[0]?100:0)+(r->cls[0]?10:0)+(r->tag[0]?1:0);
}

/* ---- CSS variable collection ------------------------------------------- */

static void collect_vars(struct css_sheet *s, const char *decls, int len){
    int i=0;
    while(i<len){
        while(i<len&&is_space(decls[i])) i++;
        if(i+1<len && decls[i]=='-' && decls[i+1]=='-'){
            char name[62]; int n=0;
            i+=2;
            while(i<len&&decls[i]!=':'&&decls[i]!=';'&&n<61) name[n++]=decls[i++];
            name[n]=0;
            /* trim trailing whitespace from name */
            while(n>0&&is_space(name[n-1])) name[--n]=0;
            if(decls[i]==':'){
                char val[126]; int vn=0;
                i++;
                while(i<len&&is_space(decls[i])) i++;
                while(i<len&&decls[i]!=';'&&vn<125) val[vn++]=decls[i++];
                while(vn>0&&is_space(val[vn-1])) vn--;
                val[vn]=0;
                if(n>0&&vn>0){
                    int j;
                    /* update existing */
                    for(j=0;j<s->var_count;j++){
                        if(ieq(s->vars[j].name,name)){
                            int k; for(k=0;k<vn;k++) s->vars[j].value[k]=val[k];
                            s->vars[j].value[vn]=0; goto next;
                        }
                    }
                    /* add new */
                    if(s->var_count<CSS_VAR_MAX){
                        int k;
                        for(k=0;k<n;k++) s->vars[s->var_count].name[k]=name[k];
                        s->vars[s->var_count].name[n]=0;
                        for(k=0;k<vn;k++) s->vars[s->var_count].value[k]=val[k];
                        s->vars[s->var_count].value[vn]=0;
                        s->var_count++;
                    }
                }
            }
        }
        next:
        while(i<len&&decls[i]!=';') i++;
        if(i<len&&decls[i]==';') i++;
    }
}

/* ---- Main parser (recursive for @media) -------------------------------- */

/* Parse rules from text[i..end). Returns position after last consumed char.
 * Stops (returns i pointing at '}') when it hits an unmatched '}' (signals
 * end of a @media body to the caller). */
static int css_parse_inner(struct css_sheet *s, const char *text, int i, int end){
    while(i<end){
        /* skip whitespace */
        while(i<end&&is_space(text[i])) i++;
        /* skip comments */
        if(i+1<end&&text[i]=='/'&&text[i+1]=='*'){
            i+=2; while(i+1<end&&!(text[i]=='*'&&text[i+1]=='/')) i++; i+=2; continue;
        }
        /* unmatched '}' -> end of @media body, signal to caller */
        if(i<end&&text[i]=='}') return i;
        if(i>=end) break;

        /* @-rule */
        if(text[i]=='@'){
            /* check for @media */
            int j=i+1; while(j<end&&!is_space(text[j])&&text[j]!='{') j++;
            /* name = text[i+1..j) */
            int nm=j-i-1; char atname[12]; int k;
            for(k=0;k<nm&&k<11;k++) atname[k]=lc(text[i+1+k]); atname[k<11?k:11]=0;

            if(ieq(atname,"media")){
                /* skip to body '{' */
                int cstart=j;
                while(i<end&&text[i]!='{') i++;
                if(i>=end){ break; }
                /* condition text is text[cstart..i) */
                char cond[128]; int cn=0;
                for(j=cstart;j<i&&cn<127;j++) cond[cn++]=text[j]; cond[cn]=0;
                i++; /* past '{' */

                /* skip if print / speech / dark color scheme */
                if(ihas(cond,"print")||ihas(cond,"speech")||
                   (ihas(cond,"prefers-color-scheme")&&ihas(cond,"dark"))){
                    int depth=1;
                    while(i<end&&depth>0){
                        if(text[i]=='{') depth++;
                        else if(text[i]=='}') depth--;
                        i++;
                    }
                } else {
                    /* enter body recursively */
                    i=css_parse_inner(s,text,i,end);
                    if(i<end&&text[i]=='}') i++; /* consume the media closing '}' */
                }
            } else {
                /* other @-rules: skip */
                int depth=0;
                while(i<end){ char c=text[i++];
                    if(c=='{') depth++;
                    else if(c=='}'){ if(--depth<=0) break; }
                    else if(c==';'&&depth==0) break;
                }
            }
            continue;
        }

        /* Normal rule: selector { declarations } */
        int sel_start=i;
        /* scan to '{', but stop on lone '}' */
        while(i<end&&text[i]!='{'&&text[i]!='}') i++;
        if(i>=end||text[i]=='}') return i; /* unmatched '}' */
        int sel_end=i;
        i++; /* past '{' */
        int blk_start=i;
        /* scan to matching '}', skipping nested {} (e.g. calc) */
        { int depth=1;
          while(i<end&&depth>0){
              if(text[i]=='{') depth++;
              else if(text[i]=='}') depth--;
              i++;
          }
        }
        int blk_end=i-1; /* points just before the closing '}' */

        /* collect CSS variables from this block */
        collect_vars(s,text+blk_start,blk_end-blk_start);

        long doff=arena_put(s,text+blk_start,blk_end-blk_start);
        if(doff<0) break;

        /* split comma selector list */
        { int a=sel_start;
          while(a<sel_end){
              int b=a; while(b<sel_end&&text[b]!=',') b++;
              { struct css_rule *r=rule_new(s); if(!r) break;
                parse_selector(text+a,b-a,r); r->decls=(char*)(long)doff; }
              a=b+1;
          }
        }
    }
    return i;
}

struct css_sheet *css_parse(const char *text, int n){
    struct css_sheet *s=(struct css_sheet*)umalloc(sizeof(struct css_sheet));
    if(!s) return 0;
    s->rules=0; s->count=0; s->cap=0;
    s->arena=0; s->alen=0; s->acap=0;
    s->var_count=0;

    css_parse_inner(s,text,0,n);

    /* finalize: arena offsets -> pointers */
    { int z; for(z=0;z<s->count;++z) s->rules[z].decls=s->arena+(long)s->rules[z].decls; }
    return s;
}

void css_free(struct css_sheet *s){
    if(!s) return;
    if(s->rules) ufree(s->rules);
    if(s->arena) ufree(s->arena);
    ufree(s);
}

/* ---- Selector matching ------------------------------------------------- */

static int has_class(const struct dom_node *node, const char *cls){
    const char *cv=dom_attr(node,"class");
    int i=0,cl=0;
    if(!cv||!cls[0]) return 0;
    while(cls[cl]) cl++;
    while(cv[i]){
        int s; while(cv[i]&&is_space(cv[i])) i++; s=i;
        while(cv[i]&&!is_space(cv[i])) i++;
        if(i-s==cl){ int k=0; for(;k<cl;++k) if(cv[s+k]!=cls[k]) break; if(k==cl) return 1; }
    }
    return 0;
}

static int rule_matches(const struct css_rule *r, const struct dom_node *node){
    if(r->unsupported) return 0;
    if(r->tag[0]&&!ieq(node->tag,r->tag)) return 0;
    if(r->cls[0]&&!has_class(node,r->cls)) return 0;
    if(r->id[0]){ const char *idv=dom_attr(node,"id"); if(!idv||!ieq(idv,r->id)) return 0; }
    if(!r->tag[0]&&!r->cls[0]&&!r->id[0]) return 0;
    return 1;
}

/* ---- CSS variable resolution ------------------------------------------- */

/* Replace var(--name) references in buf (in-place). */
static void resolve_vars(const struct css_sheet *s, char *buf, int cap){
    char tmp[1280]; int tp=0, i=0;
    if(!s||s->var_count==0) return;
    while(buf[i]&&tp<cap-1){
        /* detect var( */
        if(buf[i]=='v'&&buf[i+1]=='a'&&buf[i+2]=='r'&&buf[i+3]=='('&&
           buf[i+4]=='-'&&buf[i+5]=='-'){
            i+=6; /* skip "var(--" */
            char nm[64]; int nn=0;
            while(buf[i]&&buf[i]!=')'&&buf[i]!=','&&nn<63) nm[nn++]=buf[i++];
            /* trim whitespace */
            while(nn>0&&is_space(nm[nn-1])) nn--;
            nm[nn]=0;
            if(buf[i]==')') i++;
            /* look up */
            { int j;
              for(j=0;j<s->var_count;j++){
                  if(ieq(s->vars[j].name,nm)){
                      const char *v=s->vars[j].value;
                      while(*v&&tp<cap-1) tmp[tp++]=*v++;
                      goto resolved;
                  }
              }
            }
            /* not found — leave empty */
            resolved:;
        } else {
            tmp[tp++]=buf[i++];
        }
    }
    tmp[tp]=0;
    { int k; for(k=0;tmp[k];k++) buf[k]=tmp[k]; buf[k]=0; }
}

/* ---- Public: match + cascade ------------------------------------------- */

int css_match(struct css_sheet *s, const struct dom_node *node, char *out, int cap){
    int matched[64]; int mn=0;
    int i, o=0;
    if(!s||!node){ if(cap)out[0]=0; return 0; }

    for(i=0;i<s->count&&mn<64;++i)
        if(rule_matches(&s->rules[i],node)) matched[mn++]=i;

    /* insertion sort by (spec, order) ascending so higher-specificity rules win */
    { int a,b;
      for(a=1;a<mn;++a){ int v=matched[a]; struct css_rule *rv=&s->rules[v];
          b=a-1; while(b>=0){ struct css_rule *rb=&s->rules[matched[b]];
              if(rb->spec>rv->spec||(rb->spec==rv->spec&&rb->order>rv->order))
                  { matched[b+1]=matched[b]; b--; } else break; }
          matched[b+1]=v; }
    }

    for(i=0;i<mn;++i){ const char *d=s->rules[matched[i]].decls;
        while(*d&&o+1<cap) out[o++]=*d++;
        if(o+1<cap) out[o++]=';';
    }
    if(o<cap) out[o]=0; else if(cap) out[cap-1]=0;

    /* resolve CSS variable references */
    if(s->var_count>0) resolve_vars(s,out,cap);

    return mn;
}
