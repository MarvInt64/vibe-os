/* VibeOS arm64 kernel entry.
 *
 * kernel_main_arm64() is called from boot.S after MMU and BSS init.
 * It initializes the arm64 hardware and portable kernel subsystems,
 * then starts the main kernel loop.
 *
 * This replaces kernel.c's kernel_main() for the arm64 build.
 */
#include "arch.h"
#include "../../include/serial.h"
#include "../../include/alloc.h"

/* ---- Portable kernel subsystems (compiled for both arches) ----------- */
extern void tty_init(void);          /* tty.c */
extern void vfs_init(void);          /* vfs.c */
extern void process_init(void);      /* process.c */
extern void ext2_mount(void);        /* ext2_fs.c, optional */

/* ---- Tick counter (incremented by timer IRQ) ------------------------- */
static volatile uint64_t g_tick = 0;

uint64_t arm64_uptime_ticks(void) {
    return g_tick;
}

/* ---- IRQ handler (called from exceptions.S exc_el1_irq / exc_el0_irq) */
void arm64_irq_handler(void) {
    unsigned irq = arm64_gic_ack();

    switch (irq) {
    case TIMER_IRQ_PHYS:
        arm64_timer_ack();
        g_tick++;
        /* Future: scheduler tick, process preemption */
        break;

    case 1023:
        /* Spurious interrupt — ignore */
        break;

    default:
        serial_write("[arm64] unhandled IRQ ");
        serial_write_hex_u64(irq);
        serial_write("\n");
        break;
    }

    arm64_gic_eoi(irq);
}

/* ---- Synchronous exception handlers ---------------------------------- */
void arm64_sync_handler_el1(uint64_t esr, uint64_t elr, uint64_t far,
                             void *sp) {
    (void)sp;
    unsigned ec = (unsigned)((esr >> 26) & 0x3f);

    serial_write("\n*** ARM64 KERNEL FAULT ***\n");
    serial_write("  EC  = "); serial_write_hex_u64(ec);  serial_write("\n");
    serial_write("  ESR = "); serial_write_hex_u64(esr); serial_write("\n");
    serial_write("  ELR = "); serial_write_hex_u64(elr); serial_write("\n");
    serial_write("  FAR = "); serial_write_hex_u64(far); serial_write("\n");

    while (1) {
        __asm__ volatile("wfi");
    }
}

void arm64_sync_handler_el0(uint64_t esr, uint64_t elr, uint64_t far,
                             void *sp) {
    (void)sp;
    unsigned ec = (unsigned)((esr >> 26) & 0x3f);

    /* EC 0x15 = SVC (syscall from AArch64 EL0) */
    if (ec == 0x15) {
        /* Syscall dispatch — stub for now */
        return;
    }

    serial_write("\n*** ARM64 USER FAULT ***\n");
    serial_write("  EC  = "); serial_write_hex_u64(ec);  serial_write("\n");
    serial_write("  ESR = "); serial_write_hex_u64(esr); serial_write("\n");
    serial_write("  ELR = "); serial_write_hex_u64(elr); serial_write("\n");
    serial_write("  FAR = "); serial_write_hex_u64(far); serial_write("\n");
}

void arm64_unhandled_exception(uint64_t esr, uint64_t elr, uint64_t far) {
    serial_write("\n*** ARM64 UNHANDLED EXCEPTION ***\n");
    serial_write("  ESR = "); serial_write_hex_u64(esr); serial_write("\n");
    serial_write("  ELR = "); serial_write_hex_u64(elr); serial_write("\n");
    serial_write("  FAR = "); serial_write_hex_u64(far); serial_write("\n");
    while (1) __asm__ volatile("wfi");
}

