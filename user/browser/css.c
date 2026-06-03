/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Marvin Kicha <https://github.com/MarvInt64/> */

/* css — stylesheet parsing + selector matching + cascade. See css.h. */

#include "css.h"
#include "dom.h"
#include "umalloc.h"

static char lc(char c){ return (c>='A'&&c<='Z')?(char)(c+32):c; }
static int is_space(char c){ return c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\f'||c=='\v'; }
static int ieq(const char *a,const char *b){
    int i=0; for(;a[i]&&b[i];++i) if(lc(a[i])!=lc(b[i])) return 0; return a[i]==0&&b[i]==0;
}
static int ihas(const char *haystack, const char *needle){
    int nl=0; const char *p=needle; while(*p){nl++;p++;}
    for(;*haystack;++haystack){
        int k=0; const char *h=haystack;
        while(k<nl&&lc(*h)==lc(needle[k])){h++;k++;}
        if(k==nl) return 1;
    }
    return 0;
}

/* ---- CSS custom property store ----------------------------------------- */

#define CSS_VAR_MAX 256
struct css_var { char name[64]; char value[128]; };

/* ---- Selector: up to 3 parts (leaf + 2 ancestors) ----------------------- */

#define CSS_SEL_PARTS 3

struct css_sel_part {
    char tag[16];
    char cls[32];   /* first class token only */
    char id[32];
    signed char pseudo; /* 0=none, 1=first-child, 2=last-child, 3=nth-child */
    short nth_a, nth_b; /* :nth-child(an+b) */
};

struct css_rule {
    struct css_sel_part parts[CSS_SEL_PARTS]; /* parts[0]=leaf, parts[1..]=ancestors */
    char  combinators[CSS_SEL_PARTS];         /* combinators[i]: how parts[i] reaches parts[i+1] */
    int   part_count;
    int   spec, order, unsupported;
    char *decls; /* offset during parse, pointer after finalise */
};

struct css_sheet {
    struct css_rule *rules; int count, cap;
    char *arena; unsigned long alen, acap;
    struct css_var vars[CSS_VAR_MAX]; int var_count;
};

/* ---- Arena helpers ------------------------------------------------------- */

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
      int i;
      for(i=0;i<CSS_SEL_PARTS;i++){
          r->parts[i].tag[0]=r->parts[i].cls[0]=r->parts[i].id[0]=0;
          r->parts[i].pseudo=0; r->parts[i].nth_a=r->parts[i].nth_b=0;
          r->combinators[i]=0;
      }
      r->part_count=0; r->spec=0; r->order=s->count; r->unsupported=0; r->decls=0;
      return r; }
}

/* ---- Pseudo-class helpers (for matching) -------------------------------- */

static int is_first_element_child(const struct dom_node *node, const struct dom_node *parent){
    const struct dom_node *c; if(!parent) return 0;
    c=parent->first_child;
    while(c && c->type==DOM_TEXT) c=c->next_sibling;
    return c==node;
}
static int is_last_element_child(const struct dom_node *node, const struct dom_node *parent){
    const struct dom_node *last=0, *c; if(!parent) return 0;
    for(c=parent->first_child;c;c=c->next_sibling) if(c->type!=DOM_TEXT) last=c;
    return last==node;
}
static int element_child_index(const struct dom_node *node, const struct dom_node *parent){
    int idx=0; const struct dom_node *c; if(!parent) return 0;
    for(c=parent->first_child;c;c=c->next_sibling){
        if(c->type!=DOM_TEXT){ ++idx; if(c==node) return idx; }
    }
    return 0;
}

/* ---- Selector part parsing ---------------------------------------------- */

static int sel_is_stop(char c){
    return c=='.'||c=='#'||c==':'||c=='['||c=='('||is_space(c)||c=='>'||c=='+'||c=='~';
}

