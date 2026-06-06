/* VibeOS arm64 — ARM generic timer.
 *
 * HVF note: On Apple Silicon with QEMU's HVF backend, the EL1 physical
 * timer (CNTP_*) may trap to EL2 (HVF) because CNTHCTL_EL2.EL1PCEN is
 * not set by QEMU before dropping to EL1, causing an SError.  The virtual
 * timer (CNTV_*) is always accessible from EL1 and does not require any
 * EL2 configuration — use it as the safe default on HVF.
 *
 * CNTV_CTL_EL0:  bit 0=ENABLE, bit 1=IMASK, bit 2=ISTATUS (read-only)
 * CNTV_TVAL_EL0: countdown register (fires IRQ or sets ISTATUS when ≤ 0)
 * CNTVCT_EL0:    virtual counter (reads like CNTPCT on QEMU)
 *
 * Virtual timer PPI: IRQ 27 (GIC INTID 27).
 */
#include "arch.h"

#define VTIMER_IRQ 27

static uint64_t g_timer_interval_ticks = 0;
static int      g_poll_mode            = 0;

/* Poll mode (HVF): virtual timer, IMASK=1, no GIC MMIO. */
void arm64_timer_init_poll(void) {
    uint64_t freq          = read_sysreg(cntfrq_el0);
    g_timer_interval_ticks = freq / 100;   /* 10 ms */
    g_poll_mode            = 1;

    write_sysreg(cntv_ctl_el0, 0);                     /* disable */
    write_sysreg(cntv_tval_el0, g_timer_interval_ticks);
    write_sysreg(cntv_ctl_el0, (1u << 1) | 1u);        /* ENABLE | IMASK */
}

/* IRQ mode (TCG + GIC): physical timer, IMASK=0. */
void arm64_timer_init(void) {
    uint64_t freq          = read_sysreg(cntfrq_el0);
    g_timer_interval_ticks = freq / 100;
    g_poll_mode            = 0;

    write_sysreg(cntp_ctl_el0, 0);
    write_sysreg(cntp_tval_el0, g_timer_interval_ticks);
    write_sysreg(cntp_ctl_el0, 1u);

    arm64_gic_enable_irq(TIMER_IRQ_PHYS);
}

void arm64_timer_ack(void) {
    if (g_poll_mode)
        write_sysreg(cntv_tval_el0, g_timer_interval_ticks);
    else
        write_sysreg(cntp_tval_el0, g_timer_interval_ticks);
}

/* Returns 1 when poll-mode timer has fired (CNTV_CTL_EL0.ISTATUS set). */
int arm64_timer_poll(void) {
    if (!g_poll_mode) return 0;
    uint64_t ctl = read_sysreg(cntv_ctl_el0);
    if (ctl & (1ULL << 2)) {
        arm64_timer_ack();
        return 1;
    }
    return 0;
}

uint64_t arm64_timer_ticks(void) {
    return g_poll_mode ? read_sysreg(cntvct_el0) : read_sysreg(cntpct_el0);
}
