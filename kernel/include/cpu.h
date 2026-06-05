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

/*
 * Per-CPU data (milestone 3).
 *
 * Each logical CPU owns one struct cpu. The GS base MSR of every CPU is pointed
 * at its own struct, and the struct keeps a self-pointer in its first field, so
 * this_cpu() is a single GS-relative load regardless of which core runs it.
 * This is the foundation the per-CPU scheduler will build on; for now it tracks
 * identity plus a work counter used to prove the cores actually run code.
 */
#ifndef VIBEOS_CPU_H
#define VIBEOS_CPU_H

#include "types.h"

#define CPU_MAX 32

struct cpu {
    struct cpu   *self;     /* points at this struct; lives at gs:0  */
    unsigned      index;    /* 0 = boot CPU, then 1, 2, ...          */
    unsigned      apic_id;  /* Local APIC ID                         */
    volatile unsigned long ticks;   /* Local APIC timer ticks on this CPU    */
    volatile unsigned long allocs;  /* successful kmalloc/kfree cycles done  */
};

/* Claim the next per-CPU slot for the calling CPU, record its APIC id, and
 * point this CPU's GS base at the slot. Returns the assigned index. */
unsigned cpu_register(unsigned apic_id);

/* (Re)program the calling CPU's GS base to point at slot 'index'. Needed after
 * loading a GDT, which reloads the gs selector and clears the base. */
void cpu_set_gs_base(unsigned index);

/* Number of registered CPUs, and indexed access to their structs. */
unsigned cpu_count(void);
struct cpu *cpu_get(unsigned index);

/* The struct cpu of the CPU executing this call (valid after cpu_register). */
static inline struct cpu *this_cpu(void) {
    struct cpu *c;
    __asm__ volatile("mov %%gs:0, %0" : "=r"(c));
    return c;
}

#endif
