/*
 * MIT License
 *
 * Copyright (c) 2026 Marvin Kicha
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "cpu.h"
#include "spinlock.h"

#define IA32_GS_BASE 0xC0000101u

static struct cpu g_cpus[CPU_MAX];
static unsigned    g_cpu_count;
static spinlock_t  g_cpu_lock = SPINLOCK_INIT;

static inline void write_gs_base(uint64_t value) {
    __asm__ volatile("wrmsr"
                     : : "c"(IA32_GS_BASE),
                         "a"((uint32_t)value),
                         "d"((uint32_t)(value >> 32)));
}

unsigned cpu_register(unsigned apic_id) {
    /* Allocating the slot is the only shared step, so it is the only part that
     * needs the lock; the rest writes this CPU's own struct. */
    spin_lock(&g_cpu_lock);
    unsigned idx = g_cpu_count;
    if (idx < CPU_MAX) g_cpu_count = idx + 1;
    spin_unlock(&g_cpu_lock);

    if (idx >= CPU_MAX) idx = CPU_MAX - 1;   /* clamp; should never happen */

    struct cpu *c = &g_cpus[idx];
    c->self = c;
    c->index = idx;
    c->apic_id = apic_id;
    c->ticks = 0;
    c->allocs = 0;

    /* Point this CPU's GS base at its struct so this_cpu() resolves via gs:0. */
    write_gs_base((uint64_t)(uintptr_t)c);
    return idx;
}

unsigned cpu_count(void) { return g_cpu_count; }

struct cpu *cpu_get(unsigned index) {
    return index < CPU_MAX ? &g_cpus[index] : (struct cpu *)0;
}
