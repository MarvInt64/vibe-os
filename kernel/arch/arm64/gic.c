/* VibeOS arm64 — GICv2 interrupt controller driver.
 *
 * Distributor (GICD) is global; CPU interface (GICC) is per-CPU.
 * We support only BSP (CPU 0) for now.
 *
 * PPI (private peripheral interrupts) are INTID 16-31.
 * SPI (shared peripheral interrupts) are INTID 32+.
 * SGI (software generated) are INTID 0-15.
 *
 * Timer PPI: physical timer → INTID 30.
 */
#include "arch.h"

void arm64_gic_init(void) {
    /* ---- Distributor init -------------------------------------------- */
    uint32_t typer = mmio_read32(GICD_TYPER);
    unsigned n_ints = (typer & 0x1f) * 32 + 32;   /* ITLinesNumber */

    /* Disable distributor while configuring */
    mmio_write32(GICD_CTLR, 0);

    /* Set all interrupts to group 1 (non-secure) so GICC_CTLR.AckCtl=0
     * works correctly with NS operation.  QEMU simulates this fine. */
    for (unsigned i = 0; i < n_ints; i += 32)
        mmio_write32(GICD_BASE + 0x080 + (i / 8), 0xFFFFFFFF);

    /* Disable all SPIs */
    for (unsigned i = 32; i < n_ints; i += 32)
        mmio_write32(GICD_ICENABLER + (i / 8), 0xFFFFFFFF);

    /* Set default priority 0xA0 for all SPIs */
    for (unsigned i = 32; i < n_ints; i += 4)
        mmio_write32(GICD_IPRIORITYR(i), 0xA0A0A0A0);

    /* Route all SPIs to CPU 0 */
    for (unsigned i = 32; i < n_ints; i += 4)
        mmio_write32(GICD_ITARGETSR(i), 0x01010101);

    /* Enable distributor, group 0 + group 1 */
    mmio_write32(GICD_CTLR, 3);

    /* ---- CPU interface init ------------------------------------------ */
    /* Priority mask: accept all interrupts (lowest priority = 0xFF) */
    mmio_write32(GICC_PMR, 0xFF);
    /* Binary point: all priority bits for preemption */
    mmio_write32(GICC_BPR, 0);
    /* Enable CPU interface: signal group 0 as FIQ, group 1 as IRQ */
    mmio_write32(GICC_CTLR, 1);

    /* ---- PPI setup for this CPU (timer PPI, INTID 30) ---------------- */
    /* PPIs and SGIs are configured in the GICD with INTID 0-31 */
    /* Set priority of PPI 30 */
    /* GICD_IPRIORITYR byte index = INTID; each reg holds 4 bytes */
    uint32_t prio_reg = mmio_read32(GICD_IPRIORITYR(28)); /* covers 28-31 */
    prio_reg &= ~(0xFFU << 16);                            /* byte for INTID 30 */
    prio_reg |= (0x80U << 16);
    mmio_write32(GICD_IPRIORITYR(28), prio_reg);
}

void arm64_gic_enable_irq(unsigned irq) {
    mmio_write32(GICD_ISENABLER + (irq / 8) & ~3u,
                 1u << (irq % 32));
    /* Properly aligned: register at (irq/32)*4 */
    mmio_write32(GICD_BASE + 0x100 + (irq / 32) * 4,
                 1u << (irq % 32));
}

unsigned arm64_gic_ack(void) {
    return mmio_read32(GICC_IAR) & 0x3FF;  /* INTID field */
}

void arm64_gic_eoi(unsigned irq) {
    mmio_write32(GICC_EOIR, irq);
}
