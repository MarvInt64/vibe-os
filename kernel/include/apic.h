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
 * Local APIC + IOAPIC programming (SMP milestone 1, BSP only).
 *
 * apic_init() enumerates the machine via ACPI, software-enables this CPU's
 * Local APIC, and reprograms interrupt routing so the legacy timer is
 * delivered through the IOAPIC to the Local APIC (vector 0x20) instead of the
 * 8259 PIC. If ACPI is unavailable it leaves the PIC path untouched and
 * apic_is_active() stays false, so the kernel keeps working on legacy hardware.
 */
#ifndef VIBEOS_APIC_H
#define VIBEOS_APIC_H

#include "types.h"

/* Enumerate (ACPI), enable the Local APIC, and route the timer via the IOAPIC.
 * Must run with interrupts disabled, after timer_init() has programmed the
 * PIT. Returns 0 if the APIC path was activated, -1 if it fell back to PIC. */
int apic_init(void);

/* True once the system is running on the Local APIC (PIC has been masked). */
int apic_is_active(void);

/* Signal end-of-interrupt to the Local APIC. */
void lapic_eoi(void);

/* This CPU's Local APIC ID. */
uint8_t lapic_id(void);

/* Software-enable the Local APIC of the calling CPU (used by APs at bring-up). */
void lapic_enable_this_cpu(void);

/* Send an IPI (INIT / STARTUP / etc.) to another CPU's Local APIC. */
void lapic_send_ipi(uint8_t dest_apic_id, uint32_t icr_low);

/* Start the calling CPU's Local APIC timer (periodic) on the given vector. */
void lapic_timer_start(uint8_t vector);

#endif
