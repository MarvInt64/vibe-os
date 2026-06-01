/* browser — graphical web reader for VibeOS (Phase 1: flow-layout renderer).
 *
 * Fetches HTTP/HTTPS, follows redirects, de-chunks, and renders via the
 * weblayout engine: headings, bold, links (clickable), bullets, rules — real
 * inline/block flow with pixel-precise scrolling. Built on the VibeOS libc;
 * window-server + networking via vibeos.h. */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <vibeos.h>

#include "vexui_font.h"
#include "umalloc.h"
#include "weblayout.h"
#include "appfont.h"
#include "dom.h"
#include "appimage.h"

#define WIN_INIT_W 560
#define WIN_INIT_H 340
#define WIN_MAX_W 900
#define WIN_MAX_H 640
#define WIN_W g_win_w
#define WIN_H g_win_h
#define ADV 8
#define LINEH 16
#define BAR_H 20
#define STATUS_Y (BAR_H + 2)
#define CONTENT_TOP (BAR_H + 20)
#define MARGIN 4
#define SCROLLBAR_W 4
#define PAGE_PAD 10
#define PAGE_X MARGIN
#define PAGE_W (WIN_W - 2*MARGIN - SCROLLBAR_W)
#define CONTENT_X (PAGE_X + PAGE_PAD)
#define CONTENT_W (PAGE_W - 2*PAGE_PAD)

#define COL_BG     0x00101620u   /* desktop behind the page */
#define COL_BAR    0x001d2740u
#define COL_BAR_HI 0x0027406bu
#define COL_TEXT   0x00d6e2f2u
#define COL_DIM    0x008396adu
#define COL_ACCENT 0x0064f2ccu
#define COL_PAGE   0x00f7f8fcu   /* the rendered web page (light paper) */
#define COL_HOVER  0x00d8e4ffu   /* link hover highlight */

static uint32_t g_canvas[WIN_MAX_W * WIN_MAX_H];
static int g_win;
static int g_win_w = WIN_INIT_W;
static int g_win_h = WIN_INIT_H;

static char g_url[1024];
static int  g_url_len;
static int  g_editing = 1;
static char g_status[96];
static struct wl_doc g_doc;
static char *g_html;
static int g_html_len;
static int  g_scroll;            /* pixel scroll offset */
static int  g_hover_link = -1;   /* href index currently under the cursor */

/* base of the currently displayed page, for resolving relative links */
static int  g_cur_secure;
static char g_cur_host[256];
static char g_cur_path[512];

/* Log to BOTH the controlling terminal (stdout, inherited from the shell that
 * launched us) and the kernel journal (visible via `dmesg`). */
static void blog(const char *m){ printf("[browser] %s\n", m); vos_log(VOS_LOG_APP, m); }

/* ---- string helpers ---- */
static void sset(char *dst,int cap,const char *s){ int i=0; for(;s&&s[i]&&i<cap-1;i++)dst[i]=s[i]; dst[i]=0; }
static int ci_starts(const char *s,const char *pfx){
    for(;*pfx;++s,++pfx){ char a=*s,b=*pfx; if(a>='A'&&a<='Z')a+=32; if(b>='A'&&b<='Z')b+=32; if(a!=b) return 0; }
    return 1;
}

