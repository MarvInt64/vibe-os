/* appimage - decode PNG/JPEG/GIF/BMP into RGBA via stb_image, for the browser.
 *
 * Runs in userspace (SSE/float available). Memory comes from the umalloc heap.
 * No WebP/AVIF (stb_image can't) - those simply fail to decode and the browser
 * shows the alt-text placeholder. */
#include "appimage.h"
#include "umalloc.h"
#include <string.h>
typedef unsigned int uint32_t;
typedef unsigned char uint8_t;

#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_STDIO
#define STBI_NO_FAILURE_STRINGS
#define STBI_NO_THREAD_LOCALS   /* single-threaded; no TLS segment exists here */
#define STBI_ASSERT(x) ((void)0)
#define STBI_MALLOC(sz)        umalloc((umsize_t)(sz))
#define STBI_REALLOC(p,sz)     urealloc((p),(umsize_t)(sz))
#define STBI_FREE(p)           ufree(p)
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

unsigned int *appimage_decode(const unsigned char *data, int len, int *w, int *h){
    int comp = 0;
    unsigned char *px = stbi_load_from_memory(data, len, w, h, &comp, 4); /* force RGBA */
    if(!px) return 0;
    /* stb gives RGBA bytes; pack to 0x00RRGGBB (ignore alpha for now). */
    {
        int n = (*w) * (*h), i;
        uint32_t *out = (uint32_t*)px;   /* repack in place: 4 bytes -> u32 */
        for(i=0;i<n;++i){
            unsigned char r=px[i*4+0], g=px[i*4+1], b=px[i*4+2];
            out[i] = ((uint32_t)r<<16)|((uint32_t)g<<8)|(uint32_t)b;
        }
        return out;
    }
}

void appimage_free(unsigned int *px){ if(px) ufree(px); }
