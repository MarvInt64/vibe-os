/* weblayout — HTML flow-layout engine. See weblayout.h. Syscall-free; uses the
 * umalloc heap. Font metrics: 8x16 base glyph, scaled by run->scale. */
#include "weblayout.h"
#include "umalloc.h"
#include "dom.h"
#include "css.h"

#define WL_BODY_PX 17     /* body text pixel size */

/* App-supplied proportional metrics (fall back to a fixed approximation). */
static int (*g_adv_fn)(int,int);
static int (*g_lh_fn)(int);
void wl_set_metrics(int (*advance)(int,int), int (*line_height)(int)){ g_adv_fn=advance; g_lh_fn=line_height; }

/* App-supplied image sizer: given a src string + the max width, returns the
 * box w/h to reserve (already aspect-scaled to fit). Returns 1 if the image is
 * known/decoded, 0 if not (the layout then uses a small placeholder box). */
static int (*g_img_fn)(const char *src, int maxw, int *w, int *h);
void wl_set_image_sizer(int (*fn)(const char*,int,int*,int*)){ g_img_fn=fn; }
static int m_adv(int cp,int px){ return g_adv_fn ? g_adv_fn(cp,px) : (px/2); }
static int m_lh(int px){ return g_lh_fn ? g_lh_fn(px) : (px+3); }
static int word_w(const char *s,int n,int px){ int w=0,i; for(i=0;i<n;++i) w+=m_adv((unsigned char)s[i],px); return w; }

/* Light "paper" theme — dark text on a near-white page (set by the browser). */
#define COL_TEXT  0x00202632u   /* body text */
#define COL_LINK  0x001a56dbu   /* links */
#define COL_HEAD  0x000d1b2au   /* headings */
#define COL_QUOTE 0x00566072u   /* blockquote text */
#define COL_RULE  0x00c8cedau   /* horizontal rule / accents */
#define COL_CODEBG 0x00e7eaf2u  /* inline code / pre background */