/* ---- canvas drawing ---- */
static void fill(int x,int y,int w,int h,uint32_t c){
    int iy,ix;
    for(iy=y;iy<y+h;++iy){ if(iy<0||iy>=WIN_H)continue;
        for(ix=x;ix<x+w;++ix){ if(ix<0||ix>=WIN_W)continue; g_canvas[iy*WIN_W+ix]=c; } }
}
static void put_c(int x,int y,uint32_t c,int top){ if(x<0||y<top||x>=WIN_W||y>=WIN_H)return; g_canvas[y*WIN_W+x]=c; }
/* scaled glyph with optional faux-bold, clipped to y>=top */
static void glyph_s(int x,int y,char ch,uint32_t c,int scale,int bold,int top){
    const uint8_t *g=glyph_for_char(ch); int r,col,sx,sy;
    for(r=0;r<16;++r){ uint8_t b=g[r];
        for(col=0;col<8;++col){ if(!((b>>(7-col))&1u)) continue;
            for(sy=0;sy<scale;++sy) for(sx=0;sx<scale;++sx){
                put_c(x+col*scale+sx, y+r*scale+sy, c, top);
                if(bold) put_c(x+col*scale+sx+1, y+r*scale+sy, c, top);
            }
        }
    }
}
/* unscaled glyph for the chrome (URL bar / status) */
static void glyph(int x,int y,char ch,uint32_t c){ glyph_s(x,y,ch,c,1,0,0); }
static int text_n(int x,int y,const char *s,int n,uint32_t c){
    int i; for(i=0;i<n&&s[i];++i){ glyph(x,y,s[i],c); x+=ADV; } return i;
}
static void draw_text(int x,int y,const char *s,uint32_t c){ text_n(x,y,s,1<<20,c); }
static void present(void){ vos_window_present(g_win, g_canvas, WIN_W, WIN_H); }

/* ---- networking + HTTP parsing ---- */
struct vos_http_req;   /* from vibeos.h */
static int parse_ipv4(const char *s,uint32_t *out){
    uint32_t parts[4]; int pi=0,have=0; uint32_t v=0;
    for(;;s++){
        if(*s>='0'&&*s<='9'){ v=v*10u+(uint32_t)(*s-'0'); have=1; if(v>255)return 0; }
        else if(*s=='.'||*s==0){ if(!have||pi>3)return 0; parts[pi++]=v; v=0; have=0; if(*s==0)break; }
        else return 0;
    }
    if(pi!=4)return 0;
    *out=(parts[0]<<24)|(parts[1]<<16)|(parts[2]<<8)|parts[3];
    return 1;
}
static int http_status(const char *raw,int n){
    int i=0,code=0,d=0;
    while(i<n && raw[i]!=' ' && raw[i]!='\n') i++;
    while(i<n && raw[i]==' ') i++;
    while(i<n && raw[i]>='0' && raw[i]<='9'){ code=code*10+(raw[i]-'0'); i++; d++; }
    return d? code : 0;
}
static int find_header(const char *raw,int n,const char *name,char *out,int cap){
    int i=0, nl=0; while(name[nl]) nl++;
    while(i<n && raw[i]!='\n') i++; i++;
    while(i<n){
        if(raw[i]=='\r'||raw[i]=='\n') break;
        if(ci_starts(raw+i,name)){
            int j=i+nl, o=0;
            while(j<n && (raw[j]==' '||raw[j]=='\t')) j++;
            while(j<n && raw[j]!='\r' && raw[j]!='\n' && o+1<cap) out[o++]=raw[j++];
            out[o]=0; return o>0;
        }
        while(i<n && raw[i]!='\n') i++; i++;
    }
    return 0;
}
static int hexval(char c){ if(c>='0'&&c<='9')return c-'0'; if(c>='a'&&c<='f')return c-'a'+10; if(c>='A'&&c<='F')return c-'A'+10; return -1; }
static int dechunk(char *b,int blen){
    int ri=0, wi=0;
    while(ri<blen){
        int size=0,any=0,hv;
        while(ri<blen && (hv=hexval(b[ri]))>=0){ size=size*16+hv; ri++; any=1; }
        while(ri<blen && b[ri]!='\n') ri++; if(ri<blen) ri++;
        if(!any || size<=0) break;
        if(ri+size>blen) size=blen-ri;
        if(size<=0) break;
        memmove(b+wi,b+ri,(size_t)size); wi+=size; ri+=size;
        while(ri<blen && (b[ri]=='\r'||b[ri]=='\n')) ri++;
    }
    return wi;
}
static int resp_is_chunked(const char *raw,int n){
    char te[64]; int i;
    if(!find_header(raw,n,"transfer-encoding:",te,sizeof te)) return 0;
    for(i=0; te[i]; ++i) if(ci_starts(te+i,"chunked")) return 1;
    return 0;
}