/* Parse one compound selector token ("div.foo#bar:first-child") into a part. */
static void parse_one_part(const char *s, int n, struct css_sel_part *p, int *unsupported_out){
    int i=0,k;
    p->tag[0]=p->cls[0]=p->id[0]=0;
    p->pseudo=0; p->nth_a=0; p->nth_b=0;

    /* Tag */
    k=0;
    while(i<n && !sel_is_stop(s[i]) && k<15) p->tag[k++]=lc(s[i++]);
    p->tag[k]=0;
    if(p->tag[0]=='*'&&p->tag[1]==0) p->tag[0]=0;

    while(i<n){
        if(s[i]=='.'){
            i++; k=0;
            while(i<n && !sel_is_stop(s[i]) && k<31) p->cls[k++]=s[i++];
            p->cls[k]=0;
        } else if(s[i]=='#'){
            i++; k=0;
            while(i<n && !sel_is_stop(s[i]) && k<31) p->id[k++]=s[i++];
            p->id[k]=0;
        } else if(s[i]==':'){
            i++;
            /* pseudo-element :: → skip, mark unsupported */
            if(i<n && s[i]==':'){
                while(i<n && !is_space(s[i]) && s[i]!='(') i++;
                if(i<n && s[i]=='('){
                    int d=1; i++;
                    while(i<n&&d>0){if(s[i]=='(')d++;else if(s[i]==')')d--;i++;}
                }
                if(unsupported_out) *unsupported_out=1;
                continue;
            }
            char psname[24]; int pn=0;
            while(i<n && s[i]!='(' && !is_space(s[i]) && s[i]!='.' && s[i]!='#' && s[i]!=':'&&pn<23)
                psname[pn++]=lc(s[i++]);
            psname[pn]=0;

            if(ieq(psname,"first-child")||ieq(psname,"first-of-type")){
                p->pseudo=1;
            } else if(ieq(psname,"last-child")||ieq(psname,"last-of-type")){
                p->pseudo=2;
            } else if(ieq(psname,"nth-child")||ieq(psname,"nth-of-type")){
                if(i<n&&s[i]=='('){
                    char arg[20]; int an=0; i++;
                    while(i<n&&s[i]!=')'&&an<19) arg[an++]=lc(s[i++]);
                    arg[an]=0;
                    if(i<n&&s[i]==')') i++;
                    /* trim whitespace */
                    while(an>0&&is_space(arg[an-1])) arg[--an]=0;
                    /* parse: odd / even / N / 2n / 2n+1 */
                    if(ieq(arg,"odd"))  { p->pseudo=3; p->nth_a=2; p->nth_b=1; }
                    else if(ieq(arg,"even")){ p->pseudo=3; p->nth_a=2; p->nth_b=0; }
                    else {
                        const char *q=arg;
                        int coef=0, num=0;
                        while(*q>='0'&&*q<='9'){ coef=coef*10+(*q-'0'); q++; }
                        if(lc(*q)=='n'){
                            q++;
                            if(coef==0&&q==arg+1) coef=1; /* bare "n" */
                            while(*q==' ') q++;
                            int sign=1;
                            if(*q=='+'){q++; while(*q==' ')q++;}
                            else if(*q=='-'){sign=-1; q++; while(*q==' ')q++;}
                            while(*q>='0'&&*q<='9'){ num=num*10+(*q-'0'); q++; }
                            p->pseudo=3; p->nth_a=(short)coef; p->nth_b=(short)(sign*num);
                        } else {
                            /* just a number */
                            p->pseudo=3; p->nth_a=0; p->nth_b=(short)coef;
                        }
                    }
                }
            } else if(ieq(psname,"not")){
                /* :not() — skip arg, don't apply pseudo constraint */
                if(i<n&&s[i]=='('){
                    int d=1; i++;
                    while(i<n&&d>0){if(s[i]=='(')d++;else if(s[i]==')')d--;i++;}
                }
                /* Treat :not() as "no constraint" (applies to all) — conservative */
            } else {
                /* :hover, :focus, :active, :visited, :checked, etc. — ignore (apply rule anyway) */
                if(i<n&&s[i]=='('){
                    int d=1; i++;
                    while(i<n&&d>0){if(s[i]=='(')d++;else if(s[i]==')')d--;i++;}
                }
            }
        } else if(s[i]=='['){
            /* attribute selector — skip */
            int d=1; i++;
            while(i<n&&d>0){if(s[i]=='[')d++;else if(s[i]==']')d--;i++;}
        } else {
            i++;
        }
    }
}

