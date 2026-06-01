#include "interrupts.h"
#include "journal.h"
#include "process.h"
#include "serial.h"
#include "timer.h"

#define TIMER_VECTOR 0x20u

struct descriptor_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} __attribute__((packed));

struct tss64 {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved2;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved3;
    uint16_t reserved4;
    uint16_t iomap_base;
} __attribute__((packed));

extern void syscall_interrupt_stub(void);
extern void timer_interrupt_stub(void);
extern void pagefault_stub(void);
extern void gpfault_stub(void);

/* GP-register snapshot written by pagefault_stub before fault_handler runs.
 * Order: rax, rbx, rcx, rdx, rsi, rdi, rbp (indices 0..6). */
uint64_t g_fault_regs[16];

int fault_handler(uint64_t cr2, uint64_t error_code, uint64_t rip, uint64_t vector, uint64_t cs) {
    serial_write("CPU EXCEPTION vector=");
    serial_write_hex_u64(vector);
    serial_write(" cr2=");
    serial_write_hex_u64(cr2);
    serial_write(" err=");
    serial_write_hex_u64(error_code);
    serial_write(" rip=");
    serial_write_hex_u64(rip);
    serial_write(" cs=");
    serial_write_hex_u64(cs);
    serial_write(" rbx=");
    serial_write_hex_u64(g_fault_regs[1]);
    serial_write(" rbp=");
    serial_write_hex_u64(g_fault_regs[6]);
    serial_write("\n");

    if ((cs & 0x03u) == 0x03u) {
        /* Record rbx/rbp in the persisted journal too (visible via dmesg and
         * /journal.log) so faults can be diagnosed without a serial capture. */
        journal_log_hex(JOURNAL_FAULT, 0, "  fault rbx=", g_fault_regs[1]);
        return process_handle_user_fault(vector, rip, cr2, error_code);
    }
    return 0;
}
extern void interrupt_load_runtime_tables(const struct descriptor_ptr *gdt_desc, const struct descriptor_ptr *idt_desc, uint16_t tss_selector);

static uint64_t g_runtime_gdt[7] __attribute__((aligned(16)));
static struct idt_entry g_idt[256] __attribute__((aligned(16)));
static struct tss64 g_tss;
static uint8_t g_interrupt_stack[16384] __attribute__((aligned(16)));

static void set_idt_gate(uint8_t vector, void (*handler)(void), uint8_t type_attr) {
    uintptr_t address = (uintptr_t)handler;

    g_idt[vector].offset_low = (uint16_t)(address & 0xffffu);
    g_idt[vector].selector = KERNEL_CODE_SELECTOR;
    g_idt[vector].ist = 0;
    g_idt[vector].type_attr = type_attr;
    g_idt[vector].offset_mid = (uint16_t)((address >> 16) & 0xffffu);
    g_idt[vector].offset_high = (uint32_t)((address >> 32) & 0xffffffffu);
    g_idt[vector].reserved = 0;
}

static void set_tss_descriptor(uint64_t *gdt, const struct tss64 *tss) {
    uintptr_t base = (uintptr_t)tss;
    uint32_t limit = (uint32_t)(sizeof(*tss) - 1u);
    uint64_t low = 0;
    uint64_t high = 0;

    low |= (uint64_t)(limit & 0xffffu);
    low |= (uint64_t)(base & 0xffffu) << 16;
    low |= (uint64_t)((base >> 16) & 0xffu) << 32;
    low |= (uint64_t)0x89u << 40;
    low |= (uint64_t)((limit >> 16) & 0x0fu) << 48;
    low |= (uint64_t)((base >> 24) & 0xffu) << 56;
    high = (uint64_t)(base >> 32);

    gdt[5] = low;
    gdt[6] = high;
}

void interrupts_init(void) {
    struct descriptor_ptr gdt_desc;
    struct descriptor_ptr idt_desc;
    size_t i;

    g_runtime_gdt[0] = 0x0000000000000000ull;
    g_runtime_gdt[1] = 0x00af9a000000ffffull;
    g_runtime_gdt[2] = 0x00af92000000ffffull;
    g_runtime_gdt[3] = 0x00aff2000000ffffull;
    g_runtime_gdt[4] = 0x00affa000000ffffull;

    g_tss.reserved0 = 0;
    g_tss.rsp0 = (uintptr_t)(g_interrupt_stack + sizeof(g_interrupt_stack));
    g_tss.rsp1 = 0;
    g_tss.rsp2 = 0;
    g_tss.reserved2 = 0;
    g_tss.ist1 = 0;
    g_tss.ist2 = 0;
    g_tss.ist3 = 0;
    g_tss.ist4 = 0;
    g_tss.ist5 = 0;
    g_tss.ist6 = 0;
    g_tss.ist7 = 0;
    g_tss.reserved3 = 0;
    g_tss.reserved4 = 0;
    g_tss.iomap_base = (uint16_t)sizeof(g_tss);

    set_tss_descriptor(g_runtime_gdt, &g_tss);

    for (i = 0; i < 256u; ++i) {
        g_idt[i].offset_low = 0;
        g_idt[i].selector = 0;
        g_idt[i].ist = 0;
        g_idt[i].type_attr = 0;
        g_idt[i].offset_mid = 0;
        g_idt[i].offset_high = 0;
        g_idt[i].reserved = 0;
    }

    set_idt_gate(TIMER_VECTOR, timer_interrupt_stub, 0x8eu);
    set_idt_gate(0x80u, syscall_interrupt_stub, 0xeeu);
    set_idt_gate(13u, gpfault_stub, 0x8eu);    /* #GP */
    set_idt_gate(14u, pagefault_stub, 0x8eu);  /* #PF */

    gdt_desc.limit = (uint16_t)(sizeof(g_runtime_gdt) - 1u);
    gdt_desc.base = (uint64_t)(uintptr_t)g_runtime_gdt;
    idt_desc.limit = (uint16_t)(sizeof(g_idt) - 1u);
    idt_desc.base = (uint64_t)(uintptr_t)g_idt;

    interrupt_load_runtime_tables(&gdt_desc, &idt_desc, TSS_SELECTOR);
    timer_init();
}

/* Point the TSS ring-0 stack (rsp0) at the kernel stack of the process that is
 * about to run. The CPU loads rsp0 on every privilege transition (syscall /
 * IRQ / fault from user mode), so each process handles its traps on its own
 * kernel stack. This is what lets a process be suspended in the middle of a
 * blocking syscall without another process clobbering its kernel frames. */
void interrupt_set_kernel_stack(uintptr_t rsp0_top) {
    g_tss.rsp0 = rsp0_top;
}