static int content_h(void){ return WIN_H - CONTENT_TOP; }
static void clamp_scroll(void){
    int max=g_doc.height - content_h(); if(max<0)max=0;
    if(g_scroll>max)g_scroll=max;
    if(g_scroll<0)g_scroll=0;
}

/* Resolve href (possibly relative) against the current page base into out. */
static void resolve_href(const char *href,char *out,int cap){
    const char *scheme = g_cur_secure?"https":"http";
    if(ci_starts(href,"http://")||ci_starts(href,"https://")){ sset(out,cap,href); return; }
    if(href[0]=='#'){ out[0]=0; return; }                 /* in-page anchor: ignore */
    if(href[0]=='/'&&href[1]=='/'){ snprintf(out,cap,"%s:%s",scheme,href); return; }
    if(href[0]=='/'){ snprintf(out,cap,"%s://%s%s",scheme,g_cur_host,href); return; }
    {   /* relative to the directory of the current path */
        int dir=0,i; for(i=0; g_cur_path[i]; ++i) if(g_cur_path[i]=='/') dir=i+1;
        snprintf(out,cap,"%s://%s%.*s%s",scheme,g_cur_host,dir,g_cur_path,href);
    }
}

static void images_clear(void);
static void images_collect(struct dom_node *node);

static void layout_html_current(int load_images){
    struct dom_doc dom;
    if(!g_html || g_html_len<=0) return;
    dom_init(&dom);
    if(!dom_parse(&dom, g_html, g_html_len) || !dom.root){
        wl_init(&g_doc);
        dom_free(&dom);
        sset(g_status,sizeof g_status,"out of memory while parsing page");
        g_scroll=0;
        return;
    }
    if(load_images){
        images_clear();
        images_collect(dom.root);
    }
    wl_layout_dom(&g_doc, dom.root, CONTENT_W);
    dom_free(&dom);
    clamp_scroll();
}