/* Parse a full selector (possibly multi-part) into rule's parts[]. */
static void parse_selector(const char *sel, int n, struct css_rule *r){
    /* Tokenize left-to-right into parts, then store reversed (leaf first). */
    char ptxt[CSS_SEL_PARTS][80];
    char combs[CSS_SEL_PARTS]; /* combinator AFTER each part */
    int pc=0;
    int i=0;

    while(i<n && pc<CSS_SEL_PARTS){
        while(i<n && is_space(sel[i])) i++;
        if(i>=n) break;

        /* Collect compound token */
        int ps=i, depth=0; char q=0;
        while(i<n){
            char c=sel[i];
            if(q){ if(c==q)q=0; }
            else if(c=='"'||c=='\'') q=c;
            else if(c=='(') depth++;
            else if(c==')'){ if(depth>0)depth--; }
            else if(depth==0&&(is_space(c)||c=='>'||c=='+'||c=='~')) break;
            i++;
        }
        int plen=i-ps; if(plen>79)plen=79;
        { int k; for(k=0;k<plen;k++) ptxt[pc][k]=sel[ps+k]; ptxt[pc][plen]=0; }
        combs[pc]=0;

        /* combinator after this part */
        int ws=i;
        while(i<n && is_space(sel[i])) i++;
        if(i<n && (sel[i]=='>'||sel[i]=='+'||sel[i]=='~')){
            combs[pc]=sel[i]; i++;
            while(i<n && is_space(sel[i])) i++;
        } else if(i<n){
            combs[pc]=' '; /* descendant — consumed whitespace already */
            i=ws; /* the actual ws token was already skipped; reset if needed */
            /* Actually the next iteration will skip whitespace, so just set combinator */
            combs[pc]=' ';
        }
        pc++;
    }

    /* Clamp */
    if(pc>CSS_SEL_PARTS) pc=CSS_SEL_PARTS;
    r->part_count=pc;

    /* Store reversed: parts[0]=leaf (rightmost) */
    r->spec=0; r->unsupported=0;
    int pi;
    for(pi=0;pi<pc;pi++){
        int src=pc-1-pi; /* reverse */
        int unsup=0;
        parse_one_part(ptxt[src],(int)__builtin_strlen(ptxt[src]),&r->parts[pi],&unsup);
        if(unsup) r->unsupported=1;
        /* combinators[pi] = how to get from parts[pi] to parts[pi+1]:
           it's the combinator BEFORE part[src] in the original = combs[src-1] */
        r->combinators[pi] = (src>0) ? combs[src-1] : 0;
        /* spec contribution */
        r->spec += (r->parts[pi].id[0]?100:0)+(r->parts[pi].cls[0]?10:0)+(r->parts[pi].tag[0]?1:0);
    }

    /* Adjacent/sibling combinators — we don't support these properly */
    for(pi=0;pi<pc;pi++){
        if(r->combinators[pi]=='+'||r->combinators[pi]=='~'){
            r->unsupported=1; break;
        }
    }

    /* Empty selector */
    if(pc==0 || (!r->parts[0].tag[0]&&!r->parts[0].cls[0]&&!r->parts[0].id[0]))
        r->unsupported=1;
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
                    for(j=0;j<s->var_count;j++){
                        if(ieq(s->vars[j].name,name)){
                            int k; for(k=0;k<vn;k++) s->vars[j].value[k]=val[k];
                            s->vars[j].value[vn]=0; goto next_var;
                        }
                    }
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
        next_var:
        while(i<len&&decls[i]!=';') i++;
        if(i<len&&decls[i]==';') i++;
    }
}

/* ---- @media condition evaluation --------------------------------------- */

static int eval_media_cond(const char *cond, int viewport_w){
    /* Return 0 to skip, 1 to include */
    if(ihas(cond,"print")||ihas(cond,"speech")) return 0;
    if(ihas(cond,"prefers-color-scheme")&&ihas(cond,"dark")) return 0;
    /* min-width / max-width */
    const char *p=cond;
    while(*p){
        if(lc(p[0])=='m'&&lc(p[1])=='i'&&lc(p[2])=='n'&&p[3]=='-'&&
           lc(p[4])=='w'&&lc(p[5])=='i'&&lc(p[6])=='d'&&lc(p[7])=='t'&&lc(p[8])=='h'){
            const char *q=p+9;
            while(*q==' ')q++;
            if(*q==':'){
                q++; while(*q==' ')q++;
                int val=0; while(*q>='0'&&*q<='9'){val=val*10+(*q-'0');q++;}
                if(viewport_w<val) return 0;
            }
        }
        if(lc(p[0])=='m'&&lc(p[1])=='a'&&lc(p[2])=='x'&&p[3]=='-'&&
           lc(p[4])=='w'&&lc(p[5])=='i'&&lc(p[6])=='d'&&lc(p[7])=='t'&&lc(p[8])=='h'){
            const char *q=p+9;
            while(*q==' ')q++;
            if(*q==':'){
                q++; while(*q==' ')q++;
                int val=0; while(*q>='0'&&*q<='9'){val=val*10+(*q-'0');q++;}
                if(viewport_w>val) return 0;
            }
        }
        p++;
    }
    return 1;
}

