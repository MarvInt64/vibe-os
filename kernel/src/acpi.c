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

#include "acpi.h"
#include "serial.h"
#include "string.h"

/* The bootloader leaves the whole low 4 GB identity-mapped (see boot.S), so an
 * ACPI physical address can be dereferenced directly as a kernel pointer. */
static const void *phys(uint64_t addr) { return (const void *)(uintptr_t)addr; }

/* ---- on-disk ACPI structures (packed, little-endian) -------------------- */

struct rsdp_v1 {
    char     signature[8];   /* "RSD PTR " */
    uint8_t  checksum;       /* covers the first 20 bytes */
    char     oem_id[6];
    uint8_t  revision;       /* 0 = ACPI 1.0 (RSDT), >=2 = ACPI 2.0 (XSDT) */
    uint32_t rsdt_address;
} __attribute__((packed));

struct rsdp_v2 {
    struct rsdp_v1 v1;
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t  ext_checksum;
    uint8_t  reserved[3];
} __attribute__((packed));

struct sdt_header {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));

struct madt {
    struct sdt_header header;
    uint32_t local_apic_addr;
    uint32_t flags;
    /* variable-length list of interrupt-controller entries follows */
} __attribute__((packed));

/* MADT entry type codes we care about. */
#define MADT_LOCAL_APIC          0
#define MADT_IO_APIC             1
#define MADT_INT_SRC_OVERRIDE    2
#define MADT_LOCAL_APIC_OVERRIDE 5

#define MADT_LAPIC_ENABLED 0x1   /* flags bit 0 of a Processor Local APIC */

static struct acpi_info g_acpi;

/* Sum of `len` bytes must be zero for a valid ACPI table. */
static int checksum_ok(const void *base, uint32_t len) {
    const uint8_t *p = (const uint8_t *)base;
    uint8_t sum = 0;
    for (uint32_t i = 0; i < len; ++i) sum = (uint8_t)(sum + p[i]);
    return sum == 0;
}

/* Scan a physical byte range on 16-byte boundaries for the RSDP signature. */
static const struct rsdp_v1 *scan_for_rsdp(uint64_t start, uint64_t end) {
    for (uint64_t a = start; a < end; a += 16) {
        const struct rsdp_v1 *r = (const struct rsdp_v1 *)phys(a);
        if (memcmp(r->signature, "RSD PTR ", 8) == 0 && checksum_ok(r, 20))
            return r;
    }
    return 0;
}

/* Locate the RSDP: first in the Extended BIOS Data Area, then in the BIOS
 * read-only area 0xE0000-0xFFFFF (both identity-mapped in the low 1 MB). */
static const struct rsdp_v1 *find_rsdp(void) {
    uint16_t ebda_seg = *(const uint16_t *)phys(0x40E);
    uint64_t ebda = (uint64_t)ebda_seg << 4;
    const struct rsdp_v1 *r;
    if (ebda >= 0x400 && ebda < 0xA0000) {
        r = scan_for_rsdp(ebda, ebda + 1024);
        if (r) return r;
    }
    return scan_for_rsdp(0xE0000, 0x100000);
}

/* Record an interrupt source override (ISA IRQ -> GSI remap). */
static void add_iso(uint8_t source_irq, uint32_t gsi, uint16_t flags) {
    if (g_acpi.iso_count >= ACPI_MAX_ISO) return;
    g_acpi.iso[g_acpi.iso_count].source_irq = source_irq;
    g_acpi.iso[g_acpi.iso_count].gsi = gsi;
    g_acpi.iso[g_acpi.iso_count].flags = flags;
    g_acpi.iso_count++;
}