static void load_url(void){
    static char cur[1024];
    static char host[256];
    static char path[512];
    static char loc[1024];
    int redir;

    if(g_url[0]==0){ sset(g_status,sizeof g_status,"enter a URL"); return; }
    sset(cur,sizeof cur,g_url);

    for(redir=0; redir<6; ++redir){
        const char *u=cur;
        uint32_t ip;
        int h=0,p=0,i=0,n,cap,bodyoff,secure=0,code;
        char *raw;
        struct vos_http_req req;

        if(ci_starts(u,"https://")){ secure=1; u+=8; }
        else if(ci_starts(u,"http://")){ u+=7; }

        h=0; while(u[i]&&u[i]!='/'&&h<(int)sizeof(host)-1) host[h++]=u[i++]; host[h]=0;
        p=0; path[p++]='/';
        if(u[i]=='/'){ i++; while(u[i]&&p<(int)sizeof(path)-1) path[p++]=u[i++]; }
        path[p]=0;

        if(!parse_ipv4(host,&ip)){
            snprintf(g_status,sizeof g_status,"resolving %s ...",host); present();
            if(vos_resolve(host,&ip)<0){ sset(g_status,sizeof g_status,"DNS: cannot resolve host"); return; }
        }

        cap=512*1024;
        raw=(char*)malloc((size_t)cap+1);
        if(!raw){ sset(g_status,sizeof g_status,"out of memory"); return; }

        req.ip=ip; req.port=secure?443:80; req.host=host; req.path=path; req.out=raw; req.cap=cap;
        snprintf(g_status,sizeof g_status,"%s %s ...", secure?"TLS":"HTTP", host); present();
        blog(cur);
        n = secure ? vos_https_get(&req) : vos_http_get(&req);
        if(n<=0){ free(raw); blog(secure?"TLS request failed":"HTTP request failed");
                  sset(g_status,sizeof g_status, secure?"TLS request failed":"request failed"); return; }
        raw[n]=0;

        code=http_status(raw,n);
        if(code>=300 && code<400 && find_header(raw,n,"location:",loc,sizeof loc)){
            if(ci_starts(loc,"http://")||ci_starts(loc,"https://")) sset(cur,sizeof cur,loc);
            else if(loc[0]=='/') snprintf(cur,sizeof cur,"%s%s%s", secure?"https://":"http://", host, loc);
            else snprintf(cur,sizeof cur,"%s%s/%s", secure?"https://":"http://", host, loc);
            sset(g_url,sizeof g_url,cur); g_url_len=(int)strlen(g_url);
            free(raw);
            continue;
        }

        bodyoff=0;
        { int k; for(k=0;k+1<n;++k){
            if(k+3<n&&raw[k]=='\r'&&raw[k+1]=='\n'&&raw[k+2]=='\r'&&raw[k+3]=='\n'){ bodyoff=k+4; break; }
            if(raw[k]=='\n'&&raw[k+1]=='\n'){ bodyoff=k+2; break; } } }

        /* record base for relative-link / image-src resolution BEFORE loading
         * images (image_load resolves relative srcs against it) */
        g_cur_secure=secure; sset(g_cur_host,sizeof g_cur_host,host); sset(g_cur_path,sizeof g_cur_path,path);

        {
            int blen = n - bodyoff;
            if(resp_is_chunked(raw,n)) blen = dechunk(raw+bodyoff, blen);
            if(g_html){ free(g_html); g_html=0; g_html_len=0; }
            g_html=(char*)malloc((size_t)blen+1);
            if(!g_html){ free(raw); sset(g_status,sizeof g_status,"out of memory"); return; }
            memcpy(g_html, raw+bodyoff, (size_t)blen);
            g_html[blen]=0;
            g_html_len=blen;
            layout_html_current(1);
        }
        free(raw);
        g_scroll=0;

        snprintf(g_status,sizeof g_status,"%d  %d runs  %luK heap",
                 code, g_doc.run_count, (unsigned long)(umalloc_used()/1024));
        blog(g_status);
        g_editing=0;
        return;
    }
    sset(g_status,sizeof g_status,"too many redirects");
}

/* ---- anti-aliased text via appfont ---- */
static uint32_t blend(uint32_t dst,uint32_t src,int a){
    int dr=(dst>>16)&255,dg=(dst>>8)&255,db=dst&255;
    int sr=(src>>16)&255,sg=(src>>8)&255,sb=src&255;
    int r=(sr*a+dr*(255-a))/255, g=(sg*a+dg*(255-a))/255, b=(sb*a+db*(255-a))/255;
    return ((uint32_t)r<<16)|((uint32_t)g<<8)|(uint32_t)b;
}
static void blit_glyph(const struct af_glyph *g,int ox,int oy,uint32_t color,int top){
    int yy,xx;
    if(!g||!g->cov) return;
    for(yy=0;yy<g->h;++yy){ int py=oy+yy; if(py<top||py>=WIN_H) continue;
        for(xx=0;xx<g->w;++xx){ int a=g->cov[yy*g->w+xx]; int pxp=ox+xx;
            if(!a||pxp<0||pxp>=WIN_W) continue;
            { uint32_t *d=&g_canvas[py*WIN_W+pxp]; *d=blend(*d,color,a); } } }
}
/* draw a run's text at content (rx,sy); returns end pen x */
static void draw_run_text(int rx,int sy,const struct wl_run *r){
    int pen=rx, k, base=sy+appfont_ascent(r->px);
    for(k=0;k<r->len;++k){
        unsigned char cp=(unsigned char)g_doc.pool[r->off+k];
        const struct af_glyph *g=appfont_get(cp,r->px);
        if(g){
            blit_glyph(g,pen+g->xoff,base+g->yoff,r->color,CONTENT_TOP);
            if(r->bold) blit_glyph(g,pen+g->xoff+1,base+g->yoff,r->color,CONTENT_TOP);
            pen+=g->advance;
        } else pen+=r->px/2;
    }
}

