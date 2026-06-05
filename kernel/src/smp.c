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

#include "smp.h"
#include "acpi.h"
#include "apic.h"
#include "alloc.h"
#include "cpu.h"
#include "spinlock.h"
#include "serial.h"
#include "string.h"
#include "types.h"

/* Physical address the AP trampoline is copied to and where the APs start
 * executing. Must be page-aligned and < 1 MB (real-mode reachable); the SIPI
 * vector is this address >> 12. 0x8000 is free conventional memory. */
#define TRAMPOLINE_ADDR 0x8000u
#define AP_STACK_SIZE   (16u * 1024u)

/* Provided by ap_boot.S: the trampoline blob to relocate into low memory. */
extern uint8_t ap_trampoline_start[];
extern uint8_t ap_trampoline_end[];

/* Hand-off slot read by the trampoline's 64-bit stub: the BSP writes the top
 * of the next AP's stack here before sending the STARTUP IPI. */
volatile uint64_t ap_stack_top;

/* CPUs that have reached ap_main. The boot CPU is implicitly online. */
static volatile unsigned g_cpus_online = 1;

/* The big kernel lock and a shared counter the APs hammer through it, purely to
 * demonstrate that the cores run kernel code concurrently and that the lock
 * keeps a shared update race-free (g_work_total must equal the sum of every
 * CPU's private work count if no increment was lost). */
static spinlock_t g_bkl = SPINLOCK_INIT;
static volatile unsigned long g_work_total;

/* Each AP performs this many locked increments, then parks. */
#define AP_WORK_UNITS 5000u

/* Crude spin delay — INIT/SIPI need ~10 ms / ~200 us settling. QEMU is lenient
 * about exact timing, so a generous busy loop is sufficient at bring-up. */
static void delay_spin(volatile uint32_t iterations) {
    while (iterations--) __asm__ volatile("pause");
}

/*
 * C entry point for an application processor (called from ap_boot.S on the
 * AP's own stack, in 64-bit mode). Set up this core's per-CPU data and Local
 * APIC, announce we're alive, then run a burst of real, lock-protected kernel
 * work to prove the core executes concurrently with the others. With no
 * per-CPU scheduler yet, the AP then parks (interrupts disabled, hlt) — a
 * parked core only executes hlt, so it needs no IDT/TSS of its own for now.
 */
void ap_main(void) {
    cpu_register(lapic_id());
    lapic_enable_this_cpu();
    __atomic_add_fetch(&g_cpus_online, 1, __ATOMIC_SEQ_CST);

    for (unsigned i = 0; i < AP_WORK_UNITS; ++i) {
        spin_lock(&g_bkl);
        g_work_total++;          /* shared: protected by the big kernel lock */
        this_cpu()->work++;      /* private to this core                     */
        spin_unlock(&g_bkl);
    }

    for (;;) __asm__ volatile("cli; hlt");
}

/* Print each CPU's work tally and confirm the shared total is consistent —
 * i.e. the lock lost no increments under concurrency. */
static void smp_report(void) {
    unsigned long sum = 0;
    for (unsigned i = 0; i < cpu_count(); ++i) {
        struct cpu *c = cpu_get(i);
        sum += c->work;
        serial_write("SMP: cpu");
        serial_write_hex_u64(i);
        serial_write(" apic_id=");
        serial_write_hex_u64(c->apic_id);
        serial_write(" work=");
        serial_write_hex_u64(c->work);
        serial_write("\n");
    }
    serial_write("SMP: work_total=");
    serial_write_hex_u64(g_work_total);
    serial_write(" sum_per_cpu=");
    serial_write_hex_u64(sum);
    serial_write(g_work_total == sum ? " (lock OK)\n" : " (LOCK LOST UPDATES!)\n");
}

void smp_boot_aps(void) {
    /* Register the boot CPU as cpu 0 and point its GS base at its per-CPU
     * struct, so this_cpu() works on the BSP too and APs claim slots 1, 2, ... */
    cpu_register(apic_is_active() ? lapic_id() : 0);

    if (!apic_is_active()) {
        serial_write("SMP: APIC inactive, not starting APs\n");
        return;
    }

    const struct acpi_info *info = acpi_get();
    if (info->cpu_count <= 1) {
        serial_write("SMP: only one CPU present\n");
        return;
    }

    /* Relocate the trampoline into low memory (identity-mapped, executable). */
    uintptr_t len = (uintptr_t)ap_trampoline_end - (uintptr_t)ap_trampoline_start;
    memcpy((void *)(uintptr_t)TRAMPOLINE_ADDR, ap_trampoline_start, len);

    uint8_t bsp_id  = lapic_id();
    uint8_t sipi_vec = (uint8_t)(TRAMPOLINE_ADDR >> 12);

    for (uint8_t i = 0; i < info->cpu_count; ++i) {
        uint8_t id = info->cpu_apic_ids[i];
        if (id == bsp_id) continue;   /* skip ourselves */

        void *stack = kmalloc(AP_STACK_SIZE);
        if (!stack) {
            serial_write("SMP: out of memory for AP stack\n");
            break;
        }
        ap_stack_top = (uint64_t)(uintptr_t)stack + AP_STACK_SIZE;

        unsigned before = g_cpus_online;

        /* Universal startup: assert INIT, then two STARTUP IPIs (the second is
         * a no-op on CPUs that already started, and covers ones that missed
         * the first). 0x4500 = INIT level-assert; 0x4600|vec = STARTUP. */
        lapic_send_ipi(id, 0x00004500u);
        delay_spin(200000);
        lapic_send_ipi(id, 0x00004600u | sipi_vec);
        delay_spin(40000);
        lapic_send_ipi(id, 0x00004600u | sipi_vec);

        /* Wait (bounded) for the AP to report in. */
        for (int t = 0; t < 200 && g_cpus_online == before; ++t)
            delay_spin(100000);

        if (g_cpus_online > before) {
            serial_write("SMP: AP online apic_id=");
            serial_write_hex_u64(id);
            serial_write("\n");
        } else {
            serial_write("SMP: AP did not start, apic_id=");
            serial_write_hex_u64(id);
            serial_write("\n");
        }
    }

    serial_write("SMP: cpus online=");
    serial_write_hex_u64(g_cpus_online);
    serial_write(" of ");
    serial_write_hex_u64(info->cpu_count);
    serial_write("\n");

    /* Let the APs finish their work burst, then verify the shared counter. */
    delay_spin(2000000);
    smp_report();
}

unsigned smp_cpu_count(void) { return g_cpus_online; }
