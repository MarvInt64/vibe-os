/* VibeOS arm64 — GICv2 interrupt controller.
 *
 * For QEMU -machine virt,gic-version=2:
 *   GICD (distributor)   0x08000000
 *   GICC (CPU interface) 0x08010000
 *
 * Interrupt ID space:
 *   0-15   SGI  (software-generated, per CPU)
 *   16-31  PPI  (private peripheral, per CPU — includes timer)
 *   32+    SPI  (shared peripheral)
 *
 * ARM generic timer physical PPI = INTID 30.
 */
#include "arch.h"

void arm64_gic_init(void) {
    /* ---- Distributor ------------------------------------------------- */
    uint32_t typer   = mmio_read32(GICD_BASE + 0x004);
    unsigned n_lines = ((typer & 0x1f) + 1) * 32;  /* ITLinesNumber+1)*32 */

    /* Disable distributor while configuring */
    mmio_write32(GICD_BASE + 0x000, 0);

    /* Set all interrupts to Group 1 (non-secure / IRQ line) */
    for (unsigned i = 0; i < n_lines; i += 32)
        mmio_write32(GICD_BASE + 0x080 + (i / 8), 0xFFFFFFFF);

    /* Disable all SPIs, set default priority 0xA0 and target CPU 0 */
    for (unsigned i = 32; i < n_lines; i += 32)
        mmio_write32(GICD_BASE + 0x180 + (i / 8), 0xFFFFFFFF);  /* ICENABLER */
    for (unsigned i = 32; i < n_lines; i += 4)
        mmio_write32(GICD_BASE + 0x400 + i,        0xA0A0A0A0); /* IPRIORITY */
    for (unsigned i = 32; i < n_lines; i += 4)
        mmio_write32(GICD_BASE + 0x800 + i,        0x01010101); /* ITARGETSR */

    /* Set timer PPI (IRQ 30) priority to 0x80 */
    {
        /* GICD_IPRIORITYR for IRQs 28-31 is at offset 0x400 + 28 = 0x41C */
        uint32_t prio = mmio_read32(GICD_BASE + 0x41C);
        prio = (prio & ~(0xFFU << 16)) | (0x80U << 16);  /* byte 2 = IRQ 30 */
        mmio_write32(GICD_BASE + 0x41C, prio);
    }

    /* Enable distributor, Group 0 + Group 1 */
    mmio_write32(GICD_BASE + 0x000, 3);

    /* ---- CPU interface ------------------------------------------------ */
    mmio_write32(GICC_BASE + 0x004, 0xFF);  /* PMR: accept all priorities */
    mmio_write32(GICC_BASE + 0x008, 0x00);  /* BPR: no preemption grouping */
    mmio_write32(GICC_BASE + 0x000, 0x01);  /* CTLR: enable */
}

void arm64_gic_enable_irq(unsigned irq) {
    /* GICD_ISENABLER: one bit per IRQ, registers at +0x100, +0x104, … */
    unsigned reg = irq / 32;
    unsigned bit = irq % 32;
    mmio_write32(GICD_BASE + 0x100 + reg * 4, 1u << bit);
}

unsigned arm64_gic_ack(void) {
    /* Returns the INTID of the acknowledged interrupt (1023 = spurious) */
    return mmio_read32(GICC_BASE + 0x00C) & 0x3FF;
}

void arm64_gic_eoi(unsigned irq) {
    mmio_write32(GICC_BASE + 0x010, irq);
}