/* ===================== images ===================== */
#define MAX_IMG 12
struct img_entry { char src[256]; unsigned int *px; int w, h; };
static struct img_entry g_imgs[MAX_IMG];
static int g_nimg;

static void images_clear(void){
    int i; for(i=0;i<g_nimg;++i){ if(g_imgs[i].px){ appimage_free(g_imgs[i].px); g_imgs[i].px=0; } }
    g_nimg=0;
}

/* Single GET into out; returns response length, *bodyoff set. */
static int fetch_raw(const char *url, char *out, int cap, int *bodyoff){
    static char host[256], path[512];
    const char *u=url; uint32_t ip; int h=0,p=0,i=0,n,secure=0;
    struct vos_http_req req;
    *bodyoff=0;
    if(ci_starts(u,"https://")){ secure=1; u+=8; }
    else if(ci_starts(u,"http://")){ u+=7; }
    while(u[i]&&u[i]!='/'&&h<(int)sizeof(host)-1) host[h++]=u[i++]; host[h]=0;
    path[0]='/'; p=1; if(u[i]=='/'){ i++; while(u[i]&&p<(int)sizeof(path)-1) path[p++]=u[i++]; } path[p]=0;
    if(!parse_ipv4(host,&ip)){ if(vos_resolve(host,&ip)<0) return -1; }
    req.ip=ip; req.port=secure?443:80; req.host=host; req.path=path; req.out=out; req.cap=cap;
    n = secure ? vos_https_get(&req) : vos_http_get(&req);
    if(n<=0) return -1;
    { int k; for(k=0;k+1<n;++k){
        if(k+3<n&&out[k]=='\r'&&out[k+1]=='\n'&&out[k+2]=='\r'&&out[k+3]=='\n'){ *bodyoff=k+4; break; }
        if(out[k]=='\n'&&out[k+1]=='\n'){ *bodyoff=k+2; break; } } }
    return n;
}

static void resolve_location_against(const char *base, const char *loc, char *out, int cap){
    char host[256], path[512];
    const char *u=base, *scheme="http";
    int secure=0,h=0,p=0,i=0,dir=0;
    if(ci_starts(loc,"http://")||ci_starts(loc,"https://")){ sset(out,cap,loc); return; }
    if(ci_starts(base,"https://")){ secure=1; scheme="https"; u+=8; }
    else if(ci_starts(base,"http://")){ u+=7; }
    while(u[i]&&u[i]!='/'&&h<(int)sizeof(host)-1) host[h++]=u[i++]; host[h]=0;
    path[0]='/'; p=1; if(u[i]=='/'){ i++; while(u[i]&&p<(int)sizeof(path)-1) path[p++]=u[i++]; } path[p]=0;
    if(loc[0]=='/'&&loc[1]=='/'){ snprintf(out,cap,"%s:%s",scheme,loc); return; }
    if(loc[0]=='/'){ snprintf(out,cap,"%s://%s%s",scheme,host,loc); return; }
    for(i=0; path[i]; ++i) if(path[i]=='/') dir=i+1;
    snprintf(out,cap,"%s://%s%.*s%s",scheme,host,dir,path,loc);
    (void)secure;
}