/* ---- Main parser -------------------------------------------------------- */

static int css_parse_inner(struct css_sheet *s, const char *text, int i, int end, int vw){
    while(i<end){
        while(i<end&&is_space(text[i])) i++;
        if(i+1<end&&text[i]=='/'&&text[i+1]=='*'){
            i+=2; while(i+1<end&&!(text[i]=='*'&&text[i+1]=='/')) i++; i+=2; continue;
        }
        if(i<end&&text[i]=='}') return i;
        if(i>=end) break;

        if(text[i]=='@'){
            int j=i+1; while(j<end&&!is_space(text[j])&&text[j]!='{') j++;
            int nm=j-i-1; char atname[12]; int k;
            for(k=0;k<nm&&k<11;k++) atname[k]=lc(text[i+1+k]); atname[k<11?k:11]=0;

            if(ieq(atname,"media")){
                int cstart=j;
                while(i<end&&text[i]!='{') i++;
                if(i>=end) break;
                char cond[160]; int cn=0;
                for(j=cstart;j<i&&cn<159;j++) cond[cn++]=text[j]; cond[cn]=0;
                i++; /* past '{' */

                if(!eval_media_cond(cond,vw)){
                    int depth=1;
                    while(i<end&&depth>0){
                        if(text[i]=='{')depth++;
                        else if(text[i]=='}')depth--;
                        i++;
                    }
                } else {
                    i=css_parse_inner(s,text,i,end,vw);
                    if(i<end&&text[i]=='}') i++;
                }
            } else {
                int depth=0;
                while(i<end){ char c=text[i++];
                    if(c=='{')depth++;
                    else if(c=='}'){ if(--depth<=0) break; }
                    else if(c==';'&&depth==0) break;
                }
            }
            continue;
        }

        int sel_start=i;
        while(i<end&&text[i]!='{'&&text[i]!='}') i++;
        if(i>=end||text[i]=='}') return i;
        int sel_end=i;
        i++;
        int blk_start=i;
        { int depth=1;
          while(i<end&&depth>0){
              if(text[i]=='{')depth++;
              else if(text[i]=='}')depth--;
              i++;
          }
        }
        int blk_end=i-1;

        collect_vars(s,text+blk_start,blk_end-blk_start);
        long doff=arena_put(s,text+blk_start,blk_end-blk_start);
        if(doff<0) break;

        /* split comma selector list */
        { int a=sel_start;
          while(a<sel_end){
              int b=a;
              /* find comma at depth 0 */
              int d=0; char q=0;
              while(b<sel_end){
                  char c=text[b];
                  if(q){ if(c==q)q=0; }
                  else if(c=='"'||c=='\'') q=c;
                  else if(c=='('||c=='[') d++;
                  else if(c==')'||c==']'){ if(d>0)d--; }
                  else if(c==','&&d==0) break;
                  b++;
              }
              { struct css_rule *r=rule_new(s); if(!r) break;
                parse_selector(text+a,b-a,r); r->decls=(char*)(long)doff; }
              a=(b<sel_end)?b+1:sel_end;
          }
        }
    }
    return i;
}

struct css_sheet *css_parse(const char *text, int n, int viewport_w){
    struct css_sheet *s=(struct css_sheet*)umalloc(sizeof(struct css_sheet));
    if(!s) return 0;
    s->rules=0; s->count=0; s->cap=0;
    s->arena=0; s->alen=0; s->acap=0;
    s->var_count=0;

    css_parse_inner(s,text,0,n,viewport_w);

    /* finalise: offsets -> pointers */
    { int z; for(z=0;z<s->count;++z) s->rules[z].decls=s->arena+(long)s->rules[z].decls; }
    return s;
}

void css_free(struct css_sheet *s){
    if(!s) return;
    if(s->rules) ufree(s->rules);
    if(s->arena) ufree(s->arena);
    ufree(s);
}

/* ---- Selector matching -------------------------------------------------- */

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