/* Walk the variable-length entry list of a MADT. */
static void parse_madt(const struct madt *m) {
    g_acpi.lapic_base = m->local_apic_addr;

    const uint8_t *p   = (const uint8_t *)m + sizeof(struct madt);
    const uint8_t *end = (const uint8_t *)m + m->header.length;

    while (p + 2 <= end) {
        uint8_t type = p[0];
        uint8_t len  = p[1];
        if (len < 2 || p + len > end) break;   /* malformed: stop */

        switch (type) {
        case MADT_LOCAL_APIC: {
            uint8_t apic_id = p[3];
            uint32_t flags;
            memcpy(&flags, p + 4, 4);
            if ((flags & MADT_LAPIC_ENABLED) && g_acpi.cpu_count < ACPI_MAX_CPUS)
                g_acpi.cpu_apic_ids[g_acpi.cpu_count++] = apic_id;
            break;
        }
        case MADT_IO_APIC: {
            if (g_acpi.ioapic_base == 0) {     /* keep the first IOAPIC only */
                memcpy(&g_acpi.ioapic_base, p + 4, 4);
                memcpy(&g_acpi.ioapic_gsi_base, p + 8, 4);
            }
            break;
        }
        case MADT_INT_SRC_OVERRIDE: {
            uint8_t  source = p[3];
            uint32_t gsi;   memcpy(&gsi, p + 4, 4);
            uint16_t flags; memcpy(&flags, p + 8, 2);
            add_iso(source, gsi, flags);
            break;
        }
        case MADT_LOCAL_APIC_OVERRIDE: {
            uint64_t addr; memcpy(&addr, p + 4, 8);
            g_acpi.lapic_base = (uint32_t)addr;   /* 64-bit LAPIC override */
            break;
        }
        default:
            break;   /* ignore entry types we don't use yet */
        }
        p += len;
    }
    g_acpi.valid = 1;
}

/* Search the RSDT (32-bit pointers) or XSDT (64-bit pointers) for the MADT. */
static const struct madt *find_madt(const struct rsdp_v1 *rsdp) {
    if (rsdp->revision >= 2) {
        const struct rsdp_v2 *r2 = (const struct rsdp_v2 *)rsdp;
        const struct sdt_header *xsdt = (const struct sdt_header *)phys(r2->xsdt_address);
        uint32_t n = (xsdt->length - sizeof(struct sdt_header)) / 8;
        const uint8_t *entries = (const uint8_t *)xsdt + sizeof(struct sdt_header);
        for (uint32_t i = 0; i < n; ++i) {
            uint64_t addr; memcpy(&addr, entries + i * 8, 8);
            const struct sdt_header *h = (const struct sdt_header *)phys(addr);
            if (memcmp(h->signature, "APIC", 4) == 0) return (const struct madt *)h;
        }
    } else {
        const struct sdt_header *rsdt = (const struct sdt_header *)phys(rsdp->rsdt_address);
        uint32_t n = (rsdt->length - sizeof(struct sdt_header)) / 4;
        const uint8_t *entries = (const uint8_t *)rsdt + sizeof(struct sdt_header);
        for (uint32_t i = 0; i < n; ++i) {
            uint32_t addr; memcpy(&addr, entries + i * 4, 4);
            const struct sdt_header *h = (const struct sdt_header *)phys(addr);
            if (memcmp(h->signature, "APIC", 4) == 0) return (const struct madt *)h;
        }
    }
    return 0;
}

int acpi_init(void) {
    memset(&g_acpi, 0, sizeof(g_acpi));

    const struct rsdp_v1 *rsdp = find_rsdp();
    if (!rsdp) {
        serial_write("ACPI: RSDP not found\n");
        return -1;
    }

    const struct madt *m = find_madt(rsdp);
    if (!m) {
        serial_write("ACPI: MADT not found\n");
        return -1;
    }

    parse_madt(m);

    serial_write("ACPI: cpus=");
    serial_write_hex_u64(g_acpi.cpu_count);
    serial_write(" lapic=");
    serial_write_hex_u64(g_acpi.lapic_base);
    serial_write(" ioapic=");
    serial_write_hex_u64(g_acpi.ioapic_base);
    serial_write(" gsi_base=");
    serial_write_hex_u64(g_acpi.ioapic_gsi_base);
    serial_write(" iso=");
    serial_write_hex_u64(g_acpi.iso_count);
    serial_write("\n");
    return 0;
}

const struct acpi_info *acpi_get(void) { return &g_acpi; }

uint32_t acpi_irq_to_gsi(uint8_t irq, uint16_t *flags_out) {
    for (uint8_t i = 0; i < g_acpi.iso_count; ++i) {
        if (g_acpi.iso[i].source_irq == irq) {
            if (flags_out) *flags_out = g_acpi.iso[i].flags;
            return g_acpi.iso[i].gsi;
        }
    }
    if (flags_out) *flags_out = 0;
    return irq;   /* identity mapping when no override exists */
}