/* Fetch + decode one image into the cache. Returns index or -1. */
static int image_load(const char *rawsrc){
    static char abs_url[1024];
    static char cur[1024], loc[1024], next[1024];
    char *raw; int n=-1, bodyoff=0, blen, w=0, hh=0, code=0, redir; unsigned int *px;
    if(g_nimg>=MAX_IMG) return -1;
    resolve_href(rawsrc, abs_url, sizeof abs_url);
    if(!abs_url[0] || ci_starts(abs_url,"data:")) return -1;   /* no data: URIs yet */
    raw=(char*)malloc(512*1024);
    if(!raw) return -1;
    sset(cur,sizeof cur,abs_url);
    for(redir=0; redir<5; ++redir){
        n=fetch_raw(cur, raw, 512*1024, &bodyoff);
        if(n<=0){ free(raw); return -1; }
        code=http_status(raw,n);
        if(code>=300 && code<400 && find_header(raw,n,"location:",loc,sizeof loc)){
            resolve_location_against(cur, loc, next, sizeof next);
            sset(cur,sizeof cur,next);
            continue;
        }
        break;
    }
    if(code>=300 && code<400){ free(raw); return -1; }
    blen = n - bodyoff;
    if(resp_is_chunked(raw,n)) blen = dechunk(raw+bodyoff, blen);
    px=appimage_decode((const unsigned char*)(raw+bodyoff), blen, &w, &hh);
    free(raw);
    if(!px || w<=0 || hh<=0){
        blog("image decode failed");
        return -1;
    }
    snprintf(g_status,sizeof g_status,"loaded image %dx%d",w,hh);
    blog(g_status);
    { struct img_entry *e=&g_imgs[g_nimg]; sset(e->src,sizeof e->src,rawsrc); e->px=px; e->w=w; e->h=hh; }
    return g_nimg++;
}

/* Collect <img src> from the DOM (up to MAX_IMG) and load them. */
static void images_collect(struct dom_node *node){
    struct dom_node *c;
    if(!node || g_nimg>=MAX_IMG) return;
    for(c=node->first_child; c && g_nimg<MAX_IMG; c=c->next_sibling){
        if(c->type==DOM_ELEMENT && !__builtin_strcmp(c->tag,"img")){
            const char *src=dom_attr(c,"src");
            if((!src || !src[0]) || ci_starts(src,"data:")) src=dom_attr(c,"data-src");
            if(src && src[0]){
                int dup=0,k; for(k=0;k<g_nimg;k++) if(!__builtin_strcmp(g_imgs[k].src,src)){dup=1;break;}
                if(!dup){ snprintf(g_status,sizeof g_status,"loading image %d ...",g_nimg+1); present(); image_load(src); }
            }
        }
        images_collect(c);
    }
}

/* Image sizer for the layout: aspect-fit a decoded image into maxw. */
static int img_sizer(const char *src, int maxw, int *w, int *h){
    int i;
    for(i=0;i<g_nimg;++i){ if(!__builtin_strcmp(g_imgs[i].src,src) && g_imgs[i].px){
        int iw=g_imgs[i].w, ih=g_imgs[i].h;
        if(iw>maxw && maxw>0){ ih = (int)((long)ih*maxw/iw); iw=maxw; }
        if(ih<1)ih=1; *w=iw; *h=ih; return 1; } }
    return 0;
}

/* Find a decoded image by src (href). */
static struct img_entry *image_by_src(const char *src){
    int i; for(i=0;i<g_nimg;++i) if(!__builtin_strcmp(g_imgs[i].src,src)) return &g_imgs[i]; return 0;
}

