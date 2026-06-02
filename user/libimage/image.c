/* libimage — decode PNG/JPEG/GIF/BMP into packed 0x00RRGGBB pixels via stb_image.
 *
 * Runs in userspace (SSE/float available); memory comes from libc malloc, which
 * is the thread-safe umalloc heap, so this is safe to call from worker threads.
 * No WebP/AVIF (stb_image can't) — those simply fail and the caller handles it. */
#include "image.h"
#include <stdlib.h>

typedef unsigned int  u32;
typedef unsigned char u8;

#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_STDIO
#define STBI_NO_THREAD_LOCALS         /* single TLS-less callers */
#define STBI_ASSERT(x) ((void)0)
#define STBI_MALLOC(sz)        malloc((size_t)(sz))
#define STBI_REALLOC(p,sz)     realloc((p),(size_t)(sz))
#define STBI_FREE(p)           free(p)
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

unsigned int *image_decode(const unsigned char *data, int len, int *w, int *h) {
    int comp = 0;
    unsigned char *px = stbi_load_from_memory(data, len, w, h, &comp, 4); /* force RGBA */
    if (!px) return 0;
    /* Repack RGBA bytes to 0x00RRGGBB in place (4 bytes -> one u32). */
    {
        int n = (*w) * (*h), i;
        u32 *out = (u32 *)px;
        for (i = 0; i < n; ++i) {
            u8 r = px[i * 4 + 0], g = px[i * 4 + 1], b = px[i * 4 + 2];
            out[i] = ((u32)r << 16) | ((u32)g << 8) | (u32)b;
        }
        return out;
    }
}

void image_free(unsigned int *px) { if (px) free(px); }

const char *image_decode_failure_reason(void) {
    const char *reason = stbi_failure_reason();
    return reason ? reason : "unknown";
}
