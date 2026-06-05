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
 * Minimal ACPI discovery for SMP bring-up.
 *
 * Walks RSDP -> RSDT/XSDT -> MADT to learn how many CPUs the machine has, the
 * physical addresses of the Local APIC and IOAPIC, and the ISA-IRQ-to-GSI
 * "interrupt source overrides" needed to route the legacy timer through the
 * IOAPIC. This is read-only enumeration; programming the APICs lives in apic.c.
 */
#ifndef VIBEOS_ACPI_H
#define VIBEOS_ACPI_H

#include "types.h"

#define ACPI_MAX_CPUS 32   /* CPUs we are willing to track */
#define ACPI_MAX_ISO  24   /* interrupt-source-override entries we record */

struct acpi_iso {
    uint8_t  source_irq;   /* legacy ISA IRQ number                  */
    uint32_t gsi;          /* global system interrupt it maps to     */
    uint16_t flags;        /* MPS INTI flags (polarity / trigger)    */
};

struct acpi_info {
    int      valid;             /* non-zero once a MADT was parsed        */
    uint32_t lapic_base;        /* Local APIC MMIO physical base          */
    uint32_t ioapic_base;       /* first IOAPIC MMIO physical base        */
    uint32_t ioapic_gsi_base;   /* GSI the first IOAPIC's inputs start at  */
    uint8_t  cpu_count;         /* number of enabled processor LAPICs     */
    uint8_t  cpu_apic_ids[ACPI_MAX_CPUS];
    uint8_t  iso_count;
    struct acpi_iso iso[ACPI_MAX_ISO];
};

/* Parse the ACPI tables. Returns 0 on success, -1 if no usable MADT is found
 * (in which case the caller should stay on the legacy 8259 PIC path). */
int acpi_init(void);

/* The parsed result (always non-NULL; check ->valid). */
const struct acpi_info *acpi_get(void);

/*
 * Translate a legacy ISA IRQ to its global system interrupt, applying any
 * interrupt source override from the MADT. When an override matches, its MPS
 * INTI flags are returned via flags_out (0 otherwise). Without an override the
 * identity mapping (gsi == irq) is used.
 */
uint32_t acpi_irq_to_gsi(uint8_t irq, uint16_t *flags_out);

#endif