/* ---- rendering ---- */
static void render(void){
    int barcols=(WIN_W-2*MARGIN-2*ADV)/ADV;
    int i;

    fill(0,0,WIN_W,WIN_H,COL_BG);

    /* URL bar */
    fill(0,0,WIN_W,BAR_H,g_editing?COL_BAR_HI:COL_BAR);
    draw_text(MARGIN,2,">",COL_ACCENT);
    { int start=0; if(g_url_len>barcols) start=g_url_len-barcols;
      text_n(MARGIN+2*ADV,2,g_url+start,barcols,COL_TEXT);
      if(g_editing){ int cx=MARGIN+2*ADV+(g_url_len-start)*ADV; fill(cx,2,2,16,COL_ACCENT); } }

    draw_text(MARGIN,STATUS_Y,g_status,COL_DIM);

    /* the page itself: light paper panel behind the content */
    fill(PAGE_X, CONTENT_TOP, PAGE_W, WIN_H-CONTENT_TOP, COL_PAGE);

    /* content: positioned runs offset by scroll, clipped below the bar */
    clamp_scroll();
    for(i=0;i<g_doc.run_count;++i){
        struct wl_run *r=&g_doc.runs[i];
        int sy=CONTENT_TOP + r->y - g_scroll;
        int rx=CONTENT_X + r->x;
        if(sy+r->h < CONTENT_TOP || sy >= WIN_H) continue;       /* offscreen */

        if(r->kind==WL_RECT){
            /* block background: clip the top to the content area */
            int top=sy, h=r->h;
            if(top<CONTENT_TOP){ h-=(CONTENT_TOP-top); top=CONTENT_TOP; }
            if(h>0) fill(rx, top, r->w, h, r->color);
            continue;
        }
        if(r->kind==WL_IMAGE){
            struct img_entry *im = (r->off>=0 && r->off<g_doc.href_count) ? image_by_src(g_doc.hrefs[r->off]) : 0;
            if(im && im->px){
                int yy,xx;
                for(yy=0;yy<r->h;++yy){ int py=sy+yy; if(py<CONTENT_TOP||py>=WIN_H) continue;
                    { int syi=(int)((long)yy*im->h/r->h);
                      for(xx=0;xx<r->w;++xx){ int pxx=rx+xx; if(pxx<0||pxx>=WIN_W) continue;
                          { int sxi=(int)((long)xx*im->w/r->w);
                            g_canvas[py*WIN_W+pxx]=im->px[syi*im->w+sxi]; } } } }
            } else if(sy>=CONTENT_TOP){ fill(rx,sy,r->w,r->h,0x00e7eaf2u); }   /* missing: grey box */
            continue;
        }
        if(r->kind==WL_RULE){
            if(sy>=CONTENT_TOP) fill(rx, sy, r->w, r->h, r->color);
            continue;
        }
        if(r->kind==WL_BULLET){
            int rad=r->px/7+2, cxq=rx+rad+2, cyq=(sy+r->h/2), yy,xx;
            for(yy=-rad;yy<=rad;++yy) for(xx=-rad;xx<=rad;++xx)
                if(xx*xx+yy*yy<=rad*rad && cyq+yy>=CONTENT_TOP) put_c(cxq+xx,cyq+yy,r->color,CONTENT_TOP);
            continue;
        }
        /* WL_TEXT */
        { if(r->link>=0 && r->link==g_hover_link && sy>=CONTENT_TOP)
              fill(rx-1, sy, r->w+2, r->h, COL_HOVER);          /* hover highlight */
          else if(r->bg && sy>=CONTENT_TOP)
              fill(rx-1, sy, r->w+2, r->h, r->bg);               /* code background */
          draw_run_text(rx, sy, r);
          if(r->underline && sy+r->h-2>=CONTENT_TOP) fill(rx, sy+r->h-2, r->w, 1, r->color);
        }
    }

    /* scrollbar */
    if(g_doc.height>content_h()){
        int trackh=WIN_H-CONTENT_TOP;
        int knobh=trackh*content_h()/g_doc.height; if(knobh<8)knobh=8;
        int knoby=CONTENT_TOP+(trackh-knobh)*g_scroll/(g_doc.height-content_h());
        fill(WIN_W-SCROLLBAR_W,CONTENT_TOP,SCROLLBAR_W-1,trackh,COL_BG);
        fill(WIN_W-SCROLLBAR_W,knoby,SCROLLBAR_W-1,knobh,COL_ACCENT);
    }

    present();
}

