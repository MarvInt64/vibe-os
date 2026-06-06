/* VibeOS arm64 — ARM generic timer (EL1 physical timer).
 *
 * The physical timer fires IRQ INTID 30 (PPI) each interval.
 * We use CNTP_TVAL_EL0 (countdown register) for simplicity.
 *
 * Registers:
 *   CNTFRQ_EL0   — counter frequency in Hz (set by firmware/QEMU)
 *   CNTPCT_EL0   — physical counter (read-only, monotonic)
 *   CNTP_CTL_EL0 — control: ENABLE[0], IMASK[1], ISTATUS[2]
 *   CNTP_TVAL_EL0 — reloaded each tick: counts down, fires IRQ at 0
 *   CNTP_CVAL_EL0 — absolute comparison value (alternative to TVAL)
 */
#include "arch.h"

static uint64_t g_timer_interval_ticks = 0;

void arm64_timer_init(void) {
    /* Read hardware frequency */
    uint64_t freq = read_sysreg(cntfrq_el0);
    /* Default interval: 10 ms */
    g_timer_interval_ticks = freq / 100;

    /* Disable timer while setting up */
    write_sysreg(cntp_ctl_el0, 0);

    /* Load initial countdown value */
    write_sysreg(cntp_tval_el0, g_timer_interval_ticks);

    /* Enable physical timer (ENABLE=1, IMASK=0) */
    write_sysreg(cntp_ctl_el0, 1);

    /* Enable GIC IRQ for physical timer */
    arm64_gic_enable_irq(TIMER_IRQ_PHYS);
}

void arm64_timer_set_interval_ms(uint32_t ms) {
    uint64_t freq = read_sysreg(cntfrq_el0);
    g_timer_interval_ticks = (freq * ms) / 1000;
    write_sysreg(cntp_tval_el0, g_timer_interval_ticks);
}

void arm64_timer_ack(void) {
    /* Reload countdown — this re-arms the timer */
    write_sysreg(cntp_tval_el0, g_timer_interval_ticks);
}

uint64_t arm64_timer_ticks(void) {
    return read_sysreg(cntpct_el0);
}