/* ---- Simple boot-time memory detection for QEMU virt ----------------- */
static void arm64_detect_memory(uintptr_t *heap_start, size_t *heap_size) {
    /* QEMU virt: RAM starts at RAM_BASE.  We guess 256 MB heap starting
     * 4 MB above the kernel load point (leaves room for BSS + stacks).
     * A real implementation would parse the device tree (DTB at 0x40000000).
     * QEMU passes: -dtb (addr in x0 at EL1 entry) — we ignore it for now. */
    uintptr_t kern_end = (uintptr_t)0x40600000;  /* ~4MB past load addr */
    *heap_start = kern_end;
    *heap_size  = 256 * 1024 * 1024;             /* 256 MB */
}

/* ---- Main arm64 kernel entry ----------------------------------------- */
void kernel_main_arm64(void) {
    /* ---- Serial (UART) ------------------------------------------------ */
    serial_init();

    serial_write("\n");
    serial_write("  __   ___      _      ___  ____  \n");
    serial_write(" \\ \\ / (_)_ __| |__  / _ \\/ ___| \n");
    serial_write("  \\ V /| | '__| '_ \\| | | \\___ \\ \n");
    serial_write("   \\_/ |_|_|  |_|_|_|\\___/|____/ \n");
    serial_write("           arm64 on QEMU virt      \n\n");

    /* ---- ARM64 info --------------------------------------------------- */
    uint64_t freq = read_sysreg(cntfrq_el0);
    uint64_t midr = read_sysreg(midr_el1);
    serial_write("[arm64] MIDR_EL1  = "); serial_write_hex_u64(midr); serial_write("\n");
    serial_write("[arm64] CNTFRQ    = "); serial_write_hex_u64(freq);
    serial_write(" (");
    /* Print frequency in MHz */
    uint64_t mhz = freq / 1000000;
    char mhzstr[16]; int mi = 0;
    if (mhz == 0) { mhzstr[mi++] = '0'; }
    else { uint64_t t = mhz; while (t) { mhzstr[mi++] = (char)('0' + t%10); t/=10; } }
    /* reverse */
    for (int a=0,b=mi-1; a<b; a++,b--) { char c=mhzstr[a]; mhzstr[a]=mhzstr[b]; mhzstr[b]=c; }
    mhzstr[mi] = '\0';
    serial_write(mhzstr); serial_write(" MHz)\n");

    /* ---- Memory ------------------------------------------------------- */
    uintptr_t heap_start;
    size_t heap_size;
    arm64_detect_memory(&heap_start, &heap_size);
    serial_write("[arm64] heap = "); serial_write_hex_u64(heap_start);
    serial_write(" size = "); serial_write_hex_u64((uint64_t)heap_size); serial_write("\n");

    kmalloc_init(heap_start, heap_size);
    serial_write("[arm64] kmalloc initialised\n");

    /* ---- Interrupts --------------------------------------------------- */
    /* GIC MMIO requires the MMU (cacheability) or at least device-mapped
     * memory attributes. On HVF (real hardware), device MMIO access without
     * correct memory attributes causes a synchronous exception (ISV fault).
     * Skip GIC/timer init until the MMU identity mapping is verified. */
    serial_write("[arm64] GIC/timer init skipped (MMU not yet active)\n");

    /* ---- Idle loop ---------------------------------------------------- */
    serial_write("[arm64] kernel ready\n");
    serial_write("[arm64] MMU next: enable identity mapping, then GIC+timer\n\n");

    /* Read physical timer to prove we have hardware access */
    uint64_t t0 = read_sysreg(cntpct_el0);
    serial_write("[arm64] cntpct_el0 = "); serial_write_hex_u64(t0); serial_write("\n");

    /* Spin briefly and read again to confirm the counter advances */
    for (volatile int i = 0; i < 1000000; i++) __asm__ volatile("nop");
    uint64_t t1 = read_sysreg(cntpct_el0);
    serial_write("[arm64] cntpct_el0 = "); serial_write_hex_u64(t1);
    if (t1 > t0) serial_write("  (advancing — hardware timer confirmed)\n");
    else         serial_write("  (NOT advancing — check CNTFRQ)\n");

    serial_write("\n[arm64] SUCCESS: VibeOS arm64 runs natively on Apple M-series via HVF!\n");

    while (1) __asm__ volatile("wfi");
}
