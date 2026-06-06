/* VibeOS arm64 — ramfb framebuffer via QEMU fw_cfg.
 *
 * QEMU's `ramfb` device is a simple linear framebuffer whose parameters are
 * programmed by the guest through the fw_cfg interface. The guest:
 *   1. finds the "etc/ramfb" file in the fw_cfg directory,
 *   2. allocates a framebuffer in RAM,
 *   3. writes a RAMFBCfg struct (BE fields!) to that file via a fw_cfg DMA op.
 * After that, writing pixels into the framebuffer memory shows on screen.
 *
 * fw_cfg MMIO (QEMU virt @ 0x09020000):
 *   +0x00  data   (64-bit, but we use selector+DMA)
 *   +0x08  selector (16-bit, BE)
 *   +0x10  DMA address (64-bit, BE) — write low 32 bits last to kick
 *
 * fw_cfg DMA control (all big-endian):
 *   struct { u32 control; u32 length; u64 address; }
 *   control bits: 1=error 2=read 4=skip 8=select(<<16 selector) 16=write
 */
#include "arch.h"
#include "../../include/serial.h"
#include "../../include/alloc.h"
#include "../../include/string.h"

#define FWCFG_BASE     0x09020000UL
#define FWCFG_SELECTOR (FWCFG_BASE + 0x08)
#define FWCFG_DMA_ADDR (FWCFG_BASE + 0x10)

#define FW_CFG_SIGNATURE 0x0000
#define FW_CFG_FILE_DIR  0x0019

/* DMA control bits */
#define FW_CFG_DMA_CTL_ERROR  0x01
#define FW_CFG_DMA_CTL_READ   0x02
#define FW_CFG_DMA_CTL_SKIP   0x04
#define FW_CFG_DMA_CTL_SELECT 0x08
#define FW_CFG_DMA_CTL_WRITE  0x10

struct fw_cfg_dma {
    uint32_t control;   /* BE */
    uint32_t length;    /* BE */
    uint64_t address;   /* BE */
} __attribute__((packed));

struct fw_cfg_file {
    uint32_t size;      /* BE */
    uint16_t select;    /* BE */
    uint16_t reserved;
    char     name[56];
} __attribute__((packed));

/* RAMFBCfg — all fields big-endian */
struct ramfb_cfg {
    uint64_t addr;      /* BE: physical address of framebuffer */
    uint32_t fourcc;    /* BE: DRM fourcc pixel format */
    uint32_t flags;     /* BE */
    uint32_t width;     /* BE */
    uint32_t height;    /* BE */
    uint32_t stride;    /* BE */
} __attribute__((packed));

/* DRM_FORMAT_XRGB8888 = 'X''R''2''4' little-endian = 0x34325258 */
#define DRM_FORMAT_XRGB8888 0x34325258

/* ---- byte-swap helpers ------------------------------------------------ */
static uint16_t bswap16(uint16_t v){ return (uint16_t)((v>>8)|(v<<8)); }
static uint32_t bswap32(uint32_t v){
    return ((v>>24)&0xff)|((v>>8)&0xff00)|((v<<8)&0xff0000)|((v<<24)&0xff000000);
}
static uint64_t bswap64(uint64_t v){
    return ((uint64_t)bswap32((uint32_t)v) << 32) | bswap32((uint32_t)(v>>32));
}

/* ---- framebuffer state ------------------------------------------------ */
static uint32_t *g_fb = 0;
static uint32_t  g_fb_w = 0, g_fb_h = 0, g_fb_stride = 0;

uint32_t *ramfb_buffer(void) { return g_fb; }
uint32_t  ramfb_width(void)  { return g_fb_w; }
uint32_t  ramfb_height(void) { return g_fb_h; }
uint32_t  ramfb_stride_px(void) { return g_fb_stride / 4; }

/* Issue a fw_cfg DMA: select `key`, then read/write `len` bytes at `buf`. */
static void fwcfg_dma(uint16_t key, void *buf, uint32_t len, int write) {
    static struct fw_cfg_dma dma __attribute__((aligned(16)));
    uint32_t ctl = (uint32_t)key << 16 | FW_CFG_DMA_CTL_SELECT
                 | (write ? FW_CFG_DMA_CTL_WRITE : FW_CFG_DMA_CTL_READ);
    dma.control = bswap32(ctl);
    dma.length  = bswap32(len);
    dma.address = bswap64((uint64_t)(uintptr_t)buf);
    __asm__ volatile("dsb sy" ::: "memory");

    /* Kick: write the 64-bit BE address of the dma struct to the DMA register.
     * Writing the high half then the low half (the low write triggers it). */
    uint64_t dma_pa = (uint64_t)(uintptr_t)&dma;
    volatile uint32_t *reg = (volatile uint32_t *)FWCFG_DMA_ADDR;
    reg[0] = bswap32((uint32_t)(dma_pa >> 32));   /* high */
    reg[1] = bswap32((uint32_t)(dma_pa & 0xffffffff)); /* low → triggers */
    __asm__ volatile("dsb sy" ::: "memory");

    /* Poll until control is 0 (done) or error bit set */
    while (bswap32(dma.control) & ~FW_CFG_DMA_CTL_ERROR)
        __asm__ volatile("yield");
}