/* ---- link hit-testing ---- */
static int link_at(int ex,int ey){
    int i, docx=ex-CONTENT_X, docy=(ey-CONTENT_TOP)+g_scroll;
    if(ey<CONTENT_TOP) return -1;
    for(i=0;i<g_doc.run_count;++i){
        struct wl_run *r=&g_doc.runs[i];
        if(r->kind!=WL_TEXT || r->link<0) continue;
        if(docx>=r->x && docx<r->x+r->w && docy>=r->y && docy<r->y+r->h) return r->link;
    }
    return -1;
}
static int hit_link(int ex,int ey,char *out,int cap){
    int li=link_at(ex,ey);
    if(li<0) return 0;
    resolve_href(g_doc.hrefs[li], out, cap);
    return out[0]!=0;
}

/* ---- input ---- */
static void url_putc(char c){ if(g_url_len<(int)sizeof(g_url)-1){ g_url[g_url_len++]=c; g_url[g_url_len]=0; } }
static void url_backspace(void){ if(g_url_len>0){ g_url[--g_url_len]=0; } }

static void on_key(uint32_t k){
    if(g_editing){
        if(k=='\n') load_url();
        else if(k==0x08) url_backspace();
        else if(k>=0x20&&k<0x7f) url_putc((char)k);
        return;
    }
    switch(k){
        case 'u': case '/': g_editing=1; break;
        case 'j': g_scroll+=LINEH; break;
        case 'k': g_scroll-=LINEH; break;
        case ' ': case 'f': g_scroll+=content_h()-LINEH; break;
        case 'b': g_scroll-=content_h()-LINEH; break;
        case 'g': g_scroll=0; break;
        case 'G': g_scroll=g_doc.height; break;
        default: break;
    }
    clamp_scroll();
}

int main(void){
    g_win=vos_window_create("Browser",WIN_INIT_W,WIN_INIT_H);
    if(g_win<0){
        fputs("browser: no window server running. Start the desktop with 'gui'.\n", STDERR_FILENO);
        return 1;
    }

    appfont_init();
    wl_set_metrics(appfont_advance, appfont_line_height);
    wl_set_image_sizer(img_sizer);
    wl_init(&g_doc);
    sset(g_url,sizeof g_url,"example.com"); g_url_len=(int)strlen(g_url);
    sset(g_status,sizeof g_status,"type a URL, Enter to load  (u=edit click=link j/k/space=scroll)");
    render();

    for(;;){
        struct vos_event ev;
        int dirty=0;
        while(vos_event_poll(g_win,&ev)==1){
            if(ev.type==VOS_EV_CLOSE){ return 0; }
            else if(ev.type==VOS_EV_KEY){ on_key(ev.key); dirty=1; }
            else if(ev.type==VOS_EV_SCROLL){ g_scroll += ev.y * 3*LINEH; clamp_scroll(); dirty=1; }
            else if(ev.type==VOS_EV_RESIZE){
                g_win_w=ev.x; g_win_h=ev.y;
                if(g_win_w<80)g_win_w=80; if(g_win_h<60)g_win_h=60;
                if(g_win_w>WIN_MAX_W)g_win_w=WIN_MAX_W; if(g_win_h>WIN_MAX_H)g_win_h=WIN_MAX_H;
                layout_html_current(0);
                dirty=1;
            }
            else if(ev.type==VOS_EV_MOUSE_MOVE){
                int hl = g_editing ? -1 : link_at(ev.x,ev.y);
                if(hl != g_hover_link){ g_hover_link = hl; dirty=1; }
            }
            else if(ev.type==VOS_EV_MOUSE_DOWN){
                if(ev.y>=0&&ev.y<BAR_H){ g_editing=1; dirty=1; }
                else if(!g_editing){
                    char href[1024];
                    if(hit_link(ev.x,ev.y,href,sizeof href)){ sset(g_url,sizeof g_url,href); g_url_len=(int)strlen(g_url); load_url(); }
                    dirty=1;
                }
            }
        }
        if(dirty) render();
        vos_yield();
    }
}
