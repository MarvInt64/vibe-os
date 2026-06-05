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

#include "apic.h"
#include "acpi.h"
#include "io.h"
#include "serial.h"

/* ---- Local APIC ---------------------------------------------------------- */

#define LAPIC_REG_ID    0x020   /* APIC ID (bits 24-31)                       */
#define LAPIC_REG_EOI   0x0B0   /* end-of-interrupt (write 0)                 */
#define LAPIC_REG_SVR   0x0F0   /* spurious interrupt vector register         */
#define LAPIC_REG_TPR   0x080   /* task priority register                     */
#define LAPIC_REG_ICR_LOW  0x300 /* interrupt command register, low dword     */
#define LAPIC_REG_ICR_HIGH 0x310 /* interrupt command register, high dword    */
#define LAPIC_ICR_DELIVERY_PENDING (1u << 12)
#define LAPIC_REG_LVT_TIMER 0x320 /* local-vector-table entry for the timer    */
#define LAPIC_REG_TIMER_INIT 0x380 /* timer initial count                      */
#define LAPIC_REG_TIMER_DIV  0x3E0 /* timer divide configuration               */
#define LAPIC_TIMER_PERIODIC (1u << 17) /* LVT timer mode bit                  */
#define LAPIC_SVR_ENABLE 0x100  /* SVR bit 8: software-enable the Local APIC  */
#define LAPIC_SPURIOUS_VECTOR 0xFF

#define IA32_APIC_BASE_MSR 0x1B
#define APIC_BASE_GLOBAL_ENABLE (1u << 11)

/* The legacy timer is delivered on IDT vector 0x20 (see interrupts.c). */
#define TIMER_VECTOR 0x20u

/* ---- IOAPIC -------------------------------------------------------------- */

#define IOAPIC_REG_VER       0x01   /* bits 16-23 = max redirection entry     */
#define IOAPIC_REG_REDTBL    0x10   /* first redirection entry (low dword)    */

#define IOREDTBL_MASKED      (1u << 16)
#define IOREDTBL_LEVEL       (1u << 15)
#define IOREDTBL_ACTIVE_LOW  (1u << 13)

static uint32_t g_lapic_base;
static uint32_t g_ioapic_base;
static int      g_apic_active;

/* ---- MMIO + MSR primitives ---------------------------------------------- */

static inline uint32_t mmio_read(uint32_t base, uint32_t reg) {
    return *(volatile uint32_t *)(uintptr_t)(base + reg);
}
static inline void mmio_write(uint32_t base, uint32_t reg, uint32_t val) {
    *(volatile uint32_t *)(uintptr_t)(base + reg) = val;
}

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}
static inline void wrmsr(uint32_t msr, uint64_t value) {
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)value), "d"((uint32_t)(value >> 32)));
}

/* IOAPIC registers are accessed indirectly through an index/data window. */
static uint32_t ioapic_read(uint32_t reg) {
    mmio_write(g_ioapic_base, 0x00, reg);    /* IOREGSEL */
    return mmio_read(g_ioapic_base, 0x10);   /* IOWIN    */
}
static void ioapic_write(uint32_t reg, uint32_t val) {
    mmio_write(g_ioapic_base, 0x00, reg);
    mmio_write(g_ioapic_base, 0x10, val);
}

/* Program one IOAPIC redirection entry (64-bit: two 32-bit registers). */
static void ioapic_set_redirect(uint32_t index, uint32_t low, uint32_t high) {
    ioapic_write(IOAPIC_REG_REDTBL + index * 2 + 1, high);
    ioapic_write(IOAPIC_REG_REDTBL + index * 2,     low);
}

/* ---- public API ---------------------------------------------------------- */

void lapic_eoi(void) {
    if (g_apic_active) mmio_write(g_lapic_base, LAPIC_REG_EOI, 0);
}

uint8_t lapic_id(void) {
    return (uint8_t)(mmio_read(g_lapic_base, LAPIC_REG_ID) >> 24);
}

int apic_is_active(void) { return g_apic_active; }

/* Software-enable the Local APIC of the CPU currently executing. Shared by the
 * BSP (apic_init) and every AP (ap_main): globally enable via the MSR, then set
 * the spurious vector + software-enable bit and accept all interrupt
 * priorities. Relies on g_lapic_base having been discovered by apic_init. */
