/* appfont — stb_truetype glyph rasterization + cache. See appfont.h.
 *
 * Provides the small set of libm/libc shims stb_truetype expects. Userspace
 * SSE is enabled, so floating point works; sqrt uses the SSE instruction. The
 * transcendental shims (pow/cos/acos) are only referenced by stb's SDF path,
 * which we never call — they exist solely so the header compiles. */
#include "appfont.h"
#include "umalloc.h"
#include <string.h>

/* ---- tiny libm for stb_truetype ---- */
static double m_fabs(double x){ return x<0?-x:x; }
static double m_sqrt(double x){
    double r; int i;
    if(x<=0.0) return 0.0;
    r = x>1.0 ? x : 1.0;                 /* Newton-Raphson (portable, no FP asm) */
    for(i=0;i<30;++i) r = 0.5*(r + x/r);
    return r;
}
static double m_floor(double x){ double t=(double)(long)x; return (t>x)?t-1.0:t; }
static double m_ceil(double x){ double t=(double)(long)x; return (t<x)?t+1.0:t; }
static double m_fmod(double x,double y){ if(y==0.0) return 0.0; { double q=(double)(long)(x/y); return x-q*y; } }
/* unused-by-bitmap-path stubs (SDF only) */
static double m_pow(double x,double y){ (void)y; return x; }
static double m_cos(double x){ (void)x; return 1.0; }
static double m_acos(double x){ (void)x; return 0.0; }

#define STBTT_ifloor(x)   ((int)m_floor(x))
#define STBTT_iceil(x)    ((int)m_ceil(x))
#define STBTT_sqrt(x)     m_sqrt(x)
#define STBTT_pow(x,y)    m_pow(x,y)
#define STBTT_fmod(x,y)   m_fmod(x,y)
#define STBTT_cos(x)      m_cos(x)
#define STBTT_acos(x)     m_acos(x)
#define STBTT_fabs(x)     m_fabs(x)
#define STBTT_malloc(s,u) ((void)(u), umalloc((umsize_t)(s)))
#define STBTT_free(p,u)   ((void)(u), ufree(p))
#define STBTT_assert(x)   ((void)0)
#define STBTT_strlen(s)   strlen(s)
#define STBTT_memcpy      memcpy
#define STBTT_memset      memset
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

/* embedded font (see font_data.S) */
extern const unsigned char g_font_ttf[];
extern const unsigned char g_font_ttf_end[];

static stbtt_fontinfo g_info;
static int g_ready;

int appfont_init(void){
    if(g_ready) return 0;
    if(stbtt_InitFont(&g_info, g_font_ttf, stbtt_GetFontOffsetForIndex(g_font_ttf,0))==0)
        return -1;
    g_ready=1;
    return 0;
}

static float scale_for(int px){ return stbtt_ScaleForPixelHeight(&g_info,(float)px); }

int appfont_advance(int cp,int px){
    int aw=0,lsb=0;
    if(!g_ready && appfont_init()<0) return px/2;
    stbtt_GetCodepointHMetrics(&g_info,cp,&aw,&lsb);
    return (int)(aw*scale_for(px)+0.5f);
}
int appfont_ascent(int px){
    int a=0,d=0,g=0;
    if(!g_ready && appfont_init()<0) return px;
    stbtt_GetFontVMetrics(&g_info,&a,&d,&g);
    return (int)(a*scale_for(px)+0.5f);
}
int appfont_line_height(int px){
    int a=0,d=0,g=0;
    if(!g_ready && appfont_init()<0) return px+2;
    stbtt_GetFontVMetrics(&g_info,&a,&d,&g);
    return (int)((a-d+g)*scale_for(px)+0.5f);
}

/* ---- glyph cache ---- */
#define AF_CACHE 768
struct af_entry { int cp, px; struct af_glyph g; int used; };
static struct af_entry g_cache[AF_CACHE];
static int g_cache_n;

const struct af_glyph *appfont_get(int cp,int px){
    int i;
    if(!g_ready && appfont_init()<0) return 0;
    for(i=0;i<g_cache_n;++i) if(g_cache[i].used && g_cache[i].cp==cp && g_cache[i].px==px) return &g_cache[i].g;

    {
        struct af_entry *e;
        int w=0,h=0,xo=0,yo=0,aw=0,lsb=0;
        float sc=scale_for(px);
        unsigned char *bmp = stbtt_GetCodepointBitmap(&g_info,sc,sc,cp,&w,&h,&xo,&yo);
        stbtt_GetCodepointHMetrics(&g_info,cp,&aw,&lsb);

        if(g_cache_n>=AF_CACHE){
            /* cache full: drop everything (simple; pages rarely exceed this) */
            for(i=0;i<g_cache_n;++i){ if(g_cache[i].g.cov) ufree((void*)g_cache[i].g.cov); g_cache[i].used=0; }
            g_cache_n=0;
        }
        e=&g_cache[g_cache_n++];
        e->cp=cp; e->px=px; e->used=1;
        e->g.w=w; e->g.h=h; e->g.xoff=xo; e->g.yoff=yo;
        e->g.advance=(int)(aw*sc+0.5f);
        e->g.cov=bmp;   /* malloc'd via STBTT_malloc=umalloc; freed on cache flush */
        return &e->g;
    }
}
