/* VibeOS arm64 architecture definitions.
 * Target: QEMU virt machine with GICv2, PL011 UART, ARM generic timer.
 * Memory map (QEMU virt):
 *   0x00000000-0x07FFFFFF  MMIO low (flash, etc.)
 *   0x08000000             GICv2 distributor (GICD)
 *   0x08010000             GICv2 CPU interface (GICC)
 *   0x09000000             PL011 UART
 *   0x40000000             RAM start (512 MB → 0x5FFFFFFF)
 * Kernel loads at 0x40200000 (2 MB above RAM, DTB sits at 0x40000000).
 */
#ifndef VIBEOS_ARCH_ARM64_H
#define VIBEOS_ARCH_ARM64_H

#include <stdint.h>
#include <stddef.h>

/* ---- MMIO addresses --------------------------------------------------- */
#define GICD_BASE    0x08000000UL  /* GICv2 Distributor */
#define GICC_BASE    0x08010000UL  /* GICv2 CPU Interface */
#define UART0_BASE   0x09000000UL  /* PL011 UART */
#define RAM_BASE     0x40000000UL
#define KERN_BASE    0x40200000UL  /* where the kernel ELF is loaded */

/* ---- GICv2 Distributor registers -------------------------------------- */
#define GICD_CTLR      (GICD_BASE + 0x000)
#define GICD_TYPER     (GICD_BASE + 0x004)
#define GICD_ISENABLER (GICD_BASE + 0x100)  /* +4*n for interrupts 32n..32n+31 */
#define GICD_ICENABLER (GICD_BASE + 0x180)
#define GICD_ISPENDR   (GICD_BASE + 0x200)
#define GICD_IPRIORITYR(n) (GICD_BASE + 0x400 + (n))
#define GICD_ITARGETSR(n)  (GICD_BASE + 0x800 + (n))
#define GICD_ICFGR(n)      (GICD_BASE + 0xC00 + (n))

/* ---- GICv2 CPU Interface registers ------------------------------------ */
#define GICC_CTLR  (GICC_BASE + 0x000)
#define GICC_PMR   (GICC_BASE + 0x004)  /* priority mask (0xFF = all) */
#define GICC_BPR   (GICC_BASE + 0x008)
#define GICC_IAR   (GICC_BASE + 0x00C)  /* interrupt acknowledge */
#define GICC_EOIR  (GICC_BASE + 0x010)  /* end of interrupt */

/* ---- PL011 UART registers --------------------------------------------- */
#define UART_DR    (UART0_BASE + 0x000)  /* data register */
#define UART_FR    (UART0_BASE + 0x018)  /* flag register */
#define UART_IBRD  (UART0_BASE + 0x024)  /* integer baud divisor */
#define UART_FBRD  (UART0_BASE + 0x028)  /* fractional baud divisor */
#define UART_LCR_H (UART0_BASE + 0x02C)  /* line control */
#define UART_CR    (UART0_BASE + 0x030)  /* control */
#define UART_IMSC  (UART0_BASE + 0x038)  /* interrupt mask */
#define UART_ICR   (UART0_BASE + 0x044)  /* interrupt clear */
#define UART_FR_TXFF (1 << 5)
#define UART_FR_RXFE (1 << 4)

/* ---- ARM generic timer IRQ numbers ------------------------------------ */
/* PPIs: INTID 16-31. Physical timer PPI = 16+14 = 30. */
#define TIMER_IRQ_PHYS 30

/* ---- MMIO accessor ---------------------------------------------------- */
static inline void mmio_write32(uintptr_t addr, uint32_t val) {
    volatile uint32_t *p = (volatile uint32_t *)addr;
    *p = val;
}
static inline uint32_t mmio_read32(uintptr_t addr) {
    volatile uint32_t *p = (volatile uint32_t *)addr;
    return *p;
}

/* ---- System register helpers ----------------------------------------- */
#define read_sysreg(r)  ({ uint64_t _v; __asm__ volatile("mrs %0," #r : "=r"(_v)); _v; })
#define write_sysreg(r, v) __asm__ volatile("msr " #r ", %0" :: "r"((uint64_t)(v)))

/* ---- ramdisk forward declaration (for virtio_blk_get_device) ---------- */
struct ramdisk_device;

/* ---- virtio-blk ------------------------------------------------------- */
int virtio_blk_init(void);
int virtio_blk_get_device(struct ramdisk_device *dev);

/* ---- virtio-input ----------------------------------------------------- */
int  virtio_input_init(void);
void virtio_input_poll(void);
int  virtio_input_is_ready(void);
int  virtio_kbd_read(char *out);
extern int g_mouse_x, g_mouse_y, g_mouse_buttons, g_mouse_moved, g_mouse_wheel;

/* ---- virtio-sound ----------------------------------------------------- */
int  virtio_snd_init(void);
int  virtio_snd_ready(void);
int  virtio_snd_write(uint32_t pid, const void *buf, unsigned long bytes);
void virtio_snd_mix_tick(void);
int  virtio_snd_busy_slots(void);
void virtio_snd_set_rate(uint32_t hz);
void virtio_snd_set_volume(uint32_t vol);
uint32_t virtio_snd_get_rate(void);

/* ---- ramfb framebuffer ------------------------------------------------ */
int       ramfb_init(uint32_t width, uint32_t height);
int       ramfb_set_mode(uint32_t width, uint32_t height);
void      ramfb_clear(uint32_t argb);
void      ramfb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t argb);
uint32_t *ramfb_buffer(void);
uint32_t  ramfb_width(void);
uint32_t  ramfb_height(void);
uint32_t  ramfb_stride_px(void);

/* ---- arm64 arch function declarations --------------------------------- */
void arm64_uart_init(void);
void arm64_uart_putc(char c);
char arm64_uart_getc(void);
int  arm64_uart_can_read(void);

void arm64_mmu_init(void);

/* Per-process address spaces (see mmu.c). */
uint64_t arm64_aspace_create(void **l1_raw, void **l2_raw);
int      arm64_aspace_map_block(void *l2_raw, uint64_t va, uint64_t pa);
void     arm64_aspace_switch(uint64_t ttbr0);
void     arm64_aspace_switch_boot(void);
#define  ARM64_ASPACE_SLOT_BYTES (256UL * 1024 * 1024)

void arm64_gic_init(void);
void arm64_gic_enable_irq(unsigned irq);
unsigned arm64_gic_ack(void);
void arm64_gic_eoi(unsigned irq);

void arm64_timer_init(void);       /* IRQ mode (GIC path) */
void arm64_timer_init_poll(void);  /* Poll mode (HVF path, no GIC MMIO) */
void arm64_timer_set_interval_ms(uint32_t ms);
void arm64_timer_ack(void);
int  arm64_timer_poll(void);       /* returns 1 and advances tick when ISTATUS set */

/* Defined in arch.c, called from boot.S */
void kernel_main_arm64(void);

#endif /* VIBEOS_ARCH_ARM64_H */