/* Find a fw_cfg file by name; returns its selector key + size, or 0. */
static uint16_t fwcfg_find(const char *name, uint32_t *size_out) {
    uint32_t count_be;
    fwcfg_dma(FW_CFG_FILE_DIR, &count_be, 4, 0);
    uint32_t count = bswap32(count_be);
    if (count == 0 || count > 4096) return 0;

    for (uint32_t i = 0; i < count; i++) {
        struct fw_cfg_file f;
        /* The directory is a stream; after selecting FILE_DIR the count is
         * first, then entries. We must re-read sequentially — easiest is to
         * read the whole thing. But fw_cfg DMA re-selects each call, so read
         * count+entries in one DMA into a buffer. */
        (void)f;
        break;
    }
    /* Simpler: read the entire directory in one DMA. */
    uint32_t total = 4 + count * sizeof(struct fw_cfg_file);
    uint8_t *dir = (uint8_t *)kmalloc(total);
    if (!dir) return 0;
    fwcfg_dma(FW_CFG_FILE_DIR, dir, total, 0);
    struct fw_cfg_file *files = (struct fw_cfg_file *)(dir + 4);

    uint16_t found = 0;
    for (uint32_t i = 0; i < count; i++) {
        /* names are NUL-terminated, compare */
        int eq = 1;
        for (int k = 0; name[k] || files[i].name[k]; k++) {
            if (name[k] != files[i].name[k]) { eq = 0; break; }
            if (name[k] == 0) break;
        }
        if (eq) {
            found = bswap16(files[i].select);
            if (size_out) *size_out = bswap32(files[i].size);
            break;
        }
    }
    kfree(dir);
    return found;
}

/* Initialise ramfb at the requested resolution. Returns 0 on success. */
int ramfb_init(uint32_t width, uint32_t height) {
    /* Verify fw_cfg signature "QEMU" */
    uint32_t sig;
    fwcfg_dma(FW_CFG_SIGNATURE, &sig, 4, 0);
    if (memcmp(&sig, "QEMU", 4) != 0) {
        serial_write("[ramfb] fw_cfg signature mismatch\r\n");
        return -1;
    }

    uint32_t fsize = 0;
    uint16_t key = fwcfg_find("etc/ramfb", &fsize);
    if (!key) { serial_write("[ramfb] etc/ramfb not found\r\n"); return -1; }

    /* Allocate the framebuffer (32 bpp) */
    uint32_t stride = width * 4;
    uint32_t fbsize = stride * height;
    /* Page-align the framebuffer */
    uint8_t *raw = (uint8_t *)kmalloc(fbsize + 4096);
    if (!raw) { serial_write("[ramfb] OOM\r\n"); return -1; }
    uint8_t *fb = (uint8_t *)(((uintptr_t)raw + 4095) & ~(uintptr_t)4095);
    memset(fb, 0, fbsize);

    /* Program ramfb via the cfg struct (big-endian fields) */
    static struct ramfb_cfg cfg __attribute__((aligned(16)));
    cfg.addr   = bswap64((uint64_t)(uintptr_t)fb);
    cfg.fourcc = bswap32(DRM_FORMAT_XRGB8888);
    cfg.flags  = 0;
    cfg.width  = bswap32(width);
    cfg.height = bswap32(height);
    cfg.stride = bswap32(stride);
    fwcfg_dma(key, &cfg, sizeof(cfg), 1);   /* write config → display turns on */

    g_fb = (uint32_t *)fb;
    g_fb_w = width; g_fb_h = height; g_fb_stride = stride;

    serial_write("[ramfb] ");
    serial_write_hex_u64(width); serial_write("x");
    serial_write_hex_u64(height);
    serial_write(" fb="); serial_write_hex_u64((uint64_t)(uintptr_t)fb);
    serial_write("\r\n");
    return 0;
}

/* Fill the whole screen with one ARGB colour. */
void ramfb_clear(uint32_t argb) {
    if (!g_fb) return;
    uint32_t n = (g_fb_stride / 4) * g_fb_h;
    for (uint32_t i = 0; i < n; i++) g_fb[i] = argb;
}

/* Draw a filled rectangle. */
void ramfb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t argb) {
    if (!g_fb) return;
    uint32_t sp = g_fb_stride / 4;
    for (uint32_t yy = y; yy < y + h && yy < g_fb_h; yy++)
        for (uint32_t xx = x; xx < x + w && xx < g_fb_w; xx++)
            g_fb[yy * sp + xx] = argb;
}
