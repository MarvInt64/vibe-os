/* VibeOS arm64 SMP boot.
 * Starts secondary cores via PSCI CPU_ON.
 */
#include "arch.h"
#include "../../include/cpu.h"
#include "../../include/smp.h"
#include "../../include/serial.h"
#include "../../include/alloc.h"
#include "../../include/bkl.h"

#define PSCI_0_2_FN64_CPU_ON 0xc4000003

extern void smp_secondary_entry(void);

/* Call PSCI via HVC (QEMU virt uses HVC for PSCI) */
static inline uint64_t psci_call(uint64_t fid, uint64_t arg1, uint64_t arg2, uint64_t arg3) {
    register uint64_t x0 __asm__("x0") = fid;
    register uint64_t x1 __asm__("x1") = arg1;
    register uint64_t x2 __asm__("x2") = arg2;
    register uint64_t x3 __asm__("x3") = arg3;
    __asm__ volatile(
        "hvc #0"
        : "=r"(x0)
        : "r"(x0), "r"(x1), "r"(x2), "r"(x3)
        : "memory"
    );
    return x0;
}

static volatile int g_ap_started;
volatile uint64_t g_ap_stack_top;
static unsigned g_cpus_online = 1;

/* Called by the AP once it reaches C code */
void smp_ap_main(unsigned cpu_id) {
    /* Set up TPIDR_EL1 */
    cpu_register(cpu_id);

    uint64_t midr;
    __asm__ volatile("mrs %0, midr_el1" : "=r"(midr));
    int is_apple = (((midr >> 24) & 0xFF) == 0x61);

    if (is_apple) {
        /* Initialize timer for this core in poll mode */
        arm64_timer_init_poll();
    } else {
        /* Initialize GIC CPU interface for this core */
        arm64_gic_init();
        arm64_timer_init();
        __asm__ volatile("msr daifclr, #2");   /* unmask IRQ */
    }

    __atomic_add_fetch(&g_cpus_online, 1, __ATOMIC_SEQ_CST);

    /* Signal primary core that we're alive */
    g_ap_started = 1;
    __asm__ volatile("dmb sy" ::: "memory");

    /* Drop into the scheduler */
    extern int process_run_ready_slice(void);
    while (1) {
        bkl_acquire();
        int ran = process_run_ready_slice();
        bkl_release();
        /* Per-CPU tick: increment on every scheduler iteration.
         * If the virtual timer works, also drive from it at 100 Hz.
         * busy_ticks only when we actually ran a process. */
        struct cpu *c = this_cpu();
        if (c) {
            c->ticks++;
            if (ran) c->busy_ticks++;
        }
        if (arm64_timer_poll()) {
            if (c) c->ticks++;  /* bonus tick from timer if it fires */
        }
    }
}

void smp_boot_aps(void) {
    /* Read CPU count from DTB or assume 4 for now (we pass -smp 4) */
    /* For simplicity, we just try to boot CPUs 1, 2, 3 */
    for (int i = 1; i < 4; i++) {
        g_ap_started = 0;
        
        void *stack = kmalloc(65536);
        if (!stack) {
            serial_write("SMP: out of memory for AP stack\n");
            break;
        }
        g_ap_stack_top = (uint64_t)(uintptr_t)stack + 65536;
        __asm__ volatile("dmb sy" ::: "memory");

        /* CPU_ON: target cpu (MPIDR = i), entry point, context_id (cpu_id) */
        uint64_t res = psci_call(PSCI_0_2_FN64_CPU_ON, i, (uint64_t)(uintptr_t)smp_secondary_entry, i);
        if (res == 0) {
            /* Wait for it to signal */
            int timeout = 10000000;
            while (!g_ap_started && timeout > 0) {
                __asm__ volatile("yield");
                timeout--;
            }
            if (!g_ap_started) {
                serial_write("SMP: AP timed out\n");
            } else {
                serial_write("SMP: AP online\n");
            }
        } else {
            /* If PSCI returns error, we just stop trying. */
            kfree(stack);
            break;
        }
    }
}

unsigned smp_cpu_count(void) {
    return g_cpus_online;
}