void lapic_enable_this_cpu(void) {
    wrmsr(IA32_APIC_BASE_MSR, rdmsr(IA32_APIC_BASE_MSR) | APIC_BASE_GLOBAL_ENABLE);
    mmio_write(g_lapic_base, LAPIC_REG_SVR, LAPIC_SVR_ENABLE | LAPIC_SPURIOUS_VECTOR);
    mmio_write(g_lapic_base, LAPIC_REG_TPR, 0);
}

/* Start this CPU's Local APIC timer in periodic mode on the given vector.
 * Divide-by-16 plus a fixed initial count gives a steady, hardware-local tick
 * (exact rate is uncalibrated — fine for driving a per-CPU idle/scheduler). */
void lapic_timer_start(uint8_t vector) {
    mmio_write(g_lapic_base, LAPIC_REG_TIMER_DIV, 0x3);   /* divide by 16 */
    mmio_write(g_lapic_base, LAPIC_REG_LVT_TIMER, (uint32_t)vector | LAPIC_TIMER_PERIODIC);
    mmio_write(g_lapic_base, LAPIC_REG_TIMER_INIT, 2000000u);
}

/* Send an inter-processor interrupt to a specific Local APIC. icr_low carries
 * the delivery mode / vector (e.g. INIT or STARTUP); the destination goes in
 * the high register. Spins until the APIC reports the IPI was delivered. */
void lapic_send_ipi(uint8_t dest_apic_id, uint32_t icr_low) {
    mmio_write(g_lapic_base, LAPIC_REG_ICR_HIGH, (uint32_t)dest_apic_id << 24);
    mmio_write(g_lapic_base, LAPIC_REG_ICR_LOW, icr_low);
    while (mmio_read(g_lapic_base, LAPIC_REG_ICR_LOW) & LAPIC_ICR_DELIVERY_PENDING)
        __asm__ volatile("pause");
}

int apic_init(void) {
    if (acpi_init() != 0) {
        serial_write("APIC: no ACPI/MADT, staying on 8259 PIC\n");
        return -1;
    }

    const struct acpi_info *info = acpi_get();
    g_lapic_base  = info->lapic_base  ? info->lapic_base  : 0xFEE00000u;
    g_ioapic_base = info->ioapic_base;

    /* Without an IOAPIC we cannot route the timer once the PIC is masked, so
     * keep the legacy path rather than silently killing the timer. */
    if (g_ioapic_base == 0) {
        serial_write("APIC: no IOAPIC, staying on 8259 PIC\n");
        return -1;
    }

    /* 1. Software-enable this (boot) CPU's Local APIC; spurious -> vector 0xFF. */
    lapic_enable_this_cpu();

    uint8_t bsp_id = lapic_id();

    /* 2. Mask every IOAPIC input first; we only want the timer enabled. */
    uint32_t max_redir = (ioapic_read(IOAPIC_REG_VER) >> 16) & 0xFF;
    for (uint32_t i = 0; i <= max_redir; ++i)
        ioapic_set_redirect(i, IOREDTBL_MASKED, 0);

    /* 3. Route the legacy timer (ISA IRQ0, often overridden to GSI 2) to the
     *    BSP's Local APIC on the existing timer vector. */
    uint16_t flags = 0;
    uint32_t gsi = acpi_irq_to_gsi(0, &flags);
    uint32_t index = gsi - info->ioapic_gsi_base;

    uint32_t low = TIMER_VECTOR;        /* fixed delivery, physical dest */
    if ((flags & 0x3) == 0x3) low |= IOREDTBL_ACTIVE_LOW;   /* MPS polarity */
    if ((flags & 0xC) == 0xC) low |= IOREDTBL_LEVEL;        /* MPS trigger  */
    ioapic_set_redirect(index, low, (uint32_t)bsp_id << 24);

    /* 4. Mask the 8259 PIC completely — interrupts now arrive via the IOAPIC.
     *    (timer_init() already remapped the PIC vectors out of the exception
     *    range, so a stray PIC line cannot be mistaken for a CPU fault.) */
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);

    g_apic_active = 1;

    serial_write("APIC: active, bsp_id=");
    serial_write_hex_u64(bsp_id);
    serial_write(" timer_gsi=");
    serial_write_hex_u64(gsi);
    serial_write(" redir_index=");
    serial_write_hex_u64(index);
    serial_write("\n");
    return 0;
}