/* ---- small helpers (no libc dependency) ---- */
static void mcpy(char *d, const char *s, int n){ int i; for(i=0;i<n;++i) d[i]=s[i]; }
static char lc(char c){ return (c>='A'&&c<='Z')?(char)(c+32):c; }
static int is_space(char c){ return c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\f'||c=='\v'; }
static int is_alnum(char c){ return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9'); }
static int ieq(const char *a,const char *b){ int i=0; for(;a[i]&&b[i];++i) if(lc(a[i])!=lc(b[i])) return 0; return a[i]==0&&b[i]==0; }
static int slen(const char *s){ int n=0; while(s&&s[n])n++; return n; }

static int decode_entity(const char *e,int n,char *out){
    char name[12]; int i;
    if(n<=0) return -1;
    if(e[0]=='#'){
        unsigned int cp=0; int j=1;
        if(j<n&&(e[j]=='x'||e[j]=='X')){ j++; for(;j<n;++j){ char c=lc(e[j]); if(c>='0'&&c<='9')cp=cp*16u+(unsigned)(c-'0'); else if(c>='a'&&c<='f')cp=cp*16u+(unsigned)(c-'a'+10); else return -1; } }
        else { for(;j<n;++j){ if(e[j]<'0'||e[j]>'9')return -1; cp=cp*10u+(unsigned)(e[j]-'0'); } }
        if(cp==160){ out[0]=' '; return 1; }
        if(cp<128){ out[0]=(char)cp; return 1; }
        if(cp==8211||cp==8212){ out[0]='-'; return 1; }
        if(cp==8216||cp==8217){ out[0]='\''; return 1; }
        if(cp==8220||cp==8221){ out[0]='"'; return 1; }
        out[0]='?'; return 1;
    }
    for(i=0;i<n&&i<11;++i) name[i]=e[i];
    name[i]=0;
    if(ieq(name,"amp")){ out[0]='&'; return 1; }
    if(ieq(name,"lt")){ out[0]='<'; return 1; }
    if(ieq(name,"gt")){ out[0]='>'; return 1; }
    if(ieq(name,"quot")){ out[0]='"'; return 1; }
    if(ieq(name,"apos")){ out[0]='\''; return 1; }
    if(ieq(name,"nbsp")){ out[0]=' '; return 1; }
    if(ieq(name,"mdash")||ieq(name,"ndash")){ out[0]='-'; return 1; }
    if(ieq(name,"hellip")){ out[0]='.'; out[1]='.'; out[2]='.'; return 3; }
    return -1;
}

static int skip_until_close(const char *in,int n,int start,const char *name){
    int i=start, nl=slen(name);
    for(;i+1<n;++i){
        if(in[i]=='<'&&in[i+1]=='/'){
            int k=0,j=i+2;
            for(;k<nl&&j<n;++k,++j) if(lc(in[j])!=lc(name[k])) break;
            if(k==nl){ while(j<n&&in[j]!='>')++j; return (j<n)?j+1:n; }
        }
    }
    return n;
}

/* Extract attribute `attr` value from a tag body in[tagbody..end). */
static int get_attr(const char *in,int tagbody,int end,const char *attr,char *out,int cap){
    int al=slen(attr), i=tagbody;
    for(;i<end;++i){
        int k=0;
        while(k<al && i+k<end && lc(in[i+k])==lc(attr[k])) k++;
        if(k==al && i+k<end){
            int j=i+k;
            while(j<end && (in[j]==' '||in[j]=='\t')) j++;
            if(j<end && in[j]=='='){
                int o=0; char q=0;
                j++;
                while(j<end && (in[j]==' '||in[j]=='\t')) j++;
                if(j<end && (in[j]=='"'||in[j]=='\'')){ q=in[j]; j++; }
                while(j<end && o+1<cap){
                    char c=in[j];
                    if(q){ if(c==q) break; } else if(c==' '||c=='\t'||c=='>') break;
                    out[o++]=c; j++;
                }
                out[o]=0;
                return o>0;
            }
        }
    }
    return 0;
}

/* ---- inline style state ---- */
struct wl_style { int px; int bold; int underline; int italic; int strikethrough; wl_u32 color; wl_u32 bg; int link; int left; int hidden;
                  int padding_left; int padding_top; int padding_bottom;
                  int text_align; int line_height_pct;
                  int max_width; wl_u32 border_color; int border_width;
                  int text_transform; int list_style_none; int pos_fixed; };

/* ---- CSS-ish color + inline-style parsing ---- */
static int hexd(char c){ if(c>='0'&&c<='9')return c-'0'; c=lc(c); if(c>='a'&&c<='f')return c-'a'+10; return -1; }
static int parse_color(const char *s,wl_u32 *out){
    int i; static const struct { const char *n; wl_u32 c; } tbl[]={
        {"black",0x000000},{"white",0xffffff},{"red",0xcc0000},{"green",0x008000},
        {"blue",0x1a56db},{"navy",0x001f5f},{"gray",0x808080},{"grey",0x808080},
        {"silver",0xc0c0c0},{"orange",0xff6600},{"purple",0x800080},{"teal",0x008080},
        {"maroon",0x800000},{"olive",0x808000},{"yellow",0xd4b000},
        {"darkblue",0x00008b},{"darkgreen",0x006400},{"darkred",0x8b0000},
        {"darkgray",0xa9a9a9},{"darkgrey",0xa9a9a9},{"lightgray",0xd3d3d3},
        {"lightgrey",0xd3d3d3},{"lightblue",0xadd8e6},{"lightgreen",0x90ee90},
        {"lightyellow",0xffffe0},{"lightcoral",0xf08080},{"lightpink",0xffb6c1},
        {"lightskyblue",0x87cefa},{"lightsteelblue",0xb0c4de},
        {"pink",0xffb6c1},{"coral",0xff7f50},{"tomato",0xff6347},{"salmon",0xfa8072},
        {"crimson",0xdc143c},{"firebrick",0xb22222},{"indianred",0xcd5c5c},
        {"gold",0xffd700},{"goldenrod",0xdaa520},{"khaki",0xf0e68c},
        {"cyan",0x00ced1},{"aqua",0x00ced1},{"turquoise",0x40e0d0},
        {"magenta",0xff00ff},{"fuchsia",0xff00ff},{"violet",0xee82ee},
        {"orchid",0xda70d6},{"indigo",0x4b0082},
        {"steelblue",0x4682b4},{"cornflowerblue",0x6495ed},{"royalblue",0x4169e1},
        {"dodgerblue",0x1e90ff},{"deepskyblue",0x00bfff},{"skyblue",0x87ceeb},
        {"cadetblue",0x5f9ea0},{"mediumblue",0x0000cd},{"slateblue",0x6a5acd},
        {"chocolate",0xd2691e},{"saddlebrown",0x8b4513},{"sienna",0xa0522d},
        {"tan",0xd2b48c},{"brown",0xa52a2a},{"wheat",0xf5deb3},
        {"beige",0xf5f5dc},{"lavender",0xe6e6fa},{"mistyrose",0xffe4e1},
        {"ghostwhite",0xf8f8ff},{"whitesmoke",0xf5f5f5},{"aliceblue",0xf0f8ff},
        {"snow",0xfffafa},{"antiquewhite",0xfaebd7},{"bisque",0xffe4c4},
        {"chartreuse",0x7fff00},{"lime",0x00ff00},{"limegreen",0x32cd32},
        {"dimgray",0x696969},{"dimgrey",0x696969},{"slategray",0x708090},
        {0,0} };
    while(*s==' '||*s=='\t') s++;
    if(ieq(s,"transparent")) return 0;
    if(*s=='#'){
        int d[6],k=0; s++;
        while(k<6 && hexd(s[k])>=0){ d[k]=hexd(s[k]); k++; }
        if(k>=6){ *out=((wl_u32)(d[0]*16+d[1])<<16)|((wl_u32)(d[2]*16+d[3])<<8)|(wl_u32)(d[4]*16+d[5]); return 1; }
        if(k>=3){ *out=((wl_u32)(d[0]*17)<<16)|((wl_u32)(d[1]*17)<<8)|(wl_u32)(d[2]*17); return 1; }
        return 0;
    }
    /* rgb() / rgba() */
    if((s[0]=='r'||s[0]=='R')&&(s[1]=='g'||s[1]=='G')&&(s[2]=='b'||s[2]=='B')){
        const char *p=s; while(*p&&*p!='(') p++; if(*p=='('){
            p++;
            int rv=0,gv=0,bv=0;
            while(*p==' '||*p==',') p++; while(*p>='0'&&*p<='9'){rv=rv*10+(*p-'0');p++;}
            while(*p==' '||*p==',') p++; while(*p>='0'&&*p<='9'){gv=gv*10+(*p-'0');p++;}
            while(*p==' '||*p==',') p++; while(*p>='0'&&*p<='9'){bv=bv*10+(*p-'0');p++;}
            /* alpha */
            while(*p==' '||*p==',') p++;
            if((*p>='0'&&*p<='9')||*p=='.'){
                int aw=0,af=0; while(*p>='0'&&*p<='9'){aw=aw*10+(*p-'0');p++;}
                if(*p=='.'){ p++; int d=10; while(*p>='0'&&*p<='9'&&d<=100){af+=(*p-'0')*100/d;d*=10;p++;} }
                if(aw*100+af==0) return 0; /* fully transparent */
            }
            *out=((wl_u32)rv<<16)|((wl_u32)gv<<8)|(wl_u32)bv; return 1;
        }
    }
    for(i=0;tbl[i].n;++i) if(ieq(s,tbl[i].n)){ *out=tbl[i].c; return 1; }
    return 0;
}

/* Apply CSS declarations ("color:#333;font-weight:bold") to a style. */
static void apply_decls(const char *css,struct wl_style *st){
    int i=0;
    while(css[i]){
        char prop[24], val[64]; int p=0,v=0;
        while(css[i]&&css[i]!=':'&&css[i]!=';'){ if(css[i]!=' '&&p<23)prop[p++]=css[i]; i++; }
        prop[p]=0;
        if(css[i]==':'){ i++;
            while(css[i]&&css[i]!=';'){ if(v<63)val[v++]=css[i]; i++; }
        }
        val[v]=0;
        if(css[i]==';') i++;
        if(ieq(prop,"color")){ wl_u32 c; if(parse_color(val,&c)) st->color=c; }
        else if(ieq(prop,"font-weight")){ const char*w=val; while(*w==' ')w++; if(ieq(w,"bold")||ieq(w,"bolder")||ieq(w,"700")||ieq(w,"800")||ieq(w,"900")) st->bold=1; }
        else if(ieq(prop,"font-style")){ const char*w=val; while(*w==' ')w++; if(ieq(w,"italic")||ieq(w,"oblique")) st->italic=1; }
        else if(ieq(prop,"text-decoration")){ int k; for(k=0;val[k];++k){
            if(ieq(val+k,"underline")) st->underline=1;
            if(ieq(val+k,"line-through")) st->strikethrough=1;
        }}
        else if(ieq(prop,"font-size")){ const char*w=val; int px=0; while(*w==' ')w++; while(*w>='0'&&*w<='9'){px=px*10+(*w-'0');w++;} if(px>=10&&px<=96) st->px=px; }
        else if(ieq(prop,"display")){ const char*w=val; while(*w==' ')w++; st->hidden = ieq(w,"none"); }
        else if(ieq(prop,"background-color")||ieq(prop,"background")){ wl_u32 c; if(parse_color(val,&c)) st->bg=c; }
        else if(ieq(prop,"padding-left")){ const char*w=val; int px=0; while(*w==' ')w++; while(*w>='0'&&*w<='9'){px=px*10+(*w-'0');w++;} if(px>0&&px<=80) st->padding_left=px; }
        else if(ieq(prop,"padding-top")){ const char*w=val; int px=0; while(*w==' ')w++; while(*w>='0'&&*w<='9'){px=px*10+(*w-'0');w++;} if(px>0&&px<=80) st->padding_top=px; }
        else if(ieq(prop,"padding-bottom")){ const char*w=val; int px=0; while(*w==' ')w++; while(*w>='0'&&*w<='9'){px=px*10+(*w-'0');w++;} if(px>0&&px<=80) st->padding_bottom=px; }
        else if(ieq(prop,"padding")){ const char*w=val; int px=0; while(*w==' ')w++; while(*w>='0'&&*w<='9'){px=px*10+(*w-'0');w++;} if(px>0&&px<=80){ st->padding_left=px; st->padding_top=px; st->padding_bottom=px; } }
        else if(ieq(prop,"text-align")){ const char*w=val; while(*w==' ')w++;
            if(ieq(w,"center")) st->text_align=1;
            else if(ieq(w,"right")) st->text_align=2;
            else if(ieq(w,"left")) st->text_align=0; }
        else if(ieq(prop,"line-height")){ const char*w=val; while(*w==' ')w++;
            int whole=0,frac=0,frac_div=1;
            while(*w>='0'&&*w<='9'){whole=whole*10+(*w-'0');w++;}
            if(*w=='.'){w++; while(*w>='0'&&*w<='9'&&frac_div<100){frac=frac*10+(*w-'0');frac_div*=10;w++;}}
            int pct=whole*100+frac*100/frac_div;
            if(pct>=80&&pct<=300) st->line_height_pct=pct; }
        else if(ieq(prop,"max-width")){ const char*w=val; int px=0; while(*w==' ')w++;
            while(*w>='0'&&*w<='9'){px=px*10+(*w-'0');w++;} if(px>0&&px<=4000) st->max_width=px; }
        else if(ieq(prop,"border")||ieq(prop,"outline")){ const char*w=val; while(*w==' ')w++;
            if(ieq(w,"none")||ieq(w,"0")){ st->border_width=0; }
            else { int bw=0; while(*w>='0'&&*w<='9'){bw=bw*10+(*w-'0');w++;}
                while(*w&&*w!=' ')w++; while(*w==' ')w++; while(*w&&*w!=' ')w++; while(*w==' ')w++;
                wl_u32 bc=0xc8cedau; parse_color(w,&bc);
                if(bw>0&&bw<=8){ st->border_width=bw; st->border_color=bc; } } }
        else if(ieq(prop,"border-color")){ wl_u32 c=0; if(parse_color(val,&c)) st->border_color=c; }
        else if(ieq(prop,"border-width")){ const char*w=val; int px=0; while(*w==' ')w++;
            while(*w>='0'&&*w<='9'){px=px*10+(*w-'0');w++;} if(px>0&&px<=8) st->border_width=px; }
        else if(ieq(prop,"text-transform")){ const char*w=val; while(*w==' ')w++;
            st->text_transform=(ieq(w,"uppercase")||ieq(w,"capitalize"))?1:0; }
        else if(ieq(prop,"list-style")||ieq(prop,"list-style-type")){ const char*w=val; while(*w==' ')w++;
            st->list_style_none=ieq(w,"none")?1:0; }
        else if(ieq(prop,"position")){ const char*w=val; while(*w==' ')w++;
            st->pos_fixed=(ieq(w,"fixed")||ieq(w,"sticky"))?1:0; }
    }
}

/* ---- growable storage ---- */
static int pool_append(struct wl_doc *d,const char *s,int n){
    int off=d->pool_len;
    if(d->pool_len+n+1 > d->pool_cap){
        int nc=d->pool_cap? d->pool_cap*2 : 4096;
        while(nc < d->pool_len+n+1) nc*=2;
        char *np=(char*)urealloc(d->pool,(umsize_t)nc);
        if(!np) return -1;
        d->pool=np; d->pool_cap=nc;
    }
    mcpy(d->pool+off,s,n);
    d->pool_len+=n;
    return off;
}
static struct wl_run *add_run(struct wl_doc *d){
    if(d->run_count>=d->run_cap){
        int nc=d->run_cap? d->run_cap*2 : 128;
        struct wl_run *nr=(struct wl_run*)urealloc(d->runs,(umsize_t)nc*sizeof(struct wl_run));
        if(!nr) return 0;
        d->runs=nr; d->run_cap=nc;
    }
    return &d->runs[d->run_count++];
}
static int add_href(struct wl_doc *d,const char *href){
    int i=0;
    if(d->href_count>=d->href_cap){
        int nc=d->href_cap? d->href_cap*2 : 32;
        char (*nh)[WL_HREF_MAX]=(char(*)[WL_HREF_MAX])urealloc(d->hrefs,(umsize_t)nc*WL_HREF_MAX);
        if(!nh) return -1;
        d->hrefs=nh; d->href_cap=nc;
    }
    for(;href[i]&&i<WL_HREF_MAX-1;++i) d->hrefs[d->href_count][i]=href[i];
    d->hrefs[d->href_count][i]=0;
    return d->href_count++;
}

/* ---- layout state ---- */
struct wl_st {
    struct wl_doc *d;
    int vw;
    int cx, cy;
    int line_h;
    int left;
    int at_line_start;
    int pending_space;
    int line_start_run;  /* run index at start of current line */
    struct wl_style stk[32];
    int sp;
    struct css_sheet *sheet;
};

static void wl_apply_line_align(struct wl_st *S){
    int align = S->stk[S->sp].text_align;
    if(!align || S->at_line_start) return;
    int line_w = S->cx - S->stk[S->sp].left;
    int avail  = S->vw - S->stk[S->sp].left;
    int shift  = (align==1) ? (avail-line_w)/2 : (avail-line_w);
    int i;
    if(shift<=0) return;
    for(i=S->line_start_run; i<S->d->run_count; ++i) S->d->runs[i].x += shift;
}

static void wl_newline(struct wl_st *S){
    wl_apply_line_align(S);
    S->cy += (S->line_h>0 ? S->line_h : m_lh(WL_BODY_PX));
    S->cx = S->stk[S->sp].left;
    S->line_h = 0;
    S->at_line_start = 1;
    S->pending_space = 0;
    S->line_start_run = S->d->run_count;
}
static void wl_block(struct wl_st *S,int extra){
    if(!S->at_line_start) wl_newline(S);
    S->cy += extra;
    S->at_line_start = 1;
    S->pending_space = 0;
}
static void wl_place(struct wl_st *S,const char *buf,int len){
    struct wl_style *cs=&S->stk[S->sp];
    char upper_buf[512];
    if(cs->text_transform && len<511){
        int k; for(k=0;k<len;k++){ char c=buf[k]; upper_buf[k]=(c>='a'&&c<='z')?(char)(c-32):c; }
        upper_buf[len]=0; buf=upper_buf;
    }
    int px=cs->px;
    int hh = cs->line_height_pct>0 ? m_lh(px)*cs->line_height_pct/100 : m_lh(px);
    if(hh<m_lh(px)) hh=m_lh(px);
    int wpx=word_w(buf,len,px), gap, off;
    struct wl_run *r;
    if(len<=0) return;
    gap=(S->pending_space && !S->at_line_start)? m_adv(' ',px) : 0;
    if(!S->at_line_start && S->cx+gap+wpx > S->vw){ wl_newline(S); gap=0; }
    else S->cx += gap;
    S->pending_space=0;
    off=pool_append(S->d,buf,len);
    if(off<0) return;
    r=add_run(S->d);
    if(!r) return;
    r->kind=WL_TEXT; r->x=S->cx; r->y=S->cy; r->w=wpx; r->h=hh;
    r->px=px; r->bold=cs->bold; r->underline=cs->underline;
    r->italic=cs->italic; r->strikethrough=cs->strikethrough;
    r->color=cs->color; r->bg=cs->bg;
    r->off=off; r->len=len; r->link=cs->link;
    S->cx += wpx;
    S->at_line_start=0;
    if(hh>S->line_h) S->line_h=hh;
}

/* A list bullet: a small filled disc the browser draws (no text). */
static void wl_bullet(struct wl_st *S){
    struct wl_style *cs=&S->stk[S->sp];
    int px=cs->px, hh=m_lh(px);
    struct wl_run *r;
    wl_block(S,2);
    r=add_run(S->d);
    if(r){ r->kind=WL_BULLET; r->x=cs->left; r->y=S->cy; r->w=px; r->h=hh;
           r->px=px; r->bold=0; r->underline=0; r->color=cs->color; r->bg=0; r->off=0; r->len=0; r->link=-1; }
    S->cx=cs->left+px;
    S->at_line_start=0;
    S->line_h=hh;
    S->pending_space=0;
}
static void wl_rule(struct wl_st *S,wl_u32 color,int h){
    struct wl_run *r;
    int left=S->stk[S->sp].left;
    wl_block(S,6);
    r=add_run(S->d);
    if(r){ r->kind=WL_RULE; r->x=left; r->y=S->cy; r->w=S->vw-left; r->h=h;
           r->px=WL_BODY_PX; r->bold=0; r->underline=0; r->color=color; r->bg=0; r->off=0; r->len=0; r->link=-1; }
    S->cy += h+8;
    S->at_line_start=1;
}

static void wl_push(struct wl_st *S){ if(S->sp<31){ S->stk[S->sp+1]=S->stk[S->sp]; S->sp++; } }
static void wl_pop(struct wl_st *S){ if(S->sp>0) S->sp--; }

/* Apply a tag's style="" attribute (if any) to the just-pushed top style. */
static void apply_style_attr(const char *in,int j,int end,struct wl_st *S){
    char css[256];
    if(get_attr(in,j,end,"style",css,sizeof css)) apply_decls(css,&S->stk[S->sp]);
}

void wl_init(struct wl_doc *d){
    d->pool=0; d->pool_len=0; d->pool_cap=0;
    d->runs=0; d->run_count=0; d->run_cap=0;
    d->hrefs=0; d->href_count=0; d->href_cap=0;
    d->fields=0; d->field_count=0; d->field_cap=0;
    d->height=0;
}
void wl_free(struct wl_doc *d){
    if(d->pool) ufree(d->pool);
    if(d->runs) ufree(d->runs);
    if(d->hrefs) ufree(d->hrefs);
    if(d->fields) ufree(d->fields);
    wl_init(d);
}

int wl_layout(struct wl_doc *d,const char *in,int n,int vw){
    struct wl_st S;
    char word[512]; int wl=0;
    int i=0;

    /* reset */
    d->pool_len=0; d->run_count=0; d->href_count=0; d->height=0;
    S.d=d; S.vw=vw>64?vw:64; S.cx=0; S.cy=0; S.line_h=0; S.left=0;
    S.at_line_start=1; S.pending_space=0; S.sp=0;
    S.stk[0].px=WL_BODY_PX; S.stk[0].bold=0; S.stk[0].underline=0;
    S.stk[0].italic=0; S.stk[0].strikethrough=0;
    S.stk[0].color=COL_TEXT; S.stk[0].bg=0; S.stk[0].link=-1; S.stk[0].left=0;
    S.stk[0].padding_left=0; S.stk[0].padding_top=0; S.stk[0].padding_bottom=0;
    S.stk[0].text_align=0; S.stk[0].line_height_pct=0;
    S.stk[0].max_width=0; S.stk[0].border_color=0; S.stk[0].border_width=0;
    S.stk[0].text_transform=0; S.stk[0].list_style_none=0; S.stk[0].pos_fixed=0;
    S.line_start_run=0;

    while(i<n){
        char c=in[i];
        if(c=='<'){
            int j=i+1, closing=0, k=0, end; char name[16];
            if(wl>0){ wl_place(&S,word,wl); wl=0; }
            if(j<n&&in[j]=='/'){ closing=1; j++; }
            for(;j<n&&is_alnum(in[j])&&k<15;++j) name[k++]=in[j];
            name[k]=0;
            /* Find the tag's '>' but ignore any '>' inside quoted attribute
             * values (modern HTML has JS like :class="{...}>..." in attrs). */
            { char q=0; end=j;
              while(end<n){ char e=in[end];
                  if(q){ if(e==q) q=0; }
                  else if(e=='"'||e=='\'') q=e;
                  else if(e=='>') break;
                  end++; } }

            if(!closing&&(ieq(name,"script")||ieq(name,"style")||ieq(name,"head"))){
                i=skip_until_close(in,n,(end<n)?end+1:n,name);
                continue;
            }
            if(ieq(name,"br")){ wl_newline(&S); }
            else if(ieq(name,"hr")){ wl_rule(&S,COL_RULE,2); }
            else if(ieq(name,"h1")){
                if(!closing){ wl_block(&S,12); wl_push(&S); S.stk[S.sp].px=32; S.stk[S.sp].bold=1; S.stk[S.sp].color=COL_HEAD; apply_style_attr(in,j,end,&S); }
                else { wl_pop(&S); wl_rule(&S,COL_RULE,1); }   /* accent underline */
            }
            else if(ieq(name,"h2")){
                if(!closing){ wl_block(&S,10); wl_push(&S); S.stk[S.sp].px=26; S.stk[S.sp].bold=1; S.stk[S.sp].color=COL_HEAD; apply_style_attr(in,j,end,&S); }
                else { wl_pop(&S); wl_block(&S,8); }
            }
            else if(ieq(name,"h3")){
                if(!closing){ wl_block(&S,8); wl_push(&S); S.stk[S.sp].px=22; S.stk[S.sp].bold=1; S.stk[S.sp].color=COL_HEAD; apply_style_attr(in,j,end,&S); }
                else { wl_pop(&S); wl_block(&S,6); }
            }
            else if(ieq(name,"h4")||ieq(name,"h5")||ieq(name,"h6")){
                if(!closing){ wl_block(&S,6); wl_push(&S); S.stk[S.sp].bold=1; S.stk[S.sp].color=COL_HEAD; apply_style_attr(in,j,end,&S); }
                else { wl_pop(&S); wl_block(&S,6); }
            }
            else if(ieq(name,"b")||ieq(name,"strong")){
                if(!closing){ wl_push(&S); S.stk[S.sp].bold=1; apply_style_attr(in,j,end,&S); } else wl_pop(&S);
            }
            else if(ieq(name,"i")||ieq(name,"em")||ieq(name,"span")){
                if(!closing){ wl_push(&S); apply_style_attr(in,j,end,&S); } else wl_pop(&S);
            }
            else if(ieq(name,"font")){
                if(!closing){ char cv[64]; wl_push(&S);
                    if(get_attr(in,j,end,"color",cv,sizeof cv)){ wl_u32 c; if(parse_color(cv,&c)) S.stk[S.sp].color=c; }
                    apply_style_attr(in,j,end,&S);
                } else wl_pop(&S);
            }
            else if(ieq(name,"code")||ieq(name,"tt")||ieq(name,"kbd")||ieq(name,"samp")){
                if(!closing){ wl_push(&S); S.stk[S.sp].bg=COL_CODEBG; apply_style_attr(in,j,end,&S); } else wl_pop(&S);
            }
            else if(ieq(name,"pre")){
                if(!closing){ wl_block(&S,6); wl_push(&S); S.stk[S.sp].bg=COL_CODEBG; apply_style_attr(in,j,end,&S); }
                else { wl_pop(&S); wl_block(&S,6); }
            }
            else if(ieq(name,"a")){
                if(!closing){
                    char href[WL_HREF_MAX];
                    wl_push(&S);
                    S.stk[S.sp].color=COL_LINK; S.stk[S.sp].underline=1;
                    if(get_attr(in,j,end,"href",href,sizeof href)) S.stk[S.sp].link=add_href(d,href);
                    apply_style_attr(in,j,end,&S);
                } else wl_pop(&S);
            }
            else if(ieq(name,"ul")||ieq(name,"ol")){
                if(!closing){ wl_block(&S,4); wl_push(&S); S.stk[S.sp].left += 18; }
                else { wl_pop(&S); wl_block(&S,4); }
            }
            else if(ieq(name,"blockquote")){
                if(!closing){ wl_block(&S,4); wl_push(&S); S.stk[S.sp].left += 16; S.stk[S.sp].color=COL_QUOTE; }
                else { wl_pop(&S); wl_block(&S,4); }
            }
            else if(ieq(name,"li")){
                if(!closing) wl_bullet(&S);
            }
            else if(ieq(name,"p")||ieq(name,"div")||ieq(name,"table")||ieq(name,"tr")||
                    ieq(name,"section")||ieq(name,"article")||ieq(name,"header")||ieq(name,"footer")||
                    ieq(name,"nav")||ieq(name,"dl")||ieq(name,"dd")||ieq(name,"dt")||ieq(name,"figure")||
                    ieq(name,"figcaption")||ieq(name,"main")||ieq(name,"aside")){
                wl_block(&S,8);
            }
            i=(end<n)?end+1:n;
            continue;
        }
        if(c=='&'){
            int j=i+1; while(j<n&&in[j]!=';'&&j-i<=10&&in[j]!='<'&&!is_space(in[j])) j++;
            if(j<n&&in[j]==';'){
                char dec[4]; int dn=decode_entity(in+i+1,j-(i+1),dec);
                if(dn>0){ int q; for(q=0;q<dn;++q){ if(wl<(int)sizeof(word)-1) word[wl++]=dec[q]; } i=j+1; continue; }
            }
            if(wl<(int)sizeof(word)-1) word[wl++]='&';
            i++; continue;
        }
        if(is_space(c)){
            if(wl>0){ wl_place(&S,word,wl); wl=0; }
            S.pending_space=1;
            i++; continue;
        }
        if((unsigned char)c >= 0x80){
            /* Decode a UTF-8 sequence to a Unicode codepoint. Latin-1 range
             * (<=0xFF, incl. äöüÄÖÜß) is kept as a single byte that appfont
             * renders directly; anything higher degrades to '?'. */
            unsigned int cp; int len; unsigned char b=(unsigned char)c;
            if((b&0xE0)==0xC0 && i+1<n){ cp=((b&0x1Fu)<<6)|(in[i+1]&0x3Fu); len=2; }
            else if((b&0xF0)==0xE0 && i+2<n){ cp=((b&0x0Fu)<<12)|((in[i+1]&0x3Fu)<<6)|(in[i+2]&0x3Fu); len=3; }
            else if((b&0xF8)==0xF0 && i+3<n){ cp=((b&0x07u)<<18)|((in[i+1]&0x3Fu)<<12)|((in[i+2]&0x3Fu)<<6)|(in[i+3]&0x3Fu); len=4; }
            else { cp=b; len=1; }
            if(wl<(int)sizeof(word)-1) word[wl++]=(cp<=0xFFu)?(char)cp:'?';
            i+=len; continue;
        }
        if(wl<(int)sizeof(word)-1) word[wl++]=c;
        else { wl_place(&S,word,wl); wl=0; if(wl<(int)sizeof(word)-1) word[wl++]=c; }
        i++;
    }
    if(wl>0) wl_place(&S,word,wl);
    if(!S.at_line_start) S.cy += (S.line_h>0?S.line_h:m_lh(WL_BODY_PX));
    d->height=S.cy;
    return d->height;
}

/* ===================================================================== *
 *  DOM-tree layout — the proper path: walk a parsed dom_node tree and    *
 *  produce positioned runs. Reuses the same wl_* machinery as the legacy *
 *  byte parser above; element open/close is explicit (before/after the   *
 *  children) so nesting and auto-closing are already handled by the DOM. *
 * ===================================================================== */

/* Compute a node's style: CSS cascade from the sheet first (ascending
 * specificity), then the element's inline style="" (highest priority). */
static void dom_apply_style(struct dom_node *node, struct wl_st *S){
    if(S->sheet){ char buf[640]; css_match(S->sheet, node, buf, sizeof buf); apply_decls(buf, &S->stk[S->sp]); }
    { const char *css = dom_attr(node, "style"); if(css) apply_decls(css, &S->stk[S->sp]); }
}

/* Probe whether CSS/inline sets display:none for this node (without mutating
 * the live style stack), so layout_node can skip the whole subtree. */
static int dom_is_hidden(struct dom_node *node, struct wl_st *S){
    struct wl_style probe = S->stk[S->sp];
    probe.hidden = 0; probe.pos_fixed = 0;
    if(S->sheet){ char buf[640]; css_match(S->sheet, node, buf, sizeof buf); apply_decls(buf, &probe); }
    { const char *css = dom_attr(node, "style"); if(css) apply_decls(css, &probe); }
    return probe.hidden || probe.pos_fixed;
}

/* Place already-decoded (collapsed) text word by word. */
static void layout_text_run(struct wl_st *S, const char *t){
    int i=0;
    if(t && t[0]==' ') S->pending_space=1;
    while(t && t[i]){
        int s;
        if(t[i]==' '){ S->pending_space=1; ++i; continue; }
        s=i; while(t[i] && t[i]!=' ') ++i;
        wl_place(S, t+s, i-s);
    }
}

static void layout_node(struct wl_st *S, struct dom_node *node);

/* Block background: emit a WL_RECT placeholder BEFORE the block's children (so
 * it paints behind them), remembering its run index; patch its height after the
 * children when the final cy is known. Returns the run index, or -1 if no bg. */
static int block_bg_begin(struct wl_st *S){
    struct wl_run *r;
    int idx;
    wl_u32 bg = S->stk[S->sp].bg;
    if(!bg) return -1;
    if(!S->at_line_start) wl_newline(S);
    r = add_run(S->d);
    if(!r) return -1;
    idx = S->d->run_count - 1;
    r->kind=WL_RECT; r->x=S->stk[S->sp].left; r->y=S->cy-2; r->w=S->vw - S->stk[S->sp].left; r->h=0;
    r->px=0; r->bold=0; r->underline=0; r->color=bg; r->bg=0; r->off=0; r->len=0; r->link=-1;
    S->stk[S->sp].bg = 0;   /* don't also paint per-text-run backgrounds inside */
    return idx;
}
static void block_bg_end(struct wl_st *S, int idx){
    if(idx < 0 || idx >= S->d->run_count) return;
    if(!S->at_line_start) { /* include the trailing line */ }
    { struct wl_run *r=&S->d->runs[idx]; r->h = (S->cy + 4) - r->y; if(r->h<0) r->h=0; }
}

static void layout_children(struct wl_st *S, struct dom_node *node){
    struct dom_node *c;
    for(c=node->first_child; c; c=c->next_sibling) layout_node(S, c);
}

static void layout_node(struct wl_st *S, struct dom_node *node){
    const char *t;
    if(node->type==DOM_TEXT){ layout_text_run(S, node->text); return; }

    t = node->tag;
    /* skip non-rendered subtrees */
    if(ieq(t,"head")||ieq(t,"script")||ieq(t,"style")||ieq(t,"title")||
       ieq(t,"meta")||ieq(t,"link")||ieq(t,"noscript")) return;
    /* CSS display:none (or inline) hides the element and its whole subtree */
    if(dom_is_hidden(node, S)) return;

    if(ieq(t,"br")){ wl_newline(S); return; }
    if(ieq(t,"hr")){ wl_rule(S,COL_RULE,2); return; }
    if(ieq(t,"img")){
        const char *src = dom_attr(node,"src");
        if((!src || !src[0]) || (src[0]=='d'&&src[1]=='a'&&src[2]=='t'&&src[3]=='a'&&src[4]==':')) src = dom_attr(node,"data-src");
        int iw=0, ih=0, maxw = S->vw - S->stk[S->sp].left;
        if(src && src[0] && g_img_fn && g_img_fn(src, maxw, &iw, &ih) && iw>0 && ih>0){
            /* reserve a box and emit a WL_IMAGE run (src kept in hrefs) */
            struct wl_run *r;
            int hi = add_href(S->d, src);
            if(!S->at_line_start) wl_newline(S);
            r = add_run(S->d);
            if(r){ r->kind=WL_IMAGE; r->x=S->stk[S->sp].left; r->y=S->cy; r->w=iw; r->h=ih;
                   r->px=0; r->bold=0; r->underline=0; r->color=0; r->bg=0; r->off=hi; r->len=0; r->link=-1; }
            S->cy += ih + 4; S->at_line_start=1; S->line_h=0;
        } else {
            /* unknown image (not yet loaded / unsupported): alt-text placeholder */
            const char *alt = dom_attr(node,"alt");
            if(alt && alt[0]){ wl_push(S); S->stk[S->sp].color=COL_QUOTE; layout_text_run(S,"["); layout_text_run(S,alt); layout_text_run(S,"]"); wl_pop(S); }
        }
        return;
    }

    if(ieq(t,"h1")){ wl_block(S,12); wl_push(S); S->stk[S->sp].px=32; S->stk[S->sp].bold=1; S->stk[S->sp].color=COL_HEAD; dom_apply_style(node,S);
        layout_children(S,node); wl_pop(S); wl_rule(S,COL_RULE,1); return; }
    if(ieq(t,"h2")){ wl_block(S,10); wl_push(S); S->stk[S->sp].px=26; S->stk[S->sp].bold=1; S->stk[S->sp].color=COL_HEAD; dom_apply_style(node,S);
        layout_children(S,node); wl_pop(S); wl_block(S,8); return; }
    if(ieq(t,"h3")){ wl_block(S,8); wl_push(S); S->stk[S->sp].px=22; S->stk[S->sp].bold=1; S->stk[S->sp].color=COL_HEAD; dom_apply_style(node,S);
        layout_children(S,node); wl_pop(S); wl_block(S,6); return; }
    if(ieq(t,"h4")||ieq(t,"h5")||ieq(t,"h6")){ wl_block(S,6); wl_push(S); S->stk[S->sp].bold=1; S->stk[S->sp].color=COL_HEAD; dom_apply_style(node,S);
        layout_children(S,node); wl_pop(S); wl_block(S,6); return; }
    if(ieq(t,"b")||ieq(t,"strong")){ wl_push(S); S->stk[S->sp].bold=1; dom_apply_style(node,S); layout_children(S,node); wl_pop(S); return; }
    if(ieq(t,"i")||ieq(t,"em")){ wl_push(S); S->stk[S->sp].italic=1; dom_apply_style(node,S); layout_children(S,node); wl_pop(S); return; }
    if(ieq(t,"span")){ wl_push(S); dom_apply_style(node,S); layout_children(S,node); wl_pop(S); return; }
    if(ieq(t,"font")){ const char *cv; wl_push(S); cv=dom_attr(node,"color"); if(cv){ wl_u32 col; if(parse_color(cv,&col)) S->stk[S->sp].color=col; } dom_apply_style(node,S);
        layout_children(S,node); wl_pop(S); return; }
    if(ieq(t,"code")||ieq(t,"tt")||ieq(t,"kbd")||ieq(t,"samp")){ wl_push(S); S->stk[S->sp].bg=COL_CODEBG; dom_apply_style(node,S); layout_children(S,node); wl_pop(S); return; }
    if(ieq(t,"pre")){ wl_block(S,6); wl_push(S); S->stk[S->sp].bg=COL_CODEBG; dom_apply_style(node,S); layout_children(S,node); wl_pop(S); wl_block(S,6); return; }
    if(ieq(t,"a")){ const char *href; wl_push(S); S->stk[S->sp].color=COL_LINK; S->stk[S->sp].underline=1;
        href=dom_attr(node,"href"); if(href){ int hi=add_href(S->d,href); S->stk[S->sp].link=hi; }
        dom_apply_style(node,S); layout_children(S,node); wl_pop(S); return; }
    if(ieq(t,"ul")||ieq(t,"ol")){ wl_block(S,4); wl_push(S); S->stk[S->sp].left += 18; dom_apply_style(node,S); layout_children(S,node); wl_pop(S); wl_block(S,4); return; }
    if(ieq(t,"blockquote")){ wl_block(S,4); wl_push(S); S->stk[S->sp].left += 16; S->stk[S->sp].color=COL_QUOTE; dom_apply_style(node,S); layout_children(S,node); wl_pop(S); wl_block(S,4); return; }
    if(ieq(t,"li")){
        wl_push(S); dom_apply_style(node,S);
        if(!S->stk[S->sp].list_style_none) wl_bullet(S);
        layout_children(S,node); wl_pop(S); return; }

    /* generic block-level containers */
    if(ieq(t,"p")||ieq(t,"div")||ieq(t,"table")||ieq(t,"tr")||ieq(t,"section")||
       ieq(t,"article")||ieq(t,"header")||ieq(t,"footer")||ieq(t,"nav")||ieq(t,"dl")||
       ieq(t,"dd")||ieq(t,"dt")||ieq(t,"figure")||ieq(t,"figcaption")||ieq(t,"main")||
       ieq(t,"aside")||ieq(t,"body")){
        int bg, pt, pb, pl;
        wl_block(S,8); wl_push(S); dom_apply_style(node,S);
        pt = S->stk[S->sp].padding_top > 0 ? S->stk[S->sp].padding_top : 0;
        pb = S->stk[S->sp].padding_bottom > 0 ? S->stk[S->sp].padding_bottom : 0;
        pl = S->stk[S->sp].padding_left > 0 ? S->stk[S->sp].padding_left : 0;
        if(pl > 80) pl = 80;
        bg=block_bg_begin(S);
        if(pt > 0) S->cy += pt;
        if(pl > 0){ S->stk[S->sp].left += pl; if(S->at_line_start) S->cx = S->stk[S->sp].left; }
        layout_children(S,node);
        if(!S->at_line_start) wl_newline(S);
        if(pb > 0) S->cy += pb;
        block_bg_end(S,bg);
        wl_pop(S); wl_block(S,8); return;
    }
    if((ieq(t,"td")||ieq(t,"th"))){ wl_push(S); dom_apply_style(node,S); layout_children(S,node); wl_pop(S); S->pending_space=1; return; }

    /* unknown / inline-ish: just recurse (html, u, small, etc.) */
    wl_push(S); dom_apply_style(node,S); layout_children(S,node); wl_pop(S);
}

/* Concatenate the text of every <style> element in the tree into out[]. */
static int collect_styles(struct dom_node *node, char *out, int cap, int pos){
    struct dom_node *c;
    if(!node) return pos;
    for(c=node->first_child; c; c=c->next_sibling){
        if(c->type==DOM_ELEMENT && ieq(c->tag,"style")){
            struct dom_node *tx;
            for(tx=c->first_child; tx; tx=tx->next_sibling)
                if(tx->type==DOM_TEXT && tx->text){ const char *s=tx->text; while(*s && pos+1<cap) out[pos++]=*s++; if(pos+1<cap)out[pos++]='\n'; }
        }
        pos = collect_styles(c, out, cap, pos);
    }
    return pos;
}

int wl_layout_dom(struct wl_doc *d, struct dom_node *root, int vw){
    struct wl_st S;
    char *cssbuf;
    int csslen = 0;
    d->pool_len=0; d->run_count=0; d->href_count=0; d->height=0;
    S.d=d; S.vw=vw>64?vw:64; S.cx=0; S.cy=0; S.line_h=0; S.left=0;
    S.at_line_start=1; S.pending_space=0; S.sp=0;
    S.stk[0].px=WL_BODY_PX; S.stk[0].bold=0; S.stk[0].underline=0;
    S.stk[0].italic=0; S.stk[0].strikethrough=0;
    S.stk[0].color=COL_TEXT; S.stk[0].bg=0; S.stk[0].link=-1; S.stk[0].left=0; S.stk[0].hidden=0;
    S.stk[0].padding_left=0; S.stk[0].padding_top=0; S.stk[0].padding_bottom=0;
    S.stk[0].text_align=0; S.stk[0].line_height_pct=0;
    S.stk[0].max_width=0; S.stk[0].border_color=0; S.stk[0].border_width=0;
    S.stk[0].text_transform=0; S.stk[0].list_style_none=0; S.stk[0].pos_fixed=0;
    S.line_start_run=0;
    S.sheet=0;

    /* Stage 2: gather <style> sheets and build the cascade. */
    cssbuf = (char*)umalloc(64*1024);
    if(cssbuf){ csslen = collect_styles(root, cssbuf, 64*1024, 0); cssbuf[csslen<64*1024?csslen:64*1024-1]=0;
                if(csslen>0) S.sheet = css_parse(cssbuf, csslen); }

    if(root) layout_children(&S, root);
    if(!S.at_line_start) S.cy += (S.line_h>0?S.line_h:m_lh(WL_BODY_PX));
    d->height=S.cy;

    if(S.sheet) css_free(S.sheet);
    if(cssbuf) ufree(cssbuf);
    return d->height;
}