/* Match one selector part against a node, using parent for pseudo-class checks. */
static int match_one_part(const struct css_sel_part *p,
                           const struct dom_node *node,
                           const struct dom_node *parent)
{
    if(!node || node->type==DOM_TEXT) return 0;
    if(p->tag[0]&&!ieq(node->tag,p->tag)) return 0;
    if(p->cls[0]&&!has_class(node,p->cls)) return 0;
    if(p->id[0]){ const char *idv=dom_attr(node,"id"); if(!idv||!ieq(idv,p->id)) return 0; }
    if(!p->tag[0]&&!p->cls[0]&&!p->id[0]) return 0;

    /* Structural pseudo-classes */
    if(p->pseudo==1){ /* :first-child */
        if(!is_first_element_child(node,parent)) return 0;
    } else if(p->pseudo==2){ /* :last-child */
        if(!is_last_element_child(node,parent)) return 0;
    } else if(p->pseudo==3){ /* :nth-child(an+b) */
        int pos=element_child_index(node,parent);
        if(pos<=0) return 0;
        int a=p->nth_a, b=p->nth_b;
        if(a==0){
            if(pos!=b) return 0;
        } else {
            int rem=pos-b;
            if(rem<0||(rem%a)!=0) return 0;
        }
    }
    return 1;
}

static int rule_matches(const struct css_rule *r,
                         const struct dom_node *node,
                         const struct dom_node * const *ancestors,
                         int anc_depth)
{
    if(r->unsupported||r->part_count==0) return 0;

    const struct dom_node *parent=(anc_depth>0)?ancestors[anc_depth-1]:0;

    /* Match leaf */
    if(!match_one_part(&r->parts[0],node,parent)) return 0;
    if(r->part_count==1) return 1;

    /* Walk ancestors for remaining parts */
    int anc_idx=anc_depth-1; /* index of direct parent */
    int i;
    for(i=1;i<r->part_count;i++){
        char comb=r->combinators[i-1]; /* how parts[i-1] reaches parts[i] */
        if(comb=='>'){
            /* direct child: ancestors[anc_idx] must match */
            if(anc_idx<0) return 0;
            const struct dom_node *anc_parent=(anc_idx>0)?ancestors[anc_idx-1]:0;
            if(!match_one_part(&r->parts[i],ancestors[anc_idx],anc_parent)) return 0;
            anc_idx--;
        } else { /* ' ' = descendant */
            int found=0;
            while(anc_idx>=0){
                const struct dom_node *anc_parent=(anc_idx>0)?ancestors[anc_idx-1]:0;
                if(match_one_part(&r->parts[i],ancestors[anc_idx],anc_parent)){
                    found=1; anc_idx--; break;
                }
                anc_idx--;
            }
            if(!found) return 0;
        }
    }
    return 1;
}

/* ---- CSS variable resolution ------------------------------------------- */

static void resolve_vars(const struct css_sheet *s, char *buf, int cap){
    char tmp[1280]; int tp=0, i=0;
    if(!s||s->var_count==0) return;
    while(buf[i]&&tp<cap-1){
        if(buf[i]=='v'&&buf[i+1]=='a'&&buf[i+2]=='r'&&buf[i+3]=='('&&
           buf[i+4]=='-'&&buf[i+5]=='-'){
            i+=6;
            char nm[64]; int nn=0;
            while(buf[i]&&buf[i]!=')'&&buf[i]!=','&&nn<63) nm[nn++]=buf[i++];
            while(nn>0&&is_space(nm[nn-1])) nn--;
            nm[nn]=0;
            if(buf[i]==')') i++;
            { int j;
              for(j=0;j<s->var_count;j++){
                  if(ieq(s->vars[j].name,nm)){
                      const char *v=s->vars[j].value;
                      while(*v&&tp<cap-1) tmp[tp++]=*v++;
                      goto resolved;
                  }
              }
            }
            resolved:;
        } else {
            tmp[tp++]=buf[i++];
        }
    }
    tmp[tp]=0;
    { int k; for(k=0;tmp[k];k++) buf[k]=tmp[k]; buf[k]=0; }
}

/* ---- Public: match + cascade ------------------------------------------- */

int css_match(struct css_sheet *s, const struct dom_node *node,
              const struct dom_node * const *ancestors, int anc_depth,
              char *out, int cap)
{
    int matched[64]; int mn=0;
    int i, o=0;
    if(!s||!node){ if(cap)out[0]=0; return 0; }

    for(i=0;i<s->count&&mn<64;++i)
        if(rule_matches(&s->rules[i],node,ancestors,anc_depth)) matched[mn++]=i;

    /* sort by (spec, order) ascending so highest-specificity rules appear last (win) */
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

    if(s->var_count>0) resolve_vars(s,out,cap);
    return mn;
}
