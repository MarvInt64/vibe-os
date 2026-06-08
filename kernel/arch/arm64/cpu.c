/* VibeOS arm64 CPU management.
 * Provides the same cpu.h API as x86_64, but uses TPIDR_EL1 instead of GS base.
 */
#include "../../include/cpu.h"
#include "../../include/spinlock.h"

_Static_assert(__builtin_offsetof(struct cpu, resume_rsp) == 48,
               "CPU_RESUME_RSP must match assembly");
_Static_assert(__builtin_offsetof(struct cpu, resume_result) == 56,
               "CPU_RESUME_RESULT must match assembly");

static struct cpu g_cpus[CPU_MAX];
static unsigned    g_cpu_count;
static spinlock_t  g_cpu_lock = SPINLOCK_INIT;

static inline void write_tpidr_el1(uint64_t value) {
    __asm__ volatile("msr tpidr_el1, %0" :: "r"(value));
}

unsigned cpu_register(unsigned apic_id) {
    spin_lock(&g_cpu_lock);
    unsigned idx = g_cpu_count;
    if (idx < CPU_MAX) g_cpu_count = idx + 1;
    spin_unlock(&g_cpu_lock);

    if (idx >= CPU_MAX) idx = CPU_MAX - 1;

    struct cpu *c = &g_cpus[idx];
    c->self = c;
    c->index = idx;
    c->apic_id = apic_id;
    c->ticks = 0;
    c->allocs = 0;
    c->current = (struct process *)0;
    c->sched_cursor = 0;
    c->resume_rsp = 0;
    c->resume_result = 0;
    c->slices = 0;
    c->busy_ticks = 0;

    write_tpidr_el1((uint64_t)(uintptr_t)c);
    return idx;
}

void cpu_set_gs_base(unsigned index) {
    struct cpu *c = (index < CPU_MAX) ? &g_cpus[index] : (struct cpu *)0;
    if (c) write_tpidr_el1((uint64_t)(uintptr_t)c);
}

unsigned cpu_count(void) { return g_cpu_count; }

struct cpu *cpu_get(unsigned index) {
    return index < CPU_MAX ? &g_cpus[index] : (struct cpu *)0;
}
